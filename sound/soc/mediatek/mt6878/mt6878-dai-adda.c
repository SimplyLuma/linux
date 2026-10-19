// SPDX-License-Identifier: GPL-2.0
/*
 *  MediaTek ALSA SoC Audio DAI ADDA Control
 *
 *  Copyright (c) 2025 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/delay.h>
#include <linux/regmap.h>

#include "mt6878-afe-clk.h"
#include "mt6878-afe-common.h"
#include "mt6878-interconnection.h"

enum {
	UL_IIR_SW,
	UL_IIR_5HZ,
	UL_IIR_10HZ,
	UL_IIR_25HZ,
	UL_IIR_50HZ,
	UL_IIR_75HZ,
};

enum {
	MTK_AFE_ADDA_UL_RATE_8K,
	MTK_AFE_ADDA_UL_RATE_16K,
	MTK_AFE_ADDA_UL_RATE_32K,
	MTK_AFE_ADDA_UL_RATE_48K,
	MTK_AFE_ADDA_UL_RATE_96K,
	MTK_AFE_ADDA_UL_RATE_192K,
	MTK_AFE_ADDA_UL_RATE_48K_HD,
};


struct mtk_afe_adda_priv {
	int dl_rate;
	int ul_rate;
};

static unsigned int adda_ul_rate_transform(struct mtk_base_afe *afe,
					   unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_ADDA_UL_RATE_8K;
	case 16000:
		return MTK_AFE_ADDA_UL_RATE_16K;
	case 32000:
		return MTK_AFE_ADDA_UL_RATE_32K;
	case 48000:
		return MTK_AFE_ADDA_UL_RATE_48K;
	case 96000:
		return MTK_AFE_ADDA_UL_RATE_96K;
	case 192000:
		return MTK_AFE_ADDA_UL_RATE_192K;
	default:
		dev_warn(afe->dev, "rate %d invalid, use 48kHz!!!\n", rate);
		return MTK_AFE_ADDA_UL_RATE_48K;
	}
}



/* ADDA downlink, as in the MT6878 vendor driver (MTKAIF 2.0, no v4 TX) */
enum {
	MTK_AFE_ADDA_DL_RATE_8K = 0,
	MTK_AFE_ADDA_DL_RATE_11K = 1,
	MTK_AFE_ADDA_DL_RATE_12K = 2,
	MTK_AFE_ADDA_DL_RATE_16K = 4,
	MTK_AFE_ADDA_DL_RATE_22K = 5,
	MTK_AFE_ADDA_DL_RATE_24K = 6,
	MTK_AFE_ADDA_DL_RATE_32K = 8,
	MTK_AFE_ADDA_DL_RATE_44K = 9,
	MTK_AFE_ADDA_DL_RATE_48K = 10,
	MTK_AFE_ADDA_DL_RATE_96K = 14,
	MTK_AFE_ADDA_DL_RATE_192K = 18,
};

#define AUDIO_SDM_LEVEL_NORMAL		0x1d
#define AUDIO_SDM_2ND			0
#define SDM_AUTO_RESET_THRESHOLD	0x190000
#define MTK_AFE_ADDA_DL_GAIN_NORMAL_VAL	0xf74f

/* AUDIO_TOP_CON0 clock gates (bit set = gated) */
#define AFE_CG_DL0_DAC_TML		7
#define AFE_CG_DL0_DAC			9
#define AFE_CG_DL0_PREDIS		10

static unsigned int adda_dl_rate_transform(struct mtk_base_afe *afe,
					   unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_ADDA_DL_RATE_8K;
	case 11025:
		return MTK_AFE_ADDA_DL_RATE_11K;
	case 12000:
		return MTK_AFE_ADDA_DL_RATE_12K;
	case 16000:
		return MTK_AFE_ADDA_DL_RATE_16K;
	case 22050:
		return MTK_AFE_ADDA_DL_RATE_22K;
	case 24000:
		return MTK_AFE_ADDA_DL_RATE_24K;
	case 32000:
		return MTK_AFE_ADDA_DL_RATE_32K;
	case 44100:
		return MTK_AFE_ADDA_DL_RATE_44K;
	case 48000:
		return MTK_AFE_ADDA_DL_RATE_48K;
	case 96000:
		return MTK_AFE_ADDA_DL_RATE_96K;
	case 192000:
		return MTK_AFE_ADDA_DL_RATE_192K;
	default:
		dev_warn(afe->dev, "dl rate %d invalid, use 48kHz\n", rate);
		return MTK_AFE_ADDA_DL_RATE_48K;
	}
}

