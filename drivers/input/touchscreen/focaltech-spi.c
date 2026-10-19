// SPDX-License-Identifier: GPL-2.0
/*
 * FocalTech SPI touchscreen controllers (FT3383 on the Unihertz Titan 2 Elite).
 *
 * Frames follow the FocalTech SPI protocol: a 4-byte header (register,
 * command, 16-bit length) and three dummy bytes, then the payload. Reads
 * set the CRC flag and the controller appends a CRC-16 (reflected poly
 * 0x8408, initial value 0xffff, little endian) to the returned data.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define FTS_CMD_READ		0x80
#define FTS_CMD_CRC		0x20
#define FTS_HDR_LEN		4
#define FTS_DUMMY_LEN		3
#define FTS_CRC_LEN		2
#define FTS_CS_HIGH_US		150
#define FTS_RETRIES		3
#define FTS_BUF_LEN		128

#define FTS_REG_POINTS		0x01
#define FTS_REG_CHIP_ID_LOW	0x9f
#define FTS_REG_CHIP_ID		0xa3
#define FTS_REG_FW_VER		0xa6

#define FTS_MAX_POINTS		10
#define FTS_POINT_LEN		6
/* Data from register 0x01: a 4-byte header ([1] low nibble = point count), then 6 bytes per point. */
#define FTS_HDR_POINTS		4
#define FTS_DATA_LEN		(FTS_HDR_POINTS + FTS_POINT_LEN * FTS_MAX_POINTS)

#define FTS_EVENT_DOWN		0
#define FTS_EVENT_UP		1
#define FTS_EVENT_CONTACT	2

static bool dump;
module_param(dump, bool, 0644);
MODULE_PARM_DESC(dump, "Log raw touch reports");

struct fts_spi {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset;
	u32 edge_margin[4];	/* left, top, right, bottom, in screen pixels */
	u8 *tx;
	u8 *rx;
};

static u16 fts_crc(const u8 *data, size_t len)
{
	u16 crc = 0xffff;
	int i;

	while (len--) {
		crc ^= *data++;
		for (i = 0; i < 8; i++)
			crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

static int fts_read(struct fts_spi *ts, u8 reg, u8 *buf, size_t len)
{
	size_t data = FTS_HDR_LEN + FTS_DUMMY_LEN;
	size_t total = data + len + FTS_CRC_LEN;
	struct spi_transfer t = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = total,
	};
	int ret = -EIO, i;

	if (total > FTS_BUF_LEN)
		return -EINVAL;

	for (i = 0; i < FTS_RETRIES; i++) {
		memset(ts->tx, 0, total);
		ts->tx[0] = reg;
		ts->tx[1] = FTS_CMD_READ | FTS_CMD_CRC;
		ts->tx[2] = len >> 8;
		ts->tx[3] = len & 0xff;
		ret = spi_sync_transfer(ts->spi, &t, 1);
		udelay(FTS_CS_HIGH_US);
		if (ret)
			continue;
		if (fts_crc(ts->rx + data, len) == get_unaligned_le16(ts->rx + data + len)) {
			memcpy(buf, ts->rx + data, len);
			return 0;
		}
		ret = -EBADMSG;
	}
	return ret;
}

#define FTS_EDGE_BAND		96

/*
 * The panel reports positions near its edges compressed inward, so a finger
 * on the bezel never reaches the outermost pixels that edge gestures use.
 * Stretch the outer band of each edge back out to the screen border.
 */
static unsigned int fts_stretch(unsigned int v, unsigned int max, u32 lo, u32 hi)
{
	if (lo && v < FTS_EDGE_BAND)
		v = v <= lo ? 0 : (v - lo) * FTS_EDGE_BAND / (FTS_EDGE_BAND - lo);
	if (hi && v > max - FTS_EDGE_BAND)
		v = v >= max - hi ? max :
		    max - (max - hi - v) * FTS_EDGE_BAND / (FTS_EDGE_BAND - hi);
	return v;
}

static void fts_report(struct fts_spi *ts, const u8 *buf)
{
	struct input_dev *input = ts->input;
	int i;

	for (i = 0; i < FTS_MAX_POINTS; i++) {
		const u8 *p = buf + FTS_HDR_POINTS + i * FTS_POINT_LEN;
		unsigned int event = p[0] >> 6, id = p[2] >> 4;
		unsigned int x = ((p[0] & 0x0f) << 8) | p[1];
		unsigned int y = ((p[2] & 0x0f) << 8) | p[3];

		if (id >= FTS_MAX_POINTS)
			continue;
		input_mt_slot(input, id);
		if (event == FTS_EVENT_UP) {
			input_mt_report_slot_inactive(input);
			continue;
		}
		if (event != FTS_EVENT_DOWN && event != FTS_EVENT_CONTACT)
			continue;
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		if (ts->prop.invert_x)
			x = ts->prop.max_x - min(x, ts->prop.max_x);
		if (ts->prop.invert_y)
			y = ts->prop.max_y - min(y, ts->prop.max_y);
		x = fts_stretch(min(x, ts->prop.max_x), ts->prop.max_x,
				ts->edge_margin[0], ts->edge_margin[2]);
		y = fts_stretch(min(y, ts->prop.max_y), ts->prop.max_y,
				ts->edge_margin[1], ts->edge_margin[3]);
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_PRESSURE, p[4] ? p[4] : 0x3f);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, p[5] >> 4);
	}
	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t fts_irq(int irq, void *data)
{
	struct fts_spi *ts = data;
	u8 buf[FTS_DATA_LEN];

	if (fts_read(ts, FTS_REG_POINTS, buf, sizeof(buf))) {
		dev_err_ratelimited(&ts->spi->dev, "touch read failed\n");
		return IRQ_HANDLED;
	}
	if (dump)
		dev_info(&ts->spi->dev, "%*ph\n", FTS_HDR_POINTS + FTS_POINT_LEN * 2, buf);
	fts_report(ts, buf);
	return IRQ_HANDLED;
}


