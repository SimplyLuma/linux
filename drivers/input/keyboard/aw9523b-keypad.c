// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW9523B used as a key-matrix scanner (Unihertz Titan 2 Elite).
 *
 * Rows are the expander inputs (P0_0..P1_0 by default), columns are driven
 * low one at a time by switching that pin from input to output (its output
 * latch is held low). At rest every column is driven low so any key press
 * changes an input and raises the expander interrupt.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define AW9523_IN0		0x00
#define AW9523_OUT0		0x02
#define AW9523_CFG0		0x04
#define AW9523_INT0		0x06
#define AW9523_ID		0x10
#define AW9523_GCR		0x11
#define AW9523_LEDMODE0		0x12
#define AW9523_SWRST		0x7f
#define AW9523_ID_VAL		0x23

#define AW9523_MAX_ROWS		16
#define AW9523_MAX_COLS		16
#define AW9523_POLL_MS		20

#define AW9523_MAX_LAYER_KEYS	4
#define AW9523_MAX_LAYER_MAP	64
#define AW9523_LAYER_SHIFT	BIT(16)	/* linux,layer-keymap: send with Shift held */
#define AW9523_DOUBLE_TAP_MS	400

struct aw9523b_keypad {
	struct i2c_client *client;
	struct input_dev *input;
	u16 row_mask;
	u16 col_mask;
	unsigned int nrows;
	unsigned int ncols;
	u8 row_bit[AW9523_MAX_ROWS];
	u8 col_bit[AW9523_MAX_COLS];
	u16 state[AW9523_MAX_COLS];	/* pressed rows (bitmap by row index) per column */
	unsigned short keycodes[AW9523_MAX_ROWS * AW9523_MAX_COLS];
	/* Second layer (the symbols printed on the keys), while a layer key is held. */
	u32 layer_keys[AW9523_MAX_LAYER_KEYS];
	unsigned int nlayer_keys;
	u32 layer_map[2 * AW9523_MAX_LAYER_MAP];
	unsigned int nlayer_map;
	unsigned int layer_held;
	u32 sent[AW9523_MAX_ROWS * AW9523_MAX_COLS];	/* what each pressed key reported */
	/* Double-tapping Shift toggles Caps Lock (the keyboard has no Caps Lock key). */
	unsigned long shift_tap;	/* jiffies of the last clean Shift tap, 0 if none */
	bool shift_used;		/* another key went down since Shift did */
};

/*
 * Port 0/1 register pairs are accessed one byte at a time: a 16-bit read
 * does not reach the port 1 input register, which then never releases the
 * interrupt.
 */
static int aw9523b_write16(struct aw9523b_keypad *kp, u8 reg, u16 val)
{
	int ret = i2c_smbus_write_byte_data(kp->client, reg, val & 0xff);

	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(kp->client, reg + 1, val >> 8);
}

static int aw9523b_read16(struct aw9523b_keypad *kp, u8 reg, u16 *val)
{
	int lo, hi;

	lo = i2c_smbus_read_byte_data(kp->client, reg);
	if (lo < 0)
		return lo;
	hi = i2c_smbus_read_byte_data(kp->client, reg + 1);
	if (hi < 0)
		return hi;
	*val = (hi << 8) | lo;
	return 0;
}

/* Inputs: rows and every column except @drive; columns stay latched low. */
static int aw9523b_drive(struct aw9523b_keypad *kp, int drive)
{
	u16 cfg = 0xffff;

	if (drive < 0)
		cfg &= ~kp->col_mask;
	else
		cfg &= ~BIT(kp->col_bit[drive]);
	return aw9523b_write16(kp, AW9523_CFG0, cfg);
}

static bool aw9523b_is_layer_key(struct aw9523b_keypad *kp, unsigned short keycode)
{
	for (unsigned int i = 0; i < kp->nlayer_keys; i++)
		if (kp->layer_keys[i] == keycode)
			return true;
	return false;
}

static u32 aw9523b_layer_code(struct aw9523b_keypad *kp, unsigned short keycode)
{
	for (unsigned int i = 0; i < kp->nlayer_map; i++)
		if (kp->layer_map[2 * i] == keycode)
			return kp->layer_map[2 * i + 1];
	return keycode;
}

/*
 * Report one key. A layer key only switches the layer; another key sends its
 * layer symbol while one is held. A release always ends what its press sent,
 * whichever key is let go first.
 */
