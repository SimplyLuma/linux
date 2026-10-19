/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt6878-afe-clk.h  --  Mediatek MT6878 AFE Clock Control definitions
 *
 * Based on the MT8196 driver by Darren Ye <darren.ye@mediatek.com>
 */

#ifndef _MT6878_AFE_CLOCK_CTRL_H_
#define _MT6878_AFE_CLOCK_CTRL_H_

#define MT6878_AFE_26M 26000000
#define MT6878_APLL1_RATE 180633600
#define MT6878_APLL2_RATE 196608000

/* APLL */
#define APLL1_W_NAME "APLL1"
#define APLL2_W_NAME "APLL2"

enum {
	MT6878_APLL1 = 0,
	MT6878_APLL2,
};

enum {
	/* topckgen muxes */
	MT6878_CLK_TOP_MUX_AUDIOINTBUS,
	MT6878_CLK_TOP_MUX_AUD_1,
	MT6878_CLK_TOP_MUX_AUD_2,
	MT6878_CLK_TOP_MUX_AUD_ENG1,
	MT6878_CLK_TOP_MUX_AUD_ENG2,
	MT6878_CLK_TOP_MUX_AUDIO_H,
	/* mux parents */
	MT6878_CLK_TOP_MAINPLL_D4_D4,
	MT6878_CLK_TOP_APLL1_CK,
	MT6878_CLK_TOP_APLL2_CK,
	MT6878_CLK_TOP_APLL1_D4,
	MT6878_CLK_TOP_APLL2_D4,
	MT6878_CLK_TOP_CLK26M,
	/* pericfg_ao bus gates */
	MT6878_CLK_PERAO_AUDIO_SLV,
	MT6878_CLK_PERAO_AUDIO_MST,
	MT6878_CLK_PERAO_AUDIO_INTBUS,
	MT6878_CLK_REQUIRED_NUM,
	/* optional: mclk selectors and dividers */
	MT6878_CLK_TOP_I2SIN1_M_SEL = MT6878_CLK_REQUIRED_NUM,
	MT6878_CLK_TOP_I2SIN2_M_SEL,
	MT6878_CLK_TOP_I2SIN4_M_SEL,
	MT6878_CLK_TOP_I2SOUT1_M_SEL,
	MT6878_CLK_TOP_I2SOUT2_M_SEL,
	MT6878_CLK_TOP_I2SOUT4_M_SEL,
	MT6878_CLK_TOP_FMI2S_M_SEL,
	MT6878_CLK_TOP_APLL12_DIV_I2SIN1,
	MT6878_CLK_TOP_APLL12_DIV_I2SIN2,
	MT6878_CLK_TOP_APLL12_DIV_I2SIN4,
	MT6878_CLK_TOP_APLL12_DIV_I2SOUT1,
	MT6878_CLK_TOP_APLL12_DIV_I2SOUT2,
	MT6878_CLK_TOP_APLL12_DIV_I2SOUT4,
	MT6878_CLK_TOP_APLL12_DIV_FMI2S,
	MT6878_CLK_NUM,
};

struct mtk_base_afe;

int mt6878_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate);
int mt6878_mck_disable(struct mtk_base_afe *afe, int mck_id);
int mt6878_get_apll_rate(struct mtk_base_afe *afe, int apll);
int mt6878_get_apll_by_rate(struct mtk_base_afe *afe, int rate);
int mt6878_get_apll_by_name(struct mtk_base_afe *afe, const char *name);
int mt6878_init_clock(struct mtk_base_afe *afe);
int mt6878_afe_enable_clk(struct mtk_base_afe *afe, struct clk *clk);
void mt6878_afe_disable_clk(struct mtk_base_afe *afe, struct clk *clk);
int mt6878_apll1_enable(struct mtk_base_afe *afe);
void mt6878_apll1_disable(struct mtk_base_afe *afe);
int mt6878_apll2_enable(struct mtk_base_afe *afe);
void mt6878_apll2_disable(struct mtk_base_afe *afe);
int mt6878_afe_enable_main_clock(struct mtk_base_afe *afe);
int mt6878_afe_disable_main_clock(struct mtk_base_afe *afe);
int mt6878_afe_enable_reg_rw_clk(struct mtk_base_afe *afe);
int mt6878_afe_disable_reg_rw_clk(struct mtk_base_afe *afe);

#endif
