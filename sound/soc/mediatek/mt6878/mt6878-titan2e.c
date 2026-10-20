// SPDX-License-Identifier: GPL-2.0
/*
 * Unihertz Titan 2 Elite sound card: MT6878 AFE + MT6369 codec.
 * The FS1815 speaker amp takes MT6369 LINEOUT L as its analog input.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "mt6878-afe-common.h"
#include "mt6878-reg.h"

#define CODEC_MT6369_NAME "mtk-codec-mt6369"

void mt6369_set_mtkaif_protocol(struct snd_soc_component *cmpnt, int protocol);
int frsm_i2ca_spk_switch(int spkid, bool on);
int frsm_i2ca_set_volume(int volume);
int frsm_i2ca_get_volume(void);
void mt6369_mtkaif_calibration_enable(struct snd_soc_component *cmpnt);
void mt6369_mtkaif_calibration_disable(struct snd_soc_component *cmpnt);
void mt6369_set_mtkaif_calibration_phase(struct snd_soc_component *cmpnt,
					 int phase_1, int phase_2, int phase_3);

/* topckgen audio test block used for the MTKAIF phase sweep */
#define TOPCKGEN_BASE		0x10000000
#define CKSYS_AUD_TOP_CFG	0x032c
#define CKSYS_AUD_TOP_MON	0x0330
#define MTKAIF_NUM_PHASE	42
#define MT6369_MTKAIF_PROTOCOL_2_CLK_P2 2

struct titan_priv {
	struct gpio_desc *spk_en;
};

static int titan_spk_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct titan_priv *priv = snd_soc_card_get_drvdata(card);
	bool on = SND_SOC_DAPM_EVENT_ON(event);
	int (*spk_switch)(int spkid, bool on);

	if (priv->spk_en)
		gpiod_set_value_cansleep(priv->spk_en, on);

	/* amp driver is optional; the single amp is spkr-id 1 */
	spk_switch = symbol_get(frsm_i2ca_spk_switch);
	if (spk_switch) {
		int ret = spk_switch(1, on);

		if (ret)
			dev_warn(card->dev, "speaker amp %s: %d\n",
				 on ? "on" : "off", ret);
		symbol_put(frsm_i2ca_spk_switch);
	}
	return 0;
}

static const struct snd_soc_dapm_widget titan_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", titan_spk_event),
	SND_SOC_DAPM_SPK("Earpiece", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Second Mic", NULL),
};

static const struct snd_soc_dapm_route titan_routes[] = {
	{"Speaker", NULL, "I2SOUT4"},
	{"Earpiece", NULL, "Receiver"},
	{"AIN0", NULL, "Main Mic"},
	{"AIN2", NULL, "Second Mic"},
};

/* FS19xx digital volume: 0x3FF = 0 dB, 0.09375 dB per step */
#define TITAN_AMP_VOL_MAX	0x3FF
static const DECLARE_TLV_DB_SCALE(titan_amp_vol_tlv, -9591, 9, 0);

static int titan_amp_vol_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	int (*get)(void) = symbol_get(frsm_i2ca_get_volume);

	ucontrol->value.integer.value[0] = get ? get() : 0;
	if (get)
		symbol_put(frsm_i2ca_get_volume);
	return 0;
}

static int titan_amp_vol_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	int (*set)(int volume) = symbol_get(frsm_i2ca_set_volume);
	long v = ucontrol->value.integer.value[0];

	if (!set)
		return -ENODEV;
	if (v < 0 || v > TITAN_AMP_VOL_MAX) {
		symbol_put(frsm_i2ca_set_volume);
		return -EINVAL;
	}
	set(v);
	symbol_put(frsm_i2ca_set_volume);
	return 1;
}

static const struct snd_kcontrol_new titan_controls[] = {
	SOC_SINGLE_EXT_TLV("Speaker Amp Volume", SND_SOC_NOPM, 0,
			   TITAN_AMP_VOL_MAX, 0, titan_amp_vol_get,
			   titan_amp_vol_put, titan_amp_vol_tlv),
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Earpiece"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
	SOC_DAPM_PIN_SWITCH("Second Mic"),
};