static void aw9523b_report(struct aw9523b_keypad *kp, unsigned int code, bool down)
{
	struct input_dev *input = kp->input;
	unsigned short keycode = kp->keycodes[code];
	u32 out;

	if (aw9523b_is_layer_key(kp, keycode)) {
		if (down)
			kp->layer_held++;
		else if (kp->layer_held)
			kp->layer_held--;
		return;
	}

	if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
		if (down) {
			kp->shift_used = false;
		} else if (!kp->shift_used) {
			if (kp->shift_tap &&
			    time_before(jiffies, kp->shift_tap + msecs_to_jiffies(AW9523_DOUBLE_TAP_MS))) {
				input_report_key(input, KEY_CAPSLOCK, 1);
				input_report_key(input, KEY_CAPSLOCK, 0);
				kp->shift_tap = 0;
			} else {
				kp->shift_tap = jiffies;
			}
		}
	} else if (down) {
		kp->shift_used = true;
		kp->shift_tap = 0;
	}

	if (down) {
		out = kp->layer_held ? aw9523b_layer_code(kp, keycode) : keycode;
		kp->sent[code] = out;
		if (out & AW9523_LAYER_SHIFT)
			input_report_key(input, KEY_LEFTSHIFT, 1);
		input_report_key(input, out & KEY_MAX, 1);
	} else {
		out = kp->sent[code] ? kp->sent[code] : keycode;
		kp->sent[code] = 0;
		input_report_key(input, out & KEY_MAX, 0);
		if (out & AW9523_LAYER_SHIFT)
			input_report_key(input, KEY_LEFTSHIFT, 0);
	}
}

static int aw9523b_parse_layer(struct aw9523b_keypad *kp, struct device *dev)
{
	int n;

	n = device_property_count_u32(dev, "linux,layer-keys");
	if (n <= 0)
		return 0;
	if (n > AW9523_MAX_LAYER_KEYS)
		return dev_err_probe(dev, -EINVAL, "too many linux,layer-keys\n");
	device_property_read_u32_array(dev, "linux,layer-keys", kp->layer_keys, n);
	kp->nlayer_keys = n;

	n = device_property_count_u32(dev, "linux,layer-keymap");
	if (n < 0)
		n = 0;
	if (n % 2 || n > 2 * AW9523_MAX_LAYER_MAP)
		return dev_err_probe(dev, -EINVAL, "bad linux,layer-keymap\n");
	device_property_read_u32_array(dev, "linux,layer-keymap", kp->layer_map, n);
	kp->nlayer_map = n / 2;

	for (unsigned int i = 0; i < kp->nlayer_map; i++) {
		u32 to = kp->layer_map[2 * i + 1];

		if ((to & ~AW9523_LAYER_SHIFT) >= KEY_MAX)
			return dev_err_probe(dev, -EINVAL, "bad layer key %u\n", to);
		input_set_capability(kp->input, EV_KEY, to & KEY_MAX);
	}
	input_set_capability(kp->input, EV_KEY, KEY_LEFTSHIFT);
	return 0;
}

static void aw9523b_set_caps_capability(struct aw9523b_keypad *kp)
{
	input_set_capability(kp->input, EV_KEY, KEY_CAPSLOCK);
}

static void aw9523b_scan(struct aw9523b_keypad *kp)
{
	struct input_dev *input = kp->input;
	unsigned int c, r;
	u16 in, rows;
	int ret;

	/*
	 * Driving one column at a time changes the row inputs whenever a key
	 * is held, which would re-raise the interrupt: mask it while scanning.
	 */
	aw9523b_write16(kp, AW9523_INT0, 0xffff);

	for (c = 0; c < kp->ncols; c++) {
		ret = aw9523b_drive(kp, c);
		if (!ret)
			ret = aw9523b_read16(kp, AW9523_IN0, &in);
		if (ret) {
			dev_err_ratelimited(&kp->client->dev, "scan failed: %d\n", ret);
			break;
		}
		rows = 0;
		for (r = 0; r < kp->nrows; r++)
			if (!(in & BIT(kp->row_bit[r])))
				rows |= BIT(r);

		for (r = 0; r < kp->nrows; r++) {
			bool down = rows & BIT(r);
			unsigned int code;

			if (down == !!(kp->state[c] & BIT(r)))
				continue;
			code = MATRIX_SCAN_CODE(r, c, get_count_order(AW9523_MAX_COLS));
			input_event(input, EV_MSC, MSC_SCAN, code);
			aw9523b_report(kp, code, down);
		}
		kp->state[c] = rows;
	}
	input_sync(input);

	/*
	 * Back to rest: all columns low. Reading the inputs latches the at-rest
	 * state, so only a real change raises the next interrupt.
	 */
	aw9523b_drive(kp, -1);
	aw9523b_read16(kp, AW9523_IN0, &in);
	aw9523b_write16(kp, AW9523_INT0, (u16)~kp->row_mask);
}

