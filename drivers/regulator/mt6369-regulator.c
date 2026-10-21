// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6369 PMIC LDO regulators (SPMI).
 *
 * Covers the LDOs used on MT6878 boards so far: VMCH (SD card supply),
 * VMC (SD card I/O), VTP (touch panel) and VIBR (vibration motor). Register layout from the
 * MT6369 values used by the vendor driver.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/spmi.h>

#define MT6369_HWCID			0x0009
#define MT6369_LDO_VMCH_EN		0x1c95
#define MT6369_LDO_VMC_EN		0x1ca4
#define MT6369_LDO_VTP_EN		0x1c15
#define MT6369_LDO_VIBR_EN		0x1bc1
#define MT6369_VMCH_VOSEL		0x1da9
#define MT6369_VMC_VOSEL		0x1dad
#define MT6369_VTP_VOSEL		0x1da5
#define MT6369_VIBR_VOSEL		0x1d99
#define MT6369_LDO_EN_MASK		BIT(0)
#define MT6369_VOSEL_MASK		GENMASK(3, 0)

enum mt6369_regulator_id {
	MT6369_ID_VMCH,
	MT6369_ID_VMC,
	MT6369_ID_VTP,
	MT6369_ID_VIBR,
	MT6369_MAX_REGULATOR,
};

static const unsigned int mt6369_ldo_volt_table2[] = {
	1200000, 1300000, 1500000, 1700000, 1800000, 2000000, 2500000, 2600000,
	2700000, 2800000, 2900000, 3000000, 3100000, 3300000, 3400000, 3500000,
};

static const struct regulator_ops mt6369_ldo_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

#define MT6369_LDO(_id, _name, _en, _vsel)			\
	[MT6369_ID_##_id] = {					\
		.name = _name,					\
		.of_match = of_match_ptr(_name),		\
		.regulators_node = of_match_ptr("regulators"),	\
		.ops = &mt6369_ldo_ops,				\
		.type = REGULATOR_VOLTAGE,			\
		.id = MT6369_ID_##_id,				\
		.owner = THIS_MODULE,				\
		.n_voltages = ARRAY_SIZE(mt6369_ldo_volt_table2), \
		.volt_table = mt6369_ldo_volt_table2,		\
		.enable_reg = _en,				\
		.enable_mask = MT6369_LDO_EN_MASK,		\
		.vsel_reg = _vsel,				\
		.vsel_mask = MT6369_VOSEL_MASK,			\
	}

static const struct regulator_desc mt6369_regulators[] = {
	MT6369_LDO(VMCH, "vmch", MT6369_LDO_VMCH_EN, MT6369_VMCH_VOSEL),
	MT6369_LDO(VMC, "vmc", MT6369_LDO_VMC_EN, MT6369_VMC_VOSEL),
	MT6369_LDO(VTP, "vtp", MT6369_LDO_VTP_EN, MT6369_VTP_VOSEL),
	MT6369_LDO(VIBR, "vibr", MT6369_LDO_VIBR_EN, MT6369_VIBR_VOSEL),
};

static const struct regmap_config mt6369_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1fff,
	.fast_io = true,
};

static int mt6369_regulator_probe(struct spmi_device *sdev)
{
	struct regulator_config config = {};
	struct device *dev = &sdev->dev;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int val;
	int i;

	regmap = devm_regmap_init_spmi_ext(sdev, &mt6369_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "cannot init regmap\n");

	/*
	 * Early in boot the PMIC may not answer on the AP channel yet; keep
	 * trying for a while (probe is asynchronous, so boot is not held up).
	 */
	for (i = 0; i < 300; i++) {
		if (!regmap_read(regmap, MT6369_HWCID, &val))
			break;
		msleep(100);
	}
	if (i == 300)
		return dev_err_probe(dev, -EIO, "PMIC not responding\n");
	dev_info(dev, "MT6369 HWCID 0x%02x (after %d tries)\n", val, i + 1);

	config.dev = dev;
	config.regmap = regmap;

	for (i = 0; i < ARRAY_SIZE(mt6369_regulators); i++) {
		rdev = devm_regulator_register(dev, &mt6369_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "failed to register %s\n",
					     mt6369_regulators[i].name);
	}

	return 0;
}

static const struct of_device_id mt6369_regulator_of_match[] = {
	{ .compatible = "mediatek,mt6369-regulator" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6369_regulator_of_match);

static struct spmi_driver mt6369_regulator_driver = {
	.driver = {
		.name = "mt6369-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = mt6369_regulator_of_match,
	},
	.probe = mt6369_regulator_probe,
};
module_spmi_driver(mt6369_regulator_driver);

MODULE_DESCRIPTION("MediaTek MT6369 PMIC LDO regulator driver");
MODULE_LICENSE("GPL");