static void titan_topck_update(void __iomem *top, u32 mask, u32 val)
{
	u32 v = readl(top + CKSYS_AUD_TOP_CFG);

	writel((v & ~mask) | (val & mask), top + CKSYS_AUD_TOP_CFG);
}

/*
 * MTKAIF RX phase calibration (vendor sequence): sweep the MT6369 TX phase
 * and take the last phase before the measured delay cycle changes.
 */
static void titan_mtkaif_calibration(struct snd_soc_component *afe_cmpnt,
				     struct snd_soc_component *codec)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(afe_cmpnt);
	int chosen[2] = { -1, -1 }, cycle[2], prev[2] = { -1, -1 };
	void __iomem *top;
	int phase, counter, done[2];
	bool ok = true;
	u32 mon = 0;

	top = ioremap(TOPCKGEN_BASE, 0x1000);
	if (!top)
		return;

	pm_runtime_get_sync(afe->dev);
	mt6369_mtkaif_calibration_enable(codec);

	/* clock protocol 2 */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP_CFG0, 0xff, 0xb8);
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP_CFG0, 0xff, 0xb9);

	/* test type: synchronizer pulse */
	titan_topck_update(top, 0xffff, 0x4);

	for (phase = 0; phase <= MTKAIF_NUM_PHASE && ok; phase++) {
		mt6369_set_mtkaif_calibration_phase(codec, phase, phase, phase);
		titan_topck_update(top, 0x1, 0x1);

		done[0] = done[1] = 0;
		cycle[0] = cycle[1] = -1;
		counter = 0;
		while (!done[0] || !done[1]) {
			mon = readl(top + CKSYS_AUD_TOP_MON);
			if (!done[0] && ((mon >> 28) & 0x1)) {
				done[0] = 1;
				cycle[0] = mon & 0xf;
			}
			if (!done[1] && ((mon >> 29) & 0x1)) {
				done[1] = 1;
				cycle[1] = (mon >> 4) & 0xf;
			}
			if (++counter > 10000) {
				dev_warn(afe->dev, "mtkaif calibration timeout, mon 0x%x\n",
					 mon);
				ok = false;
				break;
			}
		}

		if (phase == 0) {
			prev[0] = cycle[0];
			prev[1] = cycle[1];
		}
		if (cycle[0] != prev[0] && chosen[0] < 0)
			chosen[0] = phase - 1;
		if (cycle[1] != prev[1] && chosen[1] < 0)
			chosen[1] = phase - 1;

		titan_topck_update(top, 0x1, 0x0);
	}

	mt6369_set_mtkaif_calibration_phase(codec,
					    chosen[0] < 0 ? 0 : chosen[0],
					    chosen[1] < 0 ? 0 : chosen[1], 0);

	/* disable rx fifo */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP_CFG0, 0xff, 0xb8);
	mt6369_mtkaif_calibration_disable(codec);
	pm_runtime_put(afe->dev);
	iounmap(top);

	dev_info(afe->dev, "mtkaif calibration %s, phase %d/%d, cycle %d/%d\n",
		 ok ? "done" : "failed", chosen[0], chosen[1], prev[0], prev[1]);
}

static bool calibrate = true;
module_param(calibrate, bool, 0444);

static int titan_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *codec =
		snd_soc_rtdcom_lookup(rtd, CODEC_MT6369_NAME);
	struct snd_soc_component *afe = snd_soc_rtd_to_cpu(rtd, 0)->component;

	if (!codec)
		return 0;

	mt6369_set_mtkaif_protocol(codec, MT6369_MTKAIF_PROTOCOL_2_CLK_P2);
	if (calibrate && afe)
		titan_mtkaif_calibration(afe, codec);
	return 0;
}

