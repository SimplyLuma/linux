// SPDX-License-Identifier: GPL-2.0
/*
 *  mt6878-afe-clk.c  --  Mediatek 6878 afe clock ctrl
 *
 *  Based on the MT8196 driver by Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/regmap.h>

#include "mt6878-afe-clk.h"
#include "mt6878-afe-common.h"

static const char *aud_clks[MT6878_CLK_NUM] = {
	[MT6878_CLK_TOP_MUX_AUDIOINTBUS] = "aud_intbus",
	[MT6878_CLK_TOP_MUX_AUD_1] = "aud_1",
	[MT6878_CLK_TOP_MUX_AUD_2] = "aud_2",
	[MT6878_CLK_TOP_MUX_AUD_ENG1] = "aud_eng1",
	[MT6878_CLK_TOP_MUX_AUD_ENG2] = "aud_eng2",
	[MT6878_CLK_TOP_MUX_AUDIO_H] = "audio_h",
	[MT6878_CLK_TOP_MAINPLL_D4_D4] = "mainpll_d4_d4",
	[MT6878_CLK_TOP_APLL1_CK] = "apll1",
	[MT6878_CLK_TOP_APLL2_CK] = "apll2",
	[MT6878_CLK_TOP_APLL1_D4] = "apll1_d4",
	[MT6878_CLK_TOP_APLL2_D4] = "apll2_d4",
	[MT6878_CLK_TOP_CLK26M] = "clk26m",
	[MT6878_CLK_PERAO_AUDIO_SLV] = "aud_slv_peri",
	[MT6878_CLK_PERAO_AUDIO_MST] = "aud_mst_peri",
	[MT6878_CLK_PERAO_AUDIO_INTBUS] = "aud_intbus_peri",
	[MT6878_CLK_TOP_I2SIN1_M_SEL] = "i2sin1_m_sel",
	[MT6878_CLK_TOP_I2SIN2_M_SEL] = "i2sin2_m_sel",
	[MT6878_CLK_TOP_I2SIN4_M_SEL] = "i2sin4_m_sel",
	[MT6878_CLK_TOP_I2SOUT1_M_SEL] = "i2sout1_m_sel",
	[MT6878_CLK_TOP_I2SOUT2_M_SEL] = "i2sout2_m_sel",
	[MT6878_CLK_TOP_I2SOUT4_M_SEL] = "i2sout4_m_sel",
	[MT6878_CLK_TOP_FMI2S_M_SEL] = "fmi2s_m_sel",
	[MT6878_CLK_TOP_APLL12_DIV_I2SIN1] = "apll12_div_i2sin1",
	[MT6878_CLK_TOP_APLL12_DIV_I2SIN2] = "apll12_div_i2sin2",
	[MT6878_CLK_TOP_APLL12_DIV_I2SIN4] = "apll12_div_i2sin4",
	[MT6878_CLK_TOP_APLL12_DIV_I2SOUT1] = "apll12_div_i2sout1",
	[MT6878_CLK_TOP_APLL12_DIV_I2SOUT2] = "apll12_div_i2sout2",
	[MT6878_CLK_TOP_APLL12_DIV_I2SOUT4] = "apll12_div_i2sout4",
	[MT6878_CLK_TOP_APLL12_DIV_FMI2S] = "apll12_div_fmi2s",
};

int mt6878_afe_enable_clk(struct mtk_base_afe *afe, struct clk *clk)
{
	int ret;

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(afe->dev, "failed to enable clk\n");
		return ret;
	}

	return 0;
}

void mt6878_afe_disable_clk(struct mtk_base_afe *afe, struct clk *clk)
{
	clk_disable_unprepare(clk);
}

static int mt6878_afe_set_parent(struct mtk_base_afe *afe, int mux, int parent)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = clk_set_parent(afe_priv->clk[mux], afe_priv->clk[parent]);
	if (ret)
		dev_err(afe->dev, "set parent %s -> %s failed: %d\n",
			aud_clks[mux], aud_clks[parent], ret);

	return ret;
}

static int mt6878_afe_enable_mux(struct mtk_base_afe *afe, int mux, int parent)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int ret;

	ret = mt6878_afe_enable_clk(afe, afe_priv->clk[mux]);
	if (ret)
		return ret;

	return mt6878_afe_set_parent(afe, mux, parent);
}

static void mt6878_afe_disable_mux(struct mtk_base_afe *afe, int mux)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;

	mt6878_afe_set_parent(afe, mux, MT6878_CLK_TOP_CLK26M);
	mt6878_afe_disable_clk(afe, afe_priv->clk[mux]);
}

static unsigned int get_top_cg_reg(unsigned int cg_type)
{
	switch (cg_type) {
	case MT6878_AUDIO_26M_EN_ON:
	case MT6878_AUDIO_F3P25M_EN_ON:
	case MT6878_AUDIO_APLL1_EN_ON:
	case MT6878_AUDIO_APLL2_EN_ON:
		return AUDIO_ENGEN_CON0;
	case MT6878_CG_AUDIO_HOPPING_CK:
	case MT6878_CG_AUDIO_F26M_CK:
	case MT6878_CG_APLL1_CK:
	case MT6878_CG_APLL2_CK:
	case MT6878_PDN_APLL_TUNER2:
	case MT6878_PDN_APLL_TUNER1:
		return AUDIO_TOP_CON4;
	default:
		return 0;
	}
}

static unsigned int get_top_cg_mask(unsigned int cg_type)
{
	switch (cg_type) {
	case MT6878_AUDIO_26M_EN_ON:
		return AUDIO_26M_EN_ON_MASK_SFT;
	case MT6878_AUDIO_F3P25M_EN_ON:
		return AUDIO_F3P25M_EN_ON_MASK_SFT;
	case MT6878_AUDIO_APLL1_EN_ON:
		return AUDIO_APLL1_EN_ON_MASK_SFT;
	case MT6878_AUDIO_APLL2_EN_ON:
		return AUDIO_APLL2_EN_ON_MASK_SFT;
	case MT6878_CG_AUDIO_HOPPING_CK:
		return CG_AUDIO_HOPPING_CK_MASK_SFT;
	case MT6878_CG_AUDIO_F26M_CK:
		return CG_AUDIO_F26M_CK_MASK_SFT;
	case MT6878_CG_APLL1_CK:
		return CG_APLL1_CK_MASK_SFT;
	case MT6878_CG_APLL2_CK:
		return CG_APLL2_CK_MASK_SFT;
	case MT6878_PDN_APLL_TUNER2:
		return PDN_APLL_TUNER2_MASK_SFT;
	case MT6878_PDN_APLL_TUNER1:
		return PDN_APLL_TUNER1_MASK_SFT;
	default:
		return 0;
	}
}

static bool top_cg_is_enable_bit(unsigned int cg_type)
{
	return get_top_cg_reg(cg_type) == AUDIO_ENGEN_CON0;
}

static int mt6878_afe_set_top_cg(struct mtk_base_afe *afe, unsigned int cg_type,
				 bool on)
{
	unsigned int reg = get_top_cg_reg(cg_type);
	unsigned int mask = get_top_cg_mask(cg_type);
	unsigned int val;
	int ret;

	if (!afe->regmap)
		return 0;

	/* AUDIO_ENGEN_CON0 bits are enables, AUDIO_TOP_CON4 bits are power-downs */
	val = (on == top_cg_is_enable_bit(cg_type)) ? mask : 0;

	ret = regmap_update_bits(afe->regmap, reg, mask, val);
	if (ret)
		dev_err(afe->dev, "regmap_update_bits failed: %d\n", ret);

	return ret;
}