static irqreturn_t aw9523b_irq(int irq, void *data)
{
	struct aw9523b_keypad *kp = data;

	usleep_range(3000, 4000);	/* debounce */
	aw9523b_scan(kp);
	return IRQ_HANDLED;
}

static void aw9523b_poll(struct input_dev *input)
{
	aw9523b_scan(input_get_drvdata(input));
}

static int aw9523b_hw_init(struct aw9523b_keypad *kp)
{
	u16 in;
	int ret;

	ret = i2c_smbus_read_byte_data(kp->client, AW9523_ID);
	if (ret < 0)
		return ret;
	if (ret != AW9523_ID_VAL)
		return dev_err_probe(&kp->client->dev, -ENODEV, "unexpected ID 0x%02x\n", ret);

	ret = i2c_smbus_write_byte_data(kp->client, AW9523_SWRST, 0);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	/* GPIO mode on all pins, P0 push-pull, output latches low. */
	ret = aw9523b_write16(kp, AW9523_LEDMODE0, 0xffff);
	if (!ret)
		ret = i2c_smbus_write_byte_data(kp->client, AW9523_GCR, BIT(4));
	if (!ret)
		ret = aw9523b_write16(kp, AW9523_OUT0, 0x0000);
	if (!ret)
		ret = aw9523b_drive(kp, -1);
	/* Interrupt on row changes only (0 = enabled). */
	if (!ret)
		ret = aw9523b_write16(kp, AW9523_INT0, (u16)~kp->row_mask);
	if (!ret)
		ret = aw9523b_read16(kp, AW9523_IN0, &in);
	return ret;
}

static int aw9523b_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw9523b_keypad *kp;
	struct gpio_desc *reset;
	struct input_dev *input;
	u32 mask;
	int ret, i;

	kp = devm_kzalloc(dev, sizeof(*kp), GFP_KERNEL);
	if (!kp)
		return -ENOMEM;
	kp->client = client;

	kp->row_mask = device_property_read_u32(dev, "awinic,row-mask", &mask) ? 0x01ff : mask;
	kp->col_mask = device_property_read_u32(dev, "awinic,col-mask", &mask) ? 0xfe00 : mask;
	if (kp->row_mask & kp->col_mask)
		return dev_err_probe(dev, -EINVAL, "row and column masks overlap\n");
	for (i = 0; i < 16; i++) {
		if (kp->row_mask & BIT(i))
			kp->row_bit[kp->nrows++] = i;
		if (kp->col_mask & BIT(i))
			kp->col_bit[kp->ncols++] = i;
	}

	ret = devm_regulator_get_enable_optional(dev, "vio");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "cannot enable vio supply\n");

	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "cannot get reset gpio\n");
	if (reset) {
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(reset, 0);
		usleep_range(5000, 6000);
	}

	ret = aw9523b_hw_init(kp);
	if (ret)
		return dev_err_probe(dev, ret, "chip init failed\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	kp->input = input;
	input->name = "AW9523B keyboard";
	input->phys = "aw9523b/input0";
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, kp);

	ret = matrix_keypad_build_keymap(NULL, NULL, kp->nrows, AW9523_MAX_COLS,
					 kp->keycodes, input);
	if (ret)
		return dev_err_probe(dev, ret, "bad keymap\n");
	input_set_capability(input, EV_MSC, MSC_SCAN);
	__set_bit(EV_REP, input->evbit);

	ret = aw9523b_parse_layer(kp, dev);
	if (ret)
		return ret;
	aw9523b_set_caps_capability(kp);

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL, aw9523b_irq,
						IRQF_ONESHOT, dev_name(dev), kp);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request irq\n");
	} else {
		ret = input_setup_polling(input, aw9523b_poll);
		if (ret)
			return ret;
		input_set_poll_interval(input, AW9523_POLL_MS);
	}

	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(dev, "%ux%u matrix, %s\n", kp->nrows, kp->ncols,
		 client->irq > 0 ? "interrupt driven" : "polled");
	return 0;
}

static const struct of_device_id aw9523b_keypad_of_match[] = {
	{ .compatible = "awinic,aw9523b-keypad" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aw9523b_keypad_of_match);

static struct i2c_driver aw9523b_keypad_driver = {
	.driver = {
		.name = "aw9523b-keypad",
		.of_match_table = aw9523b_keypad_of_match,
	},
	.probe = aw9523b_probe,
};
module_i2c_driver(aw9523b_keypad_driver);

MODULE_DESCRIPTION("Awinic AW9523B key matrix scanner");
MODULE_LICENSE("GPL");
