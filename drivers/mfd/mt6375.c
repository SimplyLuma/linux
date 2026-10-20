// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6375 charger PMIC core: PMU register bank and interrupts.
 *
 * Based on the MediaTek MT6375 MFD driver:
 * Copyright (c) 2021 MediaTek Inc.
 * Author: ShuFan Lee <shufan_lee@richtek.com>
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define MT6375_REG_DEV_INFO	0x00
#define MT6375_VENDOR_ID	0x70
#define MT6375_VENDOR_MASK	GENMASK(7, 4)

#define MT6375_REG_EVT0		0xd0
#define MT6375_REG_EVT15	0xdf
#define MT6375_REG_MSK0		0xf0
#define MT6375_REG_MSK15	0xff
#define MT6375_IRQ_REGS		16
#define MT6375_IRQS		(MT6375_IRQ_REGS * 8)

#define MT6375_IRQ_PWR_RDY	0
#define MT6375_IRQ_DETACH	1
#define MT6375_IRQ_BC12_DN	31

/*
 * The PMU bank also holds the system buck, LDO, BATFET and ship-mode
 * controls. Only the charger, BC1.2 and interrupt registers are writable.
 */
static bool mt6375_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x20:	/* CHG_TOP1: CHG_EN */
	case 0x22:	/* CHG_AICR */
	case 0x23:	/* CHG_MIVR */
	case 0x25:	/* CHG_VCHG: CV */
	case 0x26:	/* CHG_ICHG */
	case 0x28:	/* CHG_EOC: IEOC, TE */
	case 0x50:	/* BC12_FUNC: BC12_EN */
	case MT6375_REG_EVT0 ... MT6375_REG_EVT15:
	case MT6375_REG_MSK0 ... MT6375_REG_MSK15:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mt6375_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MT6375_REG_MSK15,
	.writeable_reg = mt6375_writeable_reg,
};

static const struct resource mt6375_chg_resources[] = {
	DEFINE_RES_IRQ_NAMED(MT6375_IRQ_PWR_RDY, "pwr-rdy"),
	DEFINE_RES_IRQ_NAMED(MT6375_IRQ_DETACH, "detach"),
	DEFINE_RES_IRQ_NAMED(MT6375_IRQ_BC12_DN, "bc12-dn"),
};

static const struct mfd_cell mt6375_devs[] = {
	MFD_CELL_OF("mt6375-charger", mt6375_chg_resources, NULL, 0, 0,
		    "mediatek,mt6375-charger"),
};

static int mt6375_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap_irq_chip_data *irq_data;
	struct regmap_irq_chip *chip;
	struct regmap_irq *irqs;
	struct regmap *regmap;
	unsigned int val;
	int i, ret;

	regmap = devm_regmap_init_i2c(client, &mt6375_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to init regmap\n");

	ret = regmap_read(regmap, MT6375_REG_DEV_INFO, &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device info\n");
	if ((val & MT6375_VENDOR_MASK) != MT6375_VENDOR_ID)
		return dev_err_probe(dev, -ENODEV, "unknown vendor id 0x%02x\n", val);

	/* Every line is declared so that all of them start masked. */
	irqs = devm_kcalloc(dev, MT6375_IRQS, sizeof(*irqs), GFP_KERNEL);
	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!irqs || !chip)
		return -ENOMEM;
	for (i = 0; i < MT6375_IRQS; i++) {
		irqs[i].reg_offset = i / 8;
		irqs[i].mask = BIT(i % 8);
	}
	chip->name = "mt6375";
	chip->status_base = MT6375_REG_EVT0;
	chip->ack_base = MT6375_REG_EVT0;
	chip->mask_base = MT6375_REG_MSK0;
	chip->num_regs = MT6375_IRQ_REGS;
	chip->irqs = irqs;
	chip->num_irqs = MT6375_IRQS;
	chip->init_ack_masked = true;

	ret = devm_regmap_add_irq_chip(dev, regmap, client->irq, IRQF_ONESHOT,
				       0, chip, &irq_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add irq chip\n");

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, mt6375_devs,
				    ARRAY_SIZE(mt6375_devs), NULL, 0,
				    regmap_irq_get_domain(irq_data));
}

static const struct of_device_id mt6375_of_match[] = {
	{ .compatible = "mediatek,mt6375" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6375_of_match);

static struct i2c_driver mt6375_driver = {
	.driver = {
		.name = "mt6375",
		.of_match_table = mt6375_of_match,
	},
	.probe = mt6375_probe,
};
module_i2c_driver(mt6375_driver);

MODULE_AUTHOR("ShuFan Lee <shufan_lee@richtek.com>");
MODULE_DESCRIPTION("MediaTek MT6375 charger PMIC core");
MODULE_LICENSE("GPL");
