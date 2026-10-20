// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6878 AFE PCM interface to the internal modem (speech)
 *
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/regmap.h>
#include <sound/pcm_params.h>

#include "mt6878-afe-common.h"
#include "mt6878-interconnection.h"
#include "mt6878-reg.h"

enum {
	PCM_MODE_8K = 0,
	PCM_MODE_16K = 1,
	PCM_MODE_32K = 2,
	PCM_MODE_48K = 3,
};

enum {
	PCM_1X_8K = 0,
	PCM_1X_16K = 4,
	PCM_1X_32K = 8,
	PCM_1X_48K = 10,
};

#define PCM_FMT_PCM_MODE_B	3
#define PCM_AFIFO_AFIFO		1
#define PCM_1X_EN_HOPPING_26M	0

static unsigned int pcm_mode(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return PCM_MODE_8K;
	case 16000:
		return PCM_MODE_16K;
	case 48000:
		return PCM_MODE_48K;
	default:
		return PCM_MODE_32K;
	}
}

static unsigned int pcm_1x_mode(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return PCM_1X_8K;
	case 16000:
		return PCM_1X_16K;
	case 48000:
		return PCM_1X_48K;
	default:
		return PCM_1X_32K;
	}
}

static const struct snd_kcontrol_new mtk_pcm_1_playback_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN102_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN102_0,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new mtk_pcm_1_playback_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN103_0,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN103_0,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mtk_dai_pcm_widgets[] = {
	SND_SOC_DAPM_MIXER("PCM_1_PB_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_pcm_1_playback_ch1_mix,
			   ARRAY_SIZE(mtk_pcm_1_playback_ch1_mix)),
	SND_SOC_DAPM_MIXER("PCM_1_PB_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_pcm_1_playback_ch2_mix,
			   ARRAY_SIZE(mtk_pcm_1_playback_ch2_mix)),
	SND_SOC_DAPM_SUPPLY("PCM_1_EN", AFE_PCM1_INTF_CON0, PCM1_EN_SFT, 0,
			    NULL, 0),
	SND_SOC_DAPM_INPUT("MD1_TO_AFE"),
	SND_SOC_DAPM_OUTPUT("AFE_TO_MD1"),
};

static const struct snd_soc_dapm_route mtk_dai_pcm_routes[] = {
	{"PCM 1 Playback", NULL, "PCM_1_PB_CH1"},
	{"PCM 1 Playback", NULL, "PCM_1_PB_CH2"},
	{"PCM 1 Playback", NULL, "PCM_1_EN"},
	{"PCM 1 Capture", NULL, "PCM_1_EN"},
	{"AFE_TO_MD1", NULL, "PCM 1 Playback"},
	{"PCM 1 Capture", NULL, "MD1_TO_AFE"},

	/* uplink: microphone to the modem */
	{"PCM_1_PB_CH1", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"PCM_1_PB_CH1", "ADDA_UL_CH2", "ADDA_UL_Mux"},
	{"PCM_1_PB_CH2", "ADDA_UL_CH1", "ADDA_UL_Mux"},
	{"PCM_1_PB_CH2", "ADDA_UL_CH2", "ADDA_UL_Mux"},

	/* downlink: modem to the codec DAC and the speaker amp */
	{"ADDA_DL_CH1", "PCM_1_CAP_CH1", "PCM 1 Capture"},
	{"ADDA_DL_CH2", "PCM_1_CAP_CH1", "PCM 1 Capture"},
	{"I2SOUT4_CH1", "PCM_1_CAP_CH1", "PCM 1 Capture"},
	{"I2SOUT4_CH2", "PCM_1_CAP_CH1", "PCM 1 Capture"},
};

static int mtk_dai_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int rate = params_rate(params);
	unsigned int con0 = 0, con1 = 0;

	/* the interface is shared by both directions */
	if (snd_soc_dai_stream_active(dai, SNDRV_PCM_STREAM_PLAYBACK) +
	    snd_soc_dai_stream_active(dai, SNDRV_PCM_STREAM_CAPTURE) > 1)
		return 0;

	con0 |= PCM_AFIFO_AFIFO << PCM1_AFIFO_SFT;
	con0 |= pcm_mode(rate) << PCM1_MODE_SFT;
	con0 |= PCM_FMT_PCM_MODE_B << PCM1_FMT_SFT;
	regmap_update_bits(afe->regmap, AFE_PCM1_INTF_CON0, 0xfffffffe, con0);

	con1 |= PCM_1X_EN_HOPPING_26M << PCM1_1X_EN_DOMAIN_SFT;
	con1 |= pcm_1x_mode(rate) << PCM1_1X_EN_MODE_SFT;
	regmap_update_bits(afe->regmap, AFE_PCM1_INTF_CON1, 0x3fc0000, con1);

	dev_dbg(afe->dev, "%s() rate %u con0 0x%x con1 0x%x\n",
		__func__, rate, con0, con1);
	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_pcm_ops = {
	.hw_params = mtk_dai_pcm_hw_params,
};

#define MTK_PCM_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |\
		       SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000)
#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_driver mtk_dai_pcm_driver[] = {
	{
		.name = "PCM 1",
		.id = MT6878_DAI_PCM_1,
		.playback = {
			.stream_name = "PCM 1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.capture = {
			.stream_name = "PCM 1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_dai_pcm_ops,
		.symmetric_rate = 1,
		.symmetric_sample_bits = 1,
	},
};

int mt6878_dai_pcm_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_pcm_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_pcm_driver);
	dai->dapm_widgets = mtk_dai_pcm_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_pcm_widgets);
	dai->dapm_routes = mtk_dai_pcm_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_pcm_routes);
	return 0;
}
