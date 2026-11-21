// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,sm7250-videocc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

enum {
	DT_BI_TCXO,
	DT_BI_TCXO_AO,
	DT_SLEEP_CLK,
};

enum {
	P_BI_TCXO,
	P_CHIP_SLEEP_CLK,
	P_VIDEOCC_PLL0_OUT_MAIN,
};

static const struct pll_vco lucid_vco[] = {
	{ 249600000, 2000000000, 0 },
};

static const struct alpha_pll_config videocc_pll0_config = {
	.l = 0x19,
	.cal_l = 0x44,
	.alpha = 0x0,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00002261,
	.config_ctl_hi1_val = 0x329a699c,
	.user_ctl_val = 0x00000001,
	.user_ctl_hi_val = 0x00000805,
	.user_ctl_hi1_val = 0x00000000,
	.test_ctl_hi1_val = 0x01800000,
};

static struct clk_alpha_pll videocc_pll0 = {
	.offset = 0x42c,
	.vco_table = lucid_vco,
	.num_vco = ARRAY_SIZE(lucid_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_pll0",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_ops,
		},
	},
};

static const struct parent_map videocc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
	{ P_VIDEOCC_PLL0_OUT_MAIN, 1 },
};

static const struct clk_parent_data videocc_parent_data_0[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &videocc_pll0.clkr.hw },
};

static const struct parent_map videocc_parent_map_1[] = {
	{ P_CHIP_SLEEP_CLK, 0 },
};

static const struct clk_parent_data videocc_parent_data_1[] = {
	{ .index = DT_SLEEP_CLK },
};