static int fts_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct gpio_desc *iovcc;
	struct input_dev *input;
	struct fts_spi *ts;
	u8 id_hi, id_lo, fw;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->spi = spi;
	ts->tx = devm_kzalloc(dev, 2 * FTS_BUF_LEN, GFP_KERNEL | GFP_DMA);
	if (!ts->tx)
		return -ENOMEM;
	ts->rx = ts->tx + FTS_BUF_LEN;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable vdd\n");
	iovcc = devm_gpiod_get_optional(dev, "iovcc", GPIOD_OUT_HIGH);
	if (IS_ERR(iovcc))
		return dev_err_probe(dev, PTR_ERR(iovcc), "cannot get iovcc gpio\n");
	ts->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset), "cannot get reset gpio\n");
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ts->reset, 0);
	msleep(200);

	ret = fts_read(ts, FTS_REG_CHIP_ID, &id_hi, 1);
	if (!ret)
		ret = fts_read(ts, FTS_REG_CHIP_ID_LOW, &id_lo, 1);
	if (!ret)
		ret = fts_read(ts, FTS_REG_FW_VER, &fw, 1);
	if (ret)
		return dev_err_probe(dev, ret, "controller not responding\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	ts->input = input;
	input->name = "FocalTech touchscreen";
	input->id.bustype = BUS_SPI;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 15, 0, 0);
	touchscreen_parse_properties(input, true, &ts->prop);
	if (ts->prop.swap_x_y)
		return dev_err_probe(dev, -EINVAL, "touchscreen-swapped-x-y is not supported\n");
	device_property_read_u32_array(dev, "focaltech,edge-margins", ts->edge_margin,
				       ARRAY_SIZE(ts->edge_margin));
	ret = input_mt_init_slots(input, FTS_MAX_POINTS, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, spi->irq, NULL, fts_irq, IRQF_ONESHOT,
					dev_name(dev), ts);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request irq\n");

	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(dev, "chip 0x%02x%02x, firmware 0x%02x\n", id_hi, id_lo, fw);
	return 0;
}

static const struct of_device_id fts_of_match[] = {
	{ .compatible = "focaltech,ft3383" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fts_of_match);

static const struct spi_device_id fts_spi_ids[] = {
	{ "ft3383" },
	{ }
};
MODULE_DEVICE_TABLE(spi, fts_spi_ids);

static struct spi_driver fts_driver = {
	.driver = {
		.name = "focaltech-spi",
		.of_match_table = fts_of_match,
	},
	.id_table = fts_spi_ids,
	.probe = fts_probe,
};
module_spi_driver(fts_driver);

MODULE_DESCRIPTION("FocalTech SPI touchscreen driver");
MODULE_LICENSE("GPL");