SND_SOC_DAILINK_DEFS(playback0,
	DAILINK_COMP_ARRAY(COMP_CPU("DL0")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(playback1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(capture0,
	DAILINK_COMP_ARRAY(COMP_CPU("UL0")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_speech,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless Speech DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(modem_pcm,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(spk_i2s,
	DAILINK_COMP_ARRAY(COMP_CPU("I2SOUT4")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* the FS18xx amp is set up for 48 kHz, 64 fs bit clock */
static int titan_spk_fixup(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_hw_params *params)
{
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min = 48000;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->max = 48000;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->min = 2;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max = 2;
	snd_mask_reset_range(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			     0, (__force unsigned int)SNDRV_PCM_FORMAT_LAST);
	params_set_format(params, SNDRV_PCM_FORMAT_S32_LE);
	return 0;
}

SND_SOC_DAILINK_DEFS(primary_codec,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "mt6369-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link titan_dai_links[] = {
	{
		.name = "Playback_0",
		.stream_name = "Playback_0",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback0),
	},
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback1),
	},
	{
		.name = "Capture_0",
		.stream_name = "Capture_0",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture0),
	},
	{
		/* opened for the duration of a call; moves no data */
		.name = "Hostless_Speech",
		.stream_name = "Hostless_Speech",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_speech),
	},
	{
		.name = "Speaker Amp",
		.no_pcm = 1,
		.playback_only = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = titan_spk_fixup,
		SND_SOC_DAILINK_REG(spk_i2s),
	},
	{
		.name = "Modem PCM",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(modem_pcm),
	},
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		.init = titan_codec_init,
		SND_SOC_DAILINK_REG(primary_codec),
	},
};

static struct snd_soc_card titan_card = {
	.name = "Titan2Elite",
	.owner = THIS_MODULE,
	.dai_link = titan_dai_links,
	.num_links = ARRAY_SIZE(titan_dai_links),
	.dapm_widgets = titan_widgets,
	.num_dapm_widgets = ARRAY_SIZE(titan_widgets),
	.dapm_routes = titan_routes,
	.num_dapm_routes = ARRAY_SIZE(titan_routes),
	.controls = titan_controls,
	.num_controls = ARRAY_SIZE(titan_controls),
};

static int titan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card = &titan_card;
	struct device_node *platform, *codec;
	struct snd_soc_dai_link *link;
	struct titan_priv *priv;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spk_en = devm_gpiod_get_optional(dev, "spk-en", GPIOD_OUT_LOW);
	if (IS_ERR(priv->spk_en))
		return dev_err_probe(dev, PTR_ERR(priv->spk_en), "spk-en\n");

	platform = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform)
		return dev_err_probe(dev, -EINVAL, "no platform\n");
	codec = of_parse_phandle(dev->of_node, "mediatek,audio-codec", 0);
	if (!codec) {
		of_node_put(platform);
		return dev_err_probe(dev, -EINVAL, "no codec\n");
	}

	for_each_card_prelinks(card, i, link) {
		link->platforms->of_node = platform;
		link->cpus->of_node = platform;
		if (link->no_pcm && link->codecs->dai_name &&
		    !strcmp(link->codecs->dai_name, "mt6369-snd-codec-aif1"))
			link->codecs->of_node = codec;
	}

	card->dev = dev;
	snd_soc_card_set_drvdata(card, priv);
	of_property_read_string(dev->of_node, "model", &card->name);

	ret = devm_snd_soc_register_card(dev, card);
	of_node_put(codec);
	of_node_put(platform);
	return ret;
}

static const struct of_device_id titan_dt_match[] = {
	{ .compatible = "unihertz,titan2e-sound", },
	{}
};
MODULE_DEVICE_TABLE(of, titan_dt_match);

static struct platform_driver titan_driver = {
	.driver = {
		.name = "titan2e-sound",
		.of_match_table = titan_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = titan_probe,
};
module_platform_driver(titan_driver);

MODULE_DESCRIPTION("Unihertz Titan 2 Elite sound card");
MODULE_LICENSE("GPL");