static const struct snd_kcontrol_new mtk_adda_dl_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN014_1, I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN014_1, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN014_1, I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN014_1, I_DL3_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH1", AFE_CONN014_1, I_DL4_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL5_CH1", AFE_CONN014_1, I_DL5_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH1", AFE_CONN014_1, I_DL6_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH1", AFE_CONN014_1, I_DL7_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL8_CH1", AFE_CONN014_1, I_DL8_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH1", AFE_CONN014_1, I_DL_24CH_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL24_CH1", AFE_CONN014_2, I_DL24_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN014_0, I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new mtk_adda_dl_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH2", AFE_CONN015_1, I_DL0_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL0_CH1", AFE_CONN015_1, I_DL0_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN015_1, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN015_1, I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN015_1, I_DL3_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL4_CH2", AFE_CONN015_1, I_DL4_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL5_CH2", AFE_CONN015_1, I_DL5_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL6_CH2", AFE_CONN015_1, I_DL6_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL7_CH2", AFE_CONN015_1, I_DL7_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL8_CH2", AFE_CONN015_1, I_DL8_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL_24CH_CH2", AFE_CONN015_1, I_DL_24CH_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL24_CH2", AFE_CONN015_2, I_DL24_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN015_0, I_ADDA_UL_CH2, 1, 0),
};

#define AFE_CG_UL0_TML			22
#define AFE_CG_UL0_ADC			23

static int mtk_adda_mtkaif_cfg_event(struct snd_soc_dapm_widget *w,
				     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);

	if (event != SND_SOC_DAPM_PRE_PMU)
		return 0;

	/* MTKAIF protocol 2, clock P2; no phase calibration yet */
	regmap_write(afe->regmap, AFE_MTKAIF0_CFG0, 0x00010000);
	regmap_write(afe->regmap, AFE_MTKAIF1_CFG0, 0x00010000);
	regmap_update_bits(afe->regmap, AFE_MTKAIF0_CFG0,
			   RG_MTKAIF0_RXIF_CLKINV_MASK_SFT,
			   0x1 << RG_MTKAIF0_RXIF_CLKINV_SFT);
	regmap_update_bits(afe->regmap, AFE_MTKAIF1_CFG0,
			   RG_MTKAIF1_RXIF_CLKINV_ADC_MASK_SFT,
			   0x1 << RG_MTKAIF1_RXIF_CLKINV_ADC_SFT);
	return 0;
}

static int mtk_adda_pad_top_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);

	/* MTKAIF protocol 2, clock P2 (MT6369) */
	if (event == SND_SOC_DAPM_PRE_PMU)
		regmap_write(afe->regmap, AFE_AUD_PAD_TOP_CFG0, 0xb8);
	return 0;
}

static int mtk_adda_dl_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	/* should delay 1/fs (smallest is 8k) = 125us before afe off */
	if (event == SND_SOC_DAPM_POST_PMD)
		usleep_range(125, 150);
	return 0;
}

enum {
	SUPPLY_SEQ_ADDA_CG,
	SUPPLY_SEQ_ADDA_AUD_PAD_TOP,
	SUPPLY_SEQ_ADDA_MTKAIF_CFG,
	SUPPLY_SEQ_ADDA_AFE_ON,
	SUPPLY_SEQ_ADDA_FIFO,
	SUPPLY_SEQ_ADDA_AP_DMIC,
	SUPPLY_SEQ_ADDA_UL_ON,
	SUPPLY_SEQ_ADDA_DL_ON,
};

static int mtk_adda_sleep_on_pmd_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *cmpnt = snd_soc_dapm_to_component(w->dapm);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt);

	dev_dbg(afe->dev, "name %s, event 0x%x\n", w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		break;
	case SND_SOC_DAPM_POST_PMD:
		/* should delayed 1/fs(smallest is 8k) = 125us before afe off */
		usleep_range(120, 130);
		break;
	default:
		break;
	}

	return 0;
}

/* ADDA UL MUX */
#define ADDA_UL_MUX_MASK 0x3
enum {
	ADDA_UL_MUX_MTKAIF = 0,
};

static const char *const adda_ul_mux_map[] = {
	"MTKAIF",
};

static int adda_ul_map_value[] = {
	ADDA_UL_MUX_MTKAIF,
};