int mt6878_apll1_enable(struct mtk_base_afe *afe)
{
	int ret;

	ret = mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUD_1,
				    MT6878_CLK_TOP_APLL1_CK);
	if (ret)
		return ret;

	ret = mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUD_ENG1,
				    MT6878_CLK_TOP_APLL1_D4);
	if (ret)
		goto err_eng1;

	mt6878_afe_set_top_cg(afe, MT6878_CG_APLL1_CK, true);
	mt6878_afe_set_top_cg(afe, MT6878_PDN_APLL_TUNER1, true);

	/* sel 44.1kHz:1, apll_div:7, upper bound:3 */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   XTAL_EN_128FS_SEL_MASK_SFT | APLL_DIV_MASK_SFT | UPPER_BOUND_MASK_SFT,
			   (0x1 << XTAL_EN_128FS_SEL_SFT) | (7 << APLL_DIV_SFT) |
			   (3 << UPPER_BOUND_SFT));

	/* apll1 freq tuner enable */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x1 << FREQ_TUNER_EN_SFT);

	/* audio apll1 on */
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_APLL1_EN_ON, true);

	return 0;

err_eng1:
	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_1);
	return ret;
}

void mt6878_apll1_disable(struct mtk_base_afe *afe)
{
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_APLL1_EN_ON, false);

	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT, 0x0);

	mt6878_afe_set_top_cg(afe, MT6878_PDN_APLL_TUNER1, false);
	mt6878_afe_set_top_cg(afe, MT6878_CG_APLL1_CK, false);

	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_ENG1);
	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_1);
}