static const struct parent_map videocc_parent_map_2[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data videocc_parent_data_2_ao[] = {
	{ .index = DT_BI_TCXO_AO },
};

static const struct freq_tbl ftbl_videocc_iris_clk_src[] = {
	F(240000000, P_VIDEOCC_PLL0_OUT_MAIN, 2, 0, 0),
	F(338000000, P_VIDEOCC_PLL0_OUT_MAIN, 2, 0, 0),
	F(365000000, P_VIDEOCC_PLL0_OUT_MAIN, 2, 0, 0),
	F(444000000, P_VIDEOCC_PLL0_OUT_MAIN, 2, 0, 0),
	F(533000000, P_VIDEOCC_PLL0_OUT_MAIN, 2, 0, 0),
	{ }
};

static struct clk_rcg2 videocc_iris_clk_src = {
	.cmd_rcgr = 0x7f0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = videocc_parent_map_0,
	.freq_tbl = ftbl_videocc_iris_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "videocc_iris_clk_src",
		.parent_data = videocc_parent_data_0,
		.num_parents = ARRAY_SIZE(videocc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_videocc_sleep_clk_src[] = {
	F(32000, P_CHIP_SLEEP_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 videocc_sleep_clk_src = {
	.cmd_rcgr = 0x984,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = videocc_parent_map_1,
	.freq_tbl = ftbl_videocc_sleep_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "videocc_sleep_clk_src",
		.parent_data = videocc_parent_data_1,
		.num_parents = ARRAY_SIZE(videocc_parent_data_1),
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_videocc_xo_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	{ }
};

static struct clk_rcg2 videocc_xo_clk_src = {
	.cmd_rcgr = 0x960,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = videocc_parent_map_2,
	.freq_tbl = ftbl_videocc_xo_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "videocc_xo_clk_src",
		.parent_data = videocc_parent_data_2_ao,
		.num_parents = ARRAY_SIZE(videocc_parent_data_2_ao),
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch videocc_apb_clk = {
	.halt_reg = 0xa4c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa4c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_apb_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvs0_axi_clk = {
	.halt_reg = 0x9ec,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x9ec,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvs0_axi_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvs0_core_clk = {
	.halt_reg = 0x890,
	.halt_check = BRANCH_VOTED,
	.clkr = {
		.enable_reg = 0x890,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvs0_core_clk",
			.parent_data = &(const struct clk_parent_data) {
				.hw = &videocc_iris_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvs1_axi_clk = {
	.halt_reg = 0xa0c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa0c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvs1_axi_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvs1_core_clk = {
	.halt_reg = 0x8d0,
	.halt_check = BRANCH_VOTED,
	.clkr = {
		.enable_reg = 0x8d0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvs1_core_clk",
			.parent_data = &(const struct clk_parent_data) {
				.hw = &videocc_iris_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvsc_core_clk = {
	.halt_reg = 0x850,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x850,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvsc_core_clk",
			.parent_data = &(const struct clk_parent_data) {
				.hw = &videocc_iris_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_mvsc_ctl_axi_clk = {
	.halt_reg = 0x9cc,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x9cc,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_mvsc_ctl_axi_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_sleep_clk = {
	.halt_reg = 0x9a4,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x9a4,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_sleep_clk",
			.parent_data = &(const struct clk_parent_data) {
				.hw = &videocc_sleep_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_venus_ahb_clk = {
	.halt_reg = 0xa6c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa6c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_venus_ahb_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch videocc_xo_clk = {
	.halt_reg = 0x980,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x980,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "videocc_xo_clk",
			.parent_data = &(const struct clk_parent_data) {
				.hw = &videocc_xo_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL | CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct gdsc mvsc_gdsc = {
	.gdscr = 0x814,
	.pd = {
		.name = "mvsc_gdsc",
	},
	.cxcs = (unsigned int []){ 0x850, 0x9cc },
	.cxc_count = 2,
	.pwrsts = PWRSTS_OFF_ON,
};

static struct gdsc mvs0_gdsc = {
	.gdscr = 0x874,
	.pd = {
		.name = "mvs0_gdsc",
	},
	.cxcs = (unsigned int []){ 0x890, 0x9ec },
	.cxc_count = 2,
	.pwrsts = PWRSTS_OFF_ON,
	.flags = HW_CTRL_TRIGGER,
};

static struct gdsc mvs1_gdsc = {
	.gdscr = 0x8b4,
	.pd = {
		.name = "mvs1_gdsc",
	},
	.cxcs = (unsigned int []){ 0x8d0, 0xa0c },
	.cxc_count = 2,
	.pwrsts = PWRSTS_OFF_ON,
	.flags = HW_CTRL_TRIGGER,
};

static struct clk_regmap *videocc_sm7250_clocks[] = {
	[VIDEOCC_PLL0] = &videocc_pll0.clkr,
	[VIDEOCC_APB_CLK] = &videocc_apb_clk.clkr,
	[VIDEOCC_IRIS_CLK_SRC] = &videocc_iris_clk_src.clkr,
	[VIDEOCC_MVS0_AXI_CLK] = &videocc_mvs0_axi_clk.clkr,
	[VIDEOCC_MVS0_CORE_CLK] = &videocc_mvs0_core_clk.clkr,
	[VIDEOCC_MVS1_AXI_CLK] = &videocc_mvs1_axi_clk.clkr,
	[VIDEOCC_MVS1_CORE_CLK] = &videocc_mvs1_core_clk.clkr,
	[VIDEOCC_MVSC_CORE_CLK] = &videocc_mvsc_core_clk.clkr,
	[VIDEOCC_MVSC_CTL_AXI_CLK] = &videocc_mvsc_ctl_axi_clk.clkr,
	[VIDEOCC_SLEEP_CLK] = &videocc_sleep_clk.clkr,
	[VIDEOCC_SLEEP_CLK_SRC] = &videocc_sleep_clk_src.clkr,
	[VIDEOCC_VENUS_AHB_CLK] = &videocc_venus_ahb_clk.clkr,
	[VIDEOCC_XO_CLK_SRC] = &videocc_xo_clk_src.clkr,
	[VIDEOCC_XO_CLK] = &videocc_xo_clk.clkr,
};

static const struct qcom_reset_map videocc_sm7250_resets[] = {
	[VIDEOCC_INTERFACE_BCR] = { 0x9ac },
	[VIDEOCC_MVS0_BCR] = { 0x870 },
	[VIDEOCC_MVS1_BCR] = { 0x8b0 },
	[VIDEOCC_MVSC_BCR] = { 0x810 },
};

static struct gdsc *videocc_sm7250_gdscs[] = {
	[MVSC_GDSC] = &mvsc_gdsc,
	[MVS0_GDSC] = &mvs0_gdsc,
	[MVS1_GDSC] = &mvs1_gdsc,
};

static const struct regmap_config videocc_sm7250_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0xb94,
	.fast_io = true,
};

static const struct qcom_cc_desc videocc_sm7250_desc = {
	.config = &videocc_sm7250_regmap_config,
	.clks = videocc_sm7250_clocks,
	.num_clks = ARRAY_SIZE(videocc_sm7250_clocks),
	.resets = videocc_sm7250_resets,
	.num_resets = ARRAY_SIZE(videocc_sm7250_resets),
	.gdscs = videocc_sm7250_gdscs,
	.num_gdscs = ARRAY_SIZE(videocc_sm7250_gdscs),
};

static const struct of_device_id videocc_sm7250_match_table[] = {
	{ .compatible = "qcom,sm7250-videocc" },
	{ }
};
MODULE_DEVICE_TABLE(of, videocc_sm7250_match_table);

static int videocc_sm7250_probe(struct platform_device *pdev)
{
	struct regmap *regmap;

	regmap = qcom_cc_map(pdev, &videocc_sm7250_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	clk_lucid_pll_configure(&videocc_pll0, regmap, &videocc_pll0_config);

	/* Keep some clocks always-on */
	qcom_branch_set_clk_en(regmap, 0x980); /* VIDEOCC_XO_CLK */

	return qcom_cc_really_probe(&pdev->dev, &videocc_sm7250_desc, regmap);
}

static struct platform_driver videocc_sm7250_driver = {
	.probe = videocc_sm7250_probe,
	.driver = {
		.name = "videocc-sm7250",
		.of_match_table = videocc_sm7250_match_table,
	},
};

module_platform_driver(videocc_sm7250_driver);

MODULE_DESCRIPTION("Qualcomm SM7250 Video Clock Controller");
MODULE_LICENSE("GPL");
