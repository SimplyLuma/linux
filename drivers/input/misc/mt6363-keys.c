// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6363 PMIC keys (SPMI), polled.
 *
 * The debounced key states are in TOPSTATUS, one bit per key, cleared while
 * the key is held: power on bit 1, home on bit 3, home2 on bit 4. Each key
 * is a child node named after it with its linux,keycodes; boards remap the
 * home keys (the Titan 2 Elite's left shift is home2).
 *
 * Other PMIC functions described as child nodes (the AUXADC) are populated
 * here and use this device's regmap.
 */

#include <linux/bits.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spmi.h>

#define MT6363_TOPSTATUS	0x1e
#define MT6363_KEYS_POLL_MS	30
#define MT6363_MAX_KEYS		3
#define MT6363_DOUBLE_TAP_MS	400

static const struct {
	const char *name;
	u8 bit;
} mt6363_key_bits[MT6363_MAX_KEYS] = {
	{ "power", 1 },
	{ "home", 3 },
	{ "home2", 4 },
};

struct mt6363_keys {
	struct regmap *regmap;
	unsigned int nkeys;
	u8 bit[MT6363_MAX_KEYS];
	u32 keycode[MT6363_MAX_KEYS];
	unsigned int pressed;	/* bitmap by key index */
	unsigned long shift_tap;	/* last Shift tap, for double-tap Caps Lock */
};

static void mt6363_keys_poll(struct input_dev *input)
{
	struct mt6363_keys *keys = input_get_drvdata(input);
	unsigned int val, now = 0;

	if (regmap_read(keys->regmap, MT6363_TOPSTATUS, &val))
		return;
	for (unsigned int i = 0; i < keys->nkeys; i++)
		if (!(val & BIT(keys->bit[i])))
			now |= BIT(i);
	if (now == keys->pressed)
		return;
	for (unsigned int i = 0; i < keys->nkeys; i++) {
		bool down = now & BIT(i);

		if (!((now ^ keys->pressed) & BIT(i)))
			continue;
		input_report_key(input, keys->keycode[i], down);
		/* A Shift wired here double-taps into Caps Lock, as on the keyboard. */
		if (keys->keycode[i] == KEY_LEFTSHIFT && !down) {
			if (keys->shift_tap &&
			    time_before(jiffies, keys->shift_tap + msecs_to_jiffies(MT6363_DOUBLE_TAP_MS))) {
				input_report_key(input, KEY_CAPSLOCK, 1);
				input_report_key(input, KEY_CAPSLOCK, 0);
				keys->shift_tap = 0;
			} else {
				keys->shift_tap = jiffies;
			}
		}
	}
	keys->pressed = now;
	input_sync(input);
}

static const struct regmap_config mt6363_keys_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1fff,
	.fast_io = true,
};

static int mt6363_keys_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct mt6363_keys *keys;
	struct input_dev *input;
	unsigned int val;
	int ret;

	keys = devm_kzalloc(dev, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;

	keys->regmap = devm_regmap_init_spmi_ext(sdev, &mt6363_keys_regmap_config);
	if (IS_ERR(keys->regmap))
		return dev_err_probe(dev, PTR_ERR(keys->regmap), "cannot init regmap\n");
	ret = regmap_read(keys->regmap, MT6363_TOPSTATUS, &val);
	if (ret)
		return dev_err_probe(dev, ret, "PMIC not responding\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	input->name = "MT6363 keys";
	input->phys = "mt6363/input0";
	input_set_drvdata(input, keys);

	for (unsigned int i = 0; i < MT6363_MAX_KEYS; i++) {
		struct fwnode_handle *child;
		u32 keycode;

		child = device_get_named_child_node(dev, mt6363_key_bits[i].name);
		if (!child)
			continue;
		ret = fwnode_property_read_u32(child, "linux,keycodes", &keycode);
		fwnode_handle_put(child);
		if (ret || keycode >= KEY_MAX)
			return dev_err_probe(dev, -EINVAL, "bad keycode for %s\n",
					     mt6363_key_bits[i].name);
		keys->bit[keys->nkeys] = mt6363_key_bits[i].bit;
		keys->keycode[keys->nkeys] = keycode;
		keys->nkeys++;
		input_set_capability(input, EV_KEY, keycode);
		if (keycode == KEY_LEFTSHIFT)
			input_set_capability(input, EV_KEY, KEY_CAPSLOCK);
	}
	if (!keys->nkeys) {
		/* Older device trees: the power key alone. */
		keys->bit[0] = 1;
		keys->keycode[0] = KEY_POWER;
		keys->nkeys = 1;
		input_set_capability(input, EV_KEY, KEY_POWER);
	}

	ret = input_setup_polling(input, mt6363_keys_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, MT6363_KEYS_POLL_MS);

	ret = input_register_device(input);
	if (ret)
		return ret;
	device_init_wakeup(dev, true);

	return devm_of_platform_populate(dev);
}

static const struct of_device_id mt6363_keys_of_match[] = {
	{ .compatible = "mediatek,mt6363-keys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6363_keys_of_match);

static struct spmi_driver mt6363_keys_driver = {
	.driver = {
		.name = "mt6363-keys",
		.of_match_table = mt6363_keys_of_match,
	},
	.probe = mt6363_keys_probe,
};
module_spmi_driver(mt6363_keys_driver);

MODULE_DESCRIPTION("MediaTek MT6363 PMIC keys");
MODULE_LICENSE("GPL");