int mt6878_apll2_enable(struct mtk_base_afe *afe)
{
	int ret;

	ret = mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUD_2,
				    MT6878_CLK_TOP_APLL2_CK);
	if (ret)
		return ret;

	ret = mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUD_ENG2,
				    MT6878_CLK_TOP_APLL2_D4);
	if (ret)
		goto err_eng2;

	mt6878_afe_set_top_cg(afe, MT6878_CG_APLL2_CK, true);
	mt6878_afe_set_top_cg(afe, MT6878_PDN_APLL_TUNER2, true);

	/* sel 48kHz: 2, apll_div: 7, upper bound: 3*/
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   XTAL_EN_128FS_SEL_MASK_SFT | APLL_DIV_MASK_SFT | UPPER_BOUND_MASK_SFT,
			   (0x2 << XTAL_EN_128FS_SEL_SFT) | (7 << APLL_DIV_SFT) |
			   (3 << UPPER_BOUND_SFT));

	/* apll2 freq tuner enable */
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x1 << FREQ_TUNER_EN_SFT);

	/* audio apll2 on */
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_APLL2_EN_ON, true);

	return 0;

err_eng2:
	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_2);
	return ret;
}

void mt6878_apll2_disable(struct mtk_base_afe *afe)
{
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_APLL2_EN_ON, false);

	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT, 0x0);

	mt6878_afe_set_top_cg(afe, MT6878_PDN_APLL_TUNER2, false);
	mt6878_afe_set_top_cg(afe, MT6878_CG_APLL2_CK, false);

	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_ENG2);
	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUD_2);
}

int mt6878_get_apll_rate(struct mtk_base_afe *afe, int apll)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int clk_id;

	if (apll < MT6878_APLL1 || apll > MT6878_APLL2) {
		dev_warn(afe->dev, "invalid clk id %d\n", apll);
		return 0;
	}

	clk_id = apll == MT6878_APLL1 ? MT6878_CLK_TOP_APLL1_CK : MT6878_CLK_TOP_APLL2_CK;

	return clk_get_rate(afe_priv->clk[clk_id]);
}

/* 48K: select APLL2; 44.1k: select APLL1 */
int mt6878_get_apll_by_rate(struct mtk_base_afe *afe, int rate)
{
	return (rate % 8000) ? MT6878_APLL1 : MT6878_APLL2;
}

int mt6878_get_apll_by_name(struct mtk_base_afe *afe, const char *name)
{
	if (strcmp(name, APLL1_W_NAME) == 0)
		return MT6878_APLL1;

	return MT6878_APLL2;
}

struct mt6878_mck_div {
	int m_sel_id;
	int div_clk_id;
};

static const struct mt6878_mck_div mck_div[MT6878_MCK_NUM] = {
	[MT6878_I2SIN1_MCK] = { MT6878_CLK_TOP_I2SIN1_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SIN1 },
	[MT6878_I2SIN2_MCK] = { MT6878_CLK_TOP_I2SIN2_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SIN2 },
	[MT6878_I2SIN4_MCK] = { MT6878_CLK_TOP_I2SIN4_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SIN4 },
	[MT6878_I2SOUT1_MCK] = { MT6878_CLK_TOP_I2SOUT1_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SOUT1 },
	[MT6878_I2SOUT2_MCK] = { MT6878_CLK_TOP_I2SOUT2_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SOUT2 },
	[MT6878_I2SOUT4_MCK] = { MT6878_CLK_TOP_I2SOUT4_M_SEL, MT6878_CLK_TOP_APLL12_DIV_I2SOUT4 },
	[MT6878_FMI2S_MCK] = { MT6878_CLK_TOP_FMI2S_M_SEL, MT6878_CLK_TOP_APLL12_DIV_FMI2S },
};