static SOC_VALUE_ENUM_SINGLE_DECL(adda_ul_mux_map_enum,
				  SND_SOC_NOPM,
				  0,
				  ADDA_UL_MUX_MASK,
				  adda_ul_mux_map,
				  adda_ul_map_value);

static const struct snd_kcontrol_new adda_ul_mux_control =
	SOC_DAPM_ENUM("ADDA_UL_MUX Select", adda_ul_mux_map_enum);

static const struct snd_kcontrol_new adda_ch34_ul_mux_control =
	SOC_DAPM_ENUM("ADDA_CH34_UL_MUX Select", adda_ul_mux_map_enum);

static const struct snd_soc_dapm_widget mtk_dai_adda_widgets[] = {
	/* inter-connections */
	SND_SOC_DAPM_MIXER("ADDA_DL_CH1", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch1_mix, ARRAY_SIZE(mtk_adda_dl_ch1_mix)),
	SND_SOC_DAPM_MIXER("ADDA_DL_CH2", SND_SOC_NOPM, 0, 0,
			   mtk_adda_dl_ch2_mix, ARRAY_SIZE(mtk_adda_dl_ch2_mix)),
	SND_SOC_DAPM_SUPPLY_S("ADDA Playback Enable", SUPPLY_SEQ_ADDA_DL_ON,
			      AFE_ADDA_DL_SRC_CON0,
			      AFE_DL_SRC_ON_TMP_CTL_PRE_SFT, 0,
			      mtk_adda_dl_event, SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("AUD_PAD_TOP", SUPPLY_SEQ_ADDA_AUD_PAD_TOP,
			      AFE_AUD_PAD_TOP_CFG0, RG_RX_FIFO_ON_SFT, 0,
			      mtk_adda_pad_top_event, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_SUPPLY_S("ADDA_MTKAIF_CFG", SUPPLY_SEQ_ADDA_MTKAIF_CFG,
			      SND_SOC_NOPM, 0, 0,
			      mtk_adda_mtkaif_cfg_event, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_SUPPLY_S("aud_ul0_adc_clk", SUPPLY_SEQ_ADDA_CG,
			      AUDIO_TOP_CON1, AFE_CG_UL0_ADC, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_ul0_tml_clk", SUPPLY_SEQ_ADDA_CG,
			      AUDIO_TOP_CON1, AFE_CG_UL0_TML, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_dl0_dac_clk", SUPPLY_SEQ_ADDA_CG,
			      AUDIO_TOP_CON0, AFE_CG_DL0_DAC, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_dl0_dac_tml_clk", SUPPLY_SEQ_ADDA_CG,
			      AUDIO_TOP_CON0, AFE_CG_DL0_DAC_TML, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("aud_dl0_predis_clk", SUPPLY_SEQ_ADDA_CG,
			      AUDIO_TOP_CON0, AFE_CG_DL0_PREDIS, 1, NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADDA Enable", SUPPLY_SEQ_ADDA_AFE_ON,
			      AUDIO_ENGEN_CON0, AUDIO_F3P25M_EN_ON_SFT, 0,
			      NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADDA Capture Enable", SUPPLY_SEQ_ADDA_UL_ON,
			      AFE_ADDA_UL0_SRC_CON0,
			      UL_SRC_ON_TMP_CTL_SFT, 0,
			      mtk_adda_sleep_on_pmd_event,
			      SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("ADDA CH34 Capture Enable", SUPPLY_SEQ_ADDA_UL_ON,
			      AFE_ADDA_UL1_SRC_CON0,
			      UL_SRC_ON_TMP_CTL_SFT, 0,
			      mtk_adda_sleep_on_pmd_event,
			      SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY_S("ADDA_FIFO", SUPPLY_SEQ_ADDA_FIFO,
			      AFE_ADDA_UL0_SRC_CON1,
			      FIFO_SOFT_RST_SFT, 1,
			      NULL, 0),
	SND_SOC_DAPM_SUPPLY_S("ADDA_CH34_FIFO", SUPPLY_SEQ_ADDA_FIFO,
			      AFE_ADDA_UL1_SRC_CON1,
			      FIFO_SOFT_RST_SFT, 1,
			      NULL, 0),
	SND_SOC_DAPM_MUX("ADDA_UL_Mux", SND_SOC_NOPM, 0, 0,
			 &adda_ul_mux_control),
	SND_SOC_DAPM_MUX("ADDA_CH34_UL_Mux", SND_SOC_NOPM, 0, 0,
			 &adda_ch34_ul_mux_control),
};

static const struct snd_soc_dapm_route mtk_dai_adda_routes[] = {
	/* playback */
	{"ADDA_DL_CH1", "DL0_CH1", "DL0"},
	{"ADDA_DL_CH2", "DL0_CH2", "DL0"},
	{"ADDA_DL_CH1", "DL1_CH1", "DL1"},
	{"ADDA_DL_CH2", "DL1_CH2", "DL1"},
	{"ADDA_DL_CH1", "DL2_CH1", "DL2"},
	{"ADDA_DL_CH2", "DL2_CH2", "DL2"},
	{"ADDA_DL_CH1", "DL3_CH1", "DL3"},
	{"ADDA_DL_CH2", "DL3_CH2", "DL3"},
	{"ADDA_DL_CH1", "DL4_CH1", "DL4"},
	{"ADDA_DL_CH2", "DL4_CH2", "DL4"},
	{"ADDA_DL_CH1", "DL5_CH1", "DL5"},
	{"ADDA_DL_CH2", "DL5_CH2", "DL5"},
	{"ADDA_DL_CH1", "DL6_CH1", "DL6"},
	{"ADDA_DL_CH2", "DL6_CH2", "DL6"},
	{"ADDA_DL_CH1", "DL7_CH1", "DL7"},
	{"ADDA_DL_CH2", "DL7_CH2", "DL7"},
	{"ADDA_DL_CH1", "DL8_CH1", "DL8"},
	{"ADDA_DL_CH2", "DL8_CH2", "DL8"},
	{"ADDA_DL_CH1", "DL_24CH_CH1", "DL_24CH"},
	{"ADDA_DL_CH2", "DL_24CH_CH2", "DL_24CH"},
	{"ADDA_DL_CH1", "DL24_CH1", "DL24"},
	{"ADDA_DL_CH2", "DL24_CH2", "DL24"},
	{"ADDA Playback", NULL, "ADDA_DL_CH1"},
	{"ADDA Playback", NULL, "ADDA_DL_CH2"},
	{"ADDA Playback", NULL, "ADDA Enable"},
	{"ADDA Playback", NULL, "ADDA Playback Enable"},
	{"ADDA Playback", NULL, "AUD_PAD_TOP"},
	{"ADDA Playback", NULL, "aud_dl0_dac_clk"},
	{"ADDA Playback", NULL, "aud_dl0_dac_tml_clk"},
	{"ADDA Playback", NULL, "aud_dl0_predis_clk"},
	/* capture */
	{"ADDA_UL_Mux", "MTKAIF", "ADDA Capture"},
	{"ADDA Capture", NULL, "ADDA Enable"},
	{"ADDA Capture", NULL, "ADDA Capture Enable"},
	{"ADDA Capture", NULL, "AUD_PAD_TOP"},
	{"ADDA Capture", NULL, "ADDA_MTKAIF_CFG"},
	{"ADDA Capture Enable", NULL, "aud_ul0_adc_clk"},
	{"ADDA Capture Enable", NULL, "aud_ul0_tml_clk"},

	{"ADDA_CH34_UL_Mux", "MTKAIF", "ADDA CH34 Capture"},

};

/* dai ops */
static int set_playback_hw_params(struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	unsigned int rate = params_rate(params);
	struct mtk_afe_adda_priv *adda_priv;
	int id = dai->id;

	adda_priv = afe_priv->dai_priv[id];
	if (!adda_priv)
		return -EINVAL;

	adda_priv->dl_rate = rate;

	if (id == MT6878_DAI_ADDA) {
		unsigned int dl_src_con0, dl_src_con1;

		dl_src_con0 = adda_dl_rate_transform(afe, rate) <<
			      AFE_DL_INPUT_MODE_CTL_SFT;
		/* output mode, UP_SAMPLING_RATE_X8 */
		dl_src_con0 |= 0x3 << AFE_DL_OUTPUT_SEL_CTL_SFT;
		/* turn off mute */
		dl_src_con0 |= 0x1 << AFE_DL_MUTE_CH2_OFF_CTL_PRE_SFT;
		dl_src_con0 |= 0x1 << AFE_DL_MUTE_CH1_OFF_CTL_PRE_SFT;
		/* voice input data for 8k/16k */
		if (rate == 8000 || rate == 16000)
			dl_src_con0 |= 0x1 << AFE_DL_VOICE_MODE_CTL_PRE_SFT;
		/* -0.3 dB on the audio path */
		dl_src_con1 = MTK_AFE_ADDA_DL_GAIN_NORMAL_VAL << AFE_DL_GAIN1_CTL_PRE_SFT;
		dl_src_con1 |= MTK_AFE_ADDA_DL_GAIN_NORMAL_VAL << AFE_DL_GAIN2_CTL_PRE_SFT;
		/* turn on down-link gain */
		dl_src_con0 |= 0x1 << AFE_DL_GAIN_ON_CTL_PRE_SFT;

		/* clean predistortion */
		regmap_write(afe->regmap, AFE_ADDA_DL_PREDIS_CON0, 0);
		regmap_write(afe->regmap, AFE_ADDA_DL_PREDIS_CON1, 0);

		regmap_write(afe->regmap, AFE_ADDA_DL_SRC_CON0, dl_src_con0);
		regmap_write(afe->regmap, AFE_ADDA_DL_SRC_CON1, dl_src_con1);

		/* sdm gain, 2nd order sdm */
		regmap_update_bits(afe->regmap, AFE_ADDA_DL_SDM_DCCOMP_CON,
				   AFE_DL_ATTGAIN_CTL_MASK_SFT,
				   AUDIO_SDM_LEVEL_NORMAL << AFE_DL_ATTGAIN_CTL_SFT);
		regmap_update_bits(afe->regmap, AFE_ADDA_DL_SDM_DCCOMP_CON,
				   AFE_DL_USE_3RD_SDM_MASK_SFT,
				   AUDIO_SDM_2ND << AFE_DL_USE_3RD_SDM_SFT);

		/* sdm auto reset */
		regmap_write(afe->regmap, AFE_ADDA_DL_SDM_AUTO_RESET_CON,
			     SDM_AUTO_RESET_THRESHOLD);
		regmap_update_bits(afe->regmap, AFE_ADDA_DL_SDM_AUTO_RESET_CON,
				   AFE_DL_SDM_AUTO_RESET_TEST_ON_SFT,
				   0x1 << AFE_DL_SDM_AUTO_RESET_TEST_ON_SFT);
	}
	return 0;
};

static void adda_ul_iir(struct mtk_base_afe *afe, bool ch34,
			unsigned int ul_src_con0)
{
	/* 35Hz @ 48k */
	regmap_write(afe->regmap, ch34 ? AFE_ADDA_UL1_IIR_COEF_02_01 :
		     AFE_ADDA_UL0_IIR_COEF_02_01, 0x00000000);
	regmap_write(afe->regmap, ch34 ? AFE_ADDA_UL1_IIR_COEF_04_03 :
		     AFE_ADDA_UL0_IIR_COEF_04_03, 0x00003FB8);
	regmap_write(afe->regmap, ch34 ? AFE_ADDA_UL1_IIR_COEF_06_05 :
		     AFE_ADDA_UL0_IIR_COEF_06_05, 0x3FB80000);
	regmap_write(afe->regmap, ch34 ? AFE_ADDA_UL1_IIR_COEF_08_07 :
		     AFE_ADDA_UL0_IIR_COEF_08_07, 0x3FB80000);
	regmap_write(afe->regmap, ch34 ? AFE_ADDA_UL1_IIR_COEF_10_09 :
		     AFE_ADDA_UL0_IIR_COEF_10_09, 0x0000C048);

	regmap_update_bits(afe->regmap,
			   ch34 ? AFE_ADDA_UL1_SRC_CON0 : AFE_ADDA_UL0_SRC_CON0,
			   UL_IIR_ON_TMP_CTL_MASK_SFT |
			   UL_IIRMODE_CTL_MASK_SFT |
			   UL_VOICE_MODE_CH1_CH2_CTL_MASK_SFT,
			   ul_src_con0);

	/* mtkaif_rxif_data_mode = 0, amic */
	regmap_update_bits(afe->regmap,
			   ch34 ? AFE_MTKAIF1_RX_CFG0 : AFE_MTKAIF0_RX_CFG0,
			   0x1 << 0, 0x0 << 0);
}

/* vendor MT6878 uplink setup: MTKAIF 2.0, no v4 RX */
static int set_capture_hw_params(struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	unsigned int rate = params_rate(params);
	struct mtk_afe_adda_priv *adda_priv;
	unsigned int ul_src_con0 = 0;
	int id = dai->id;

	adda_priv = afe_priv->dai_priv[id];
	if (!adda_priv)
		return -EINVAL;

	adda_priv->ul_rate = rate;

	ul_src_con0 |= (adda_ul_rate_transform(afe, rate) << 17) & (0x7 << 17);
	ul_src_con0 |= (1 << UL_IIR_ON_TMP_CTL_SFT) & UL_IIR_ON_TMP_CTL_MASK_SFT;
	ul_src_con0 |= (UL_IIR_SW << UL_IIRMODE_CTL_SFT) & UL_IIRMODE_CTL_MASK_SFT;

	switch (id) {
	case MT6878_DAI_ADDA:
	case MT6878_DAI_AP_DMIC:
		adda_ul_iir(afe, false, ul_src_con0);
		break;
	case MT6878_DAI_ADDA_CH34:
	case MT6878_DAI_AP_DMIC_CH34:
		adda_ul_iir(afe, true, ul_src_con0);
		break;
	default:
		break;
	}

	return 0;
}

static int mtk_dai_adda_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	int id = dai->id;

	if (id >= MT6878_DAI_NUM || id < 0)
		return -EINVAL;

	dev_dbg(afe->dev, "id %d, stream %d, rate %d\n",
		id, substream->stream, params_rate(params));

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return set_playback_hw_params(params, dai);
	else
		return set_capture_hw_params(params, dai);

	return 0;
}

static const struct snd_soc_dai_ops mtk_dai_adda_ops = {
	.hw_params = mtk_dai_adda_hw_params,
};

/* dai driver */
#define MTK_ADDA_PLAYBACK_RATES (SNDRV_PCM_RATE_8000_48000 |\
				 SNDRV_PCM_RATE_96000 |\
				 SNDRV_PCM_RATE_192000)

#define MTK_ADDA_CAPTURE_RATES (SNDRV_PCM_RATE_8000 |\
				SNDRV_PCM_RATE_16000 |\
				SNDRV_PCM_RATE_32000 |\
				SNDRV_PCM_RATE_48000 |\
				SNDRV_PCM_RATE_96000 |\
				SNDRV_PCM_RATE_192000)

#define MTK_ADDA_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			  SNDRV_PCM_FMTBIT_S24_LE |\
			  SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mtk_dai_adda_driver[] = {
	{
		.name = "ADDA",
		.id = MT6878_DAI_ADDA,
		.playback = {
			.stream_name = "ADDA Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_PLAYBACK_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.capture = {
			.stream_name = "ADDA Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_CAPTURE_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.ops = &mtk_dai_adda_ops,
	},
	{
		.name = "ADDA_CH34",
		.id = MT6878_DAI_ADDA_CH34,
		.playback = {
			.stream_name = "ADDA CH34 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_PLAYBACK_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.capture = {
			.stream_name = "ADDA CH34 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_CAPTURE_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.ops = &mtk_dai_adda_ops,
	},
	{
		.name = "ADDA_CH56",
		.id = MT6878_DAI_ADDA_CH56,
		.capture = {
			.stream_name = "ADDA CH56 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_ADDA_CAPTURE_RATES,
			.formats = MTK_ADDA_FORMATS,
		},
		.ops = &mtk_dai_adda_ops,
	},
};

static int init_adda_priv_data(struct mtk_base_afe *afe)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	struct mtk_afe_adda_priv *adda_priv;
	static const int adda_dai_list[] = {
		MT6878_DAI_ADDA,
		MT6878_DAI_ADDA_CH34,
		MT6878_DAI_ADDA_CH56,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(adda_dai_list); i++) {
		adda_priv = devm_kzalloc(afe->dev,
					 sizeof(struct mtk_afe_adda_priv),
					 GFP_KERNEL);
		if (!adda_priv)
			return -ENOMEM;

		afe_priv->dai_priv[adda_dai_list[i]] = adda_priv;
	}

	return 0;
}

int mt6878_dai_adda_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;
	int ret;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	dai->dai_drivers = mtk_dai_adda_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_adda_driver);
	dai->dapm_widgets = mtk_dai_adda_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_adda_widgets);
	dai->dapm_routes = mtk_dai_adda_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_adda_routes);

	ret = init_adda_priv_data(afe);
	if (ret)
		return ret;

	list_add(&dai->list, &afe->sub_dais);

	return 0;
}

