// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Jorijn van der Graaf
 *
 * SoundWire slave driver for the Qualcomm WCD9378 audio codec.
 *
 * Bring-up skeleton: binds to the two WCD9378 SoundWire devices (TX and
 * RX), maps the SDCA control space through regmap and verifies the device
 * identity registers. No audio paths yet.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw_type.h>

#include "wcd9378.h"

struct wcd9378_sdw_priv {
	struct sdw_slave *sdev;
	struct regmap *regmap;
};

static const struct regmap_config wcd9378_sdw_regmap_config = {
	.name = "wcd9378_sdw",
	.reg_bits = 32,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.max_register = WCD9378_MAX_REGISTER,
};

static void wcd9378_sdw_read_id(struct wcd9378_sdw_priv *wcd)
{
	struct device *dev = &wcd->sdev->dev;
	static const struct {
		const char *name;
		unsigned int reg;
	} id_regs[] = {
		{ "DEV_MANU_ID_0", WCD9378_DEV_MANU_ID_0 },
		{ "DEV_MANU_ID_1", WCD9378_DEV_MANU_ID_1 },
		{ "DEV_PART_ID_0", WCD9378_DEV_PART_ID_0 },
		{ "DEV_PART_ID_1", WCD9378_DEV_PART_ID_1 },
		{ "DEV_VER",       WCD9378_DEV_VER },
		{ "FUNC_EXT_ID_0", WCD9378_FUNC_EXT_ID_0 },
		{ "FUNC_EXT_ID_1", WCD9378_FUNC_EXT_ID_1 },
		{ "ANA_BIAS",      WCD9378_ANA_BIAS },
		{ "ANA_TX_CH1",    WCD9378_ANA_TX_CH1 },
		{ "ANA_MICB1",     WCD9378_ANA_MICB1 },
		{ "SYS_USAGE_CTRL", WCD9378_SYS_USAGE_CTRL },
	};
	unsigned int val, packed, pval;
	int i, ret, pret;

	for (i = 0; i < ARRAY_SIZE(id_regs); i++) {
		/* downstream WCD9378_REG() wire packing */
		packed = ((id_regs[i].reg & 0x0ff00000) >> 8) |
			 (id_regs[i].reg & 0xfff);

		ret = regmap_read(wcd->regmap, id_regs[i].reg, &val);
		pret = regmap_read(wcd->regmap, packed, &pval);
		dev_info(dev, "%s: virt %#010x = %#04x (ret %d), packed %#07x = %#04x (ret %d)\n",
			 id_regs[i].name,
			 id_regs[i].reg, ret ? 0xdead : val, ret,
			 packed, pret ? 0xdead : pval, pret);
	}
}

static int wcd9378_sdw_update_status(struct sdw_slave *slave,
				     enum sdw_slave_status status)
{
	struct wcd9378_sdw_priv *wcd = dev_get_drvdata(&slave->dev);

	dev_info(&slave->dev, "status %d (dev_num %d)\n", status,
		 slave->dev_num);

	if (status == SDW_SLAVE_ATTACHED)
		wcd9378_sdw_read_id(wcd);

	return 0;
}

static const struct sdw_slave_ops wcd9378_sdw_ops = {
	.update_status = wcd9378_sdw_update_status,
};

static int wcd9378_sdw_probe(struct sdw_slave *pdev,
			     const struct sdw_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct wcd9378_sdw_priv *wcd;

	wcd = devm_kzalloc(dev, sizeof(*wcd), GFP_KERNEL);
	if (!wcd)
		return -ENOMEM;

	wcd->sdev = pdev;
	dev_set_drvdata(dev, wcd);

	pdev->prop.scp_int1_mask = SDW_SCP_INT1_IMPL_DEF |
				   SDW_SCP_INT1_BUS_CLASH |
				   SDW_SCP_INT1_PARITY;
	pdev->prop.simple_clk_stop_capable = true;
	/* The SDCA control space sits above the 16-bit address range */
	pdev->prop.paging_support = true;

	wcd->regmap = devm_regmap_init_sdw(pdev, &wcd9378_sdw_regmap_config);
	if (IS_ERR(wcd->regmap))
		return dev_err_probe(dev, PTR_ERR(wcd->regmap),
				     "regmap init failed\n");

	dev_info(dev, "wcd9378 sdw slave probed\n");

	return 0;
}

static const struct sdw_device_id wcd9378_sdw_id[] = {
	SDW_SLAVE_ENTRY(0x0217, 0x0110, 0),
	{ },
};
MODULE_DEVICE_TABLE(sdw, wcd9378_sdw_id);

static struct sdw_driver wcd9378_sdw_driver = {
	.probe = wcd9378_sdw_probe,
	.ops = &wcd9378_sdw_ops,
	.id_table = wcd9378_sdw_id,
	.driver = {
		.name = "wcd9378-sdw",
	},
};
module_sdw_driver(wcd9378_sdw_driver);

MODULE_DESCRIPTION("WCD9378 SoundWire slave driver");
MODULE_LICENSE("GPL");