int mt6878_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int apll = mt6878_get_apll_by_rate(afe, rate);
	int apll_mux = apll == MT6878_APLL1 ?
		       MT6878_CLK_TOP_MUX_AUD_1 : MT6878_CLK_TOP_MUX_AUD_2;
	struct clk *m_sel, *div;
	int ret;

	if (mck_id >= MT6878_MCK_NUM || mck_id < 0)
		return -EINVAL;

	m_sel = afe_priv->clk[mck_div[mck_id].m_sel_id];
	div = afe_priv->clk[mck_div[mck_id].div_clk_id];

	/* mclk outputs are optional on this SoC; boards without them skip mclk */
	if (!m_sel || !div)
		return 0;

	ret = mt6878_afe_enable_clk(afe, m_sel);
	if (ret)
		return ret;

	ret = clk_set_parent(m_sel, afe_priv->clk[apll_mux]);
	if (ret)
		return ret;

	ret = mt6878_afe_enable_clk(afe, div);
	if (ret)
		return ret;

	return clk_set_rate(div, rate);
}

int mt6878_mck_disable(struct mtk_base_afe *afe, int mck_id)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	struct clk *m_sel, *div;

	if (mck_id >= MT6878_MCK_NUM || mck_id < 0)
		return -EINVAL;

	m_sel = afe_priv->clk[mck_div[mck_id].m_sel_id];
	div = afe_priv->clk[mck_div[mck_id].div_clk_id];
	if (!m_sel || !div)
		return 0;

	mt6878_afe_disable_clk(afe, div);
	mt6878_afe_disable_clk(afe, m_sel);

	return 0;
}

int mt6878_afe_enable_reg_rw_clk(struct mtk_base_afe *afe)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int ret;

	/* infra bus access to the audio subsystem */
	ret = mt6878_afe_enable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_SLV]);
	if (ret)
		return ret;
	ret = mt6878_afe_enable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_MST]);
	if (ret)
		return ret;
	ret = mt6878_afe_enable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_INTBUS]);
	if (ret)
		return ret;

	/* bus clock for AFE internal access, like AFE SRAM */
	ret = mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUDIOINTBUS,
				    MT6878_CLK_TOP_MAINPLL_D4_D4);
	if (ret)
		return ret;

	/* audio h clock, switched to an APLL when one is in use */
	return mt6878_afe_enable_mux(afe, MT6878_CLK_TOP_MUX_AUDIO_H,
				     MT6878_CLK_TOP_CLK26M);
}

int mt6878_afe_disable_reg_rw_clk(struct mtk_base_afe *afe)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;

	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUDIO_H);
	mt6878_afe_disable_mux(afe, MT6878_CLK_TOP_MUX_AUDIOINTBUS);
	mt6878_afe_disable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_INTBUS]);
	mt6878_afe_disable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_MST]);
	mt6878_afe_disable_clk(afe, afe_priv->clk[MT6878_CLK_PERAO_AUDIO_SLV]);
	return 0;
}

int mt6878_afe_enable_main_clock(struct mtk_base_afe *afe)
{
	mt6878_afe_set_top_cg(afe, MT6878_CG_AUDIO_HOPPING_CK, true);
	mt6878_afe_set_top_cg(afe, MT6878_CG_AUDIO_F26M_CK, true);
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_26M_EN_ON, true);
	return 0;
}

int mt6878_afe_disable_main_clock(struct mtk_base_afe *afe)
{
	mt6878_afe_set_top_cg(afe, MT6878_AUDIO_26M_EN_ON, false);
	return 0;
}

int mt6878_init_clock(struct mtk_base_afe *afe)
{
	struct mt6878_afe_private *afe_priv = afe->platform_priv;
	int i;

	afe_priv->clk = devm_kcalloc(afe->dev, MT6878_CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < MT6878_CLK_NUM; i++) {
		if (i < MT6878_CLK_REQUIRED_NUM)
			afe_priv->clk[i] = devm_clk_get(afe->dev, aud_clks[i]);
		else
			afe_priv->clk[i] = devm_clk_get_optional(afe->dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i]))
			return dev_err_probe(afe->dev, PTR_ERR(afe_priv->clk[i]),
					     "devm_clk_get %s fail\n", aud_clks[i]);
	}


	return 0;
}
