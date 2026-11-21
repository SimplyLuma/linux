// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMS644VK04 AMOLED DSI command mode panel driver (DSC)
 *
 * 1080x2400, 4 lanes, DSC 1.1 8 bpc -> 8 bpp, 60 Hz and 90 Hz.
 * Found in the OnePlus Nord (avicii).
 *
 * The initialisation sequences were generated from the downstream panel
 * description ("samsung ams644vk04 fhd cmd mode dsc dsi panel").
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define AMS644VK04_WIDTH		1080
#define AMS644VK04_HEIGHT		2400

#define AMS644VK04_MAX_BRIGHTNESS	0x7ff
#define AMS644VK04_DEFAULT_BRIGHTNESS	0x140

enum {
	AMS644VK04_SUPPLY_VDDIO,
	AMS644VK04_SUPPLY_VOUT,
	AMS644VK04_SUPPLY_VDD,
	AMS644VK04_NUM_SUPPLIES,
};

/* Power-on order, post-on delay and load as given by the downstream supply entries */
static const struct ams644vk04_supply_info {
	const char *name;
	unsigned int enable_load_ua;
	unsigned int disable_load_ua;
	unsigned int post_on_ms;
} ams644vk04_supply_info[AMS644VK04_NUM_SUPPLIES] = {
	[AMS644VK04_SUPPLY_VDDIO] = { "vddio", 62000, 80, 20 },
	[AMS644VK04_SUPPLY_VOUT] = { "vout", 857000, 0, 20 },
	[AMS644VK04_SUPPLY_VDD] = { "vdd", 857000, 0, 0 },
};

struct ams644vk04 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_connector *connector;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator *supplies[AMS644VK04_NUM_SUPPLIES];
	bool prepared;
};

static inline struct ams644vk04 *to_ams644vk04(struct drm_panel *panel)
{
	return container_of(panel, struct ams644vk04, panel);
}

/*
 * DSC Picture Parameter Set as sent by the downstream driver: DCS 0x9e with
 * the first 88 bytes of the PPS.  It is not what the DRM DSC helpers compute
 * for the same slice geometry (SCR1-style rate control parameters), but it is
 * the combination known to work with this panel.
 */
static const u8 ams644vk04_pps_cmd[] = {
	0x9e, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09,
	0x60, 0x04, 0x38, 0x00, 0x30, 0x02, 0x1c, 0x02,
	0x1c, 0x02, 0x00, 0x02, 0x52, 0x00, 0x20, 0x04,
	0x23, 0x00, 0x07, 0x00, 0x0f, 0x02, 0x8e, 0x02,
	0x1f, 0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20,
	0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a,
	0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79,
	0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09,
	0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19,
	0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a,
	0xb6, 0x2a, 0xf4, 0x2a, 0xf4, 0x4b, 0x34, 0x63,
	0x74,
};

static void ams644vk04_send_on(struct mipi_dsi_multi_context *dsi_ctx, bool hz90)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x9d, 0x01);
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, ams644vk04_pps_cmd, sizeof(ams644vk04_pps_cmd));
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_set_tear_on_multi(dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_set_column_address_multi(dsi_ctx, 0, AMS644VK04_WIDTH - 1);
	mipi_dsi_dcs_set_page_address_multi(dsi_ctx, 0, AMS644VK04_HEIGHT - 1);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf5, 0x85);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf8, 0xc4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf8, 0x41, 0x47, 0x11, 0xb8, 0xb8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf8, 0xcc);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf2, 0x82);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf5, 0x87);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdd, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_msleep(dsi_ctx, 110);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x06);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x05);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7, 0x13);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7, 0x01, 0x37);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	if (hz90)
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x60, 0x10);
	else
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x60, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
	if (hz90) {
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe6, 0x00);
	} else {
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe6, 0x02);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x69);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe6, 0x02);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x77);
		mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe6, 0xb3);
	}
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);
}

static void ams644vk04_send_off(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_set_display_off_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 10);
	mipi_dsi_dcs_enter_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 120);
}

static void ams644vk04_reset(struct ams644vk04 *ctx)
{
	/* Downstream: <1 10>, <0 10>, <1 10> with 1 = out of reset */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 10100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 10100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 10100);
}

static void ams644vk04_supplies_disable(struct ams644vk04 *ctx, int first)
{
	int i;

	for (i = first; i >= 0; i--) {
		if (!ctx->supplies[i])
			continue;

		regulator_set_load(ctx->supplies[i],
				   ams644vk04_supply_info[i].disable_load_ua);
		regulator_disable(ctx->supplies[i]);
	}
}

static int ams644vk04_supplies_enable(struct ams644vk04 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int i, ret;

	for (i = 0; i < AMS644VK04_NUM_SUPPLIES; i++) {
		if (!ctx->supplies[i])
			continue;

		/* Best effort, only matters if the regulator can switch modes */
		regulator_set_load(ctx->supplies[i],
				   ams644vk04_supply_info[i].enable_load_ua);

		ret = regulator_enable(ctx->supplies[i]);
		if (ret) {
			dev_err(dev, "Failed to enable %s: %d\n",
				ams644vk04_supply_info[i].name, ret);
			ams644vk04_supplies_disable(ctx, i - 1);
			return ret;
		}

		if (ams644vk04_supply_info[i].post_on_ms)
			msleep(ams644vk04_supply_info[i].post_on_ms);
	}

	return 0;
}

static bool ams644vk04_want_90hz(struct ams644vk04 *ctx)
{
	struct drm_connector *connector = ctx->connector;

	if (!connector || !connector->state || !connector->state->crtc ||
	    !connector->state->crtc->state)
		return false;

	return drm_mode_vrefresh(&connector->state->crtc->state->mode) > 75;
}

static int ams644vk04_prepare(struct drm_panel *panel)
{
	struct ams644vk04 *ctx = to_ams644vk04(panel);
	struct device *dev = &ctx->dsi->dev;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	bool hz90 = ams644vk04_want_90hz(ctx);
	int ret;

	dev_info(dev, "prepare: start (%s mode)\n", hz90 ? "90 Hz" : "60 Hz");

	ret = ams644vk04_supplies_enable(ctx);
	if (ret)
		return ret;

	ams644vk04_reset(ctx);

	ams644vk04_send_on(&dsi_ctx, hz90);
	if (dsi_ctx.accum_err) {
		ret = dsi_ctx.accum_err;
		dev_err(dev, "Failed to send on sequence: %d\n", ret);
		goto err_power_down;
	}

	ctx->prepared = true;
	dev_info(dev, "prepare: done\n");

	return 0;

err_power_down:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	ams644vk04_supplies_disable(ctx, AMS644VK04_NUM_SUPPLIES - 1);

	return ret;
}

static int ams644vk04_unprepare(struct drm_panel *panel)
{
	struct ams644vk04 *ctx = to_ams644vk04(panel);
	struct device *dev = &ctx->dsi->dev;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->prepared = false;

	ams644vk04_send_off(&dsi_ctx);
	if (dsi_ctx.accum_err)
		dev_err(dev, "Failed to send off sequence: %d\n", dsi_ctx.accum_err);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	ams644vk04_supplies_disable(ctx, AMS644VK04_NUM_SUPPLIES - 1);

	return 0;
}

/*
 * Timings are the downstream ones.  The clock is htotal * vtotal * refresh:
 * the msm DSI host scales the pixel clock for DSC by itself, so the mode
 * describes the uncompressed picture.  The large 60 Hz vertical front porch
 * is what keeps both modes on the same pixel clock.
 */
static const struct drm_display_mode ams644vk04_modes[] = {
	{	/* 60 Hz */
		.clock = (1080 + 52 + 24 + 24) * (2400 + 1208 + 4 + 8) * 60 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 52,
		.hsync_end = 1080 + 52 + 24,
		.htotal = 1080 + 52 + 24 + 24,
		.vdisplay = 2400,
		.vsync_start = 2400 + 1208,
		.vsync_end = 2400 + 1208 + 4,
		.vtotal = 2400 + 1208 + 4 + 8,
		.width_mm = 67,
		.height_mm = 149,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	{	/* 90 Hz */
		.clock = (1080 + 52 + 24 + 24) * (2400 + 4 + 4 + 8) * 90 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 52,
		.hsync_end = 1080 + 52 + 24,
		.htotal = 1080 + 52 + 24 + 24,
		.vdisplay = 2400,
		.vsync_start = 2400 + 4,
		.vsync_end = 2400 + 4 + 4,
		.vtotal = 2400 + 4 + 4 + 8,
		.width_mm = 67,
		.height_mm = 149,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int ams644vk04_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct ams644vk04 *ctx = to_ams644vk04(panel);
	int i, count = 0;

	for (i = 0; i < ARRAY_SIZE(ams644vk04_modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &ams644vk04_modes[i]);
		if (!mode)
			return count;

		drm_mode_probed_add(connector, mode);
		count++;
	}

	connector->display_info.width_mm = ams644vk04_modes[0].width_mm;
	connector->display_info.height_mm = ams644vk04_modes[0].height_mm;
	connector->display_info.bpc = 8;
	ctx->connector = connector;

	return count;
}

static const struct drm_panel_funcs ams644vk04_panel_funcs = {
	.prepare = ams644vk04_prepare,
	.unprepare = ams644vk04_unprepare,
	.get_modes = ams644vk04_get_modes,
};

static int ams644vk04_bl_update_status(struct backlight_device *bl)
{
	struct ams644vk04 *ctx = bl_get_data(bl);

	if (!ctx->prepared)
		return 0;

	/* The panel takes the 11 bit level most significant byte first */
	return mipi_dsi_dcs_set_display_brightness_large(ctx->dsi,
							 backlight_get_brightness(bl));
}

static const struct backlight_ops ams644vk04_bl_ops = {
	.update_status = ams644vk04_bl_update_status,
};

static int ams644vk04_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = AMS644VK04_DEFAULT_BRIGHTNESS,
		.max_brightness = AMS644VK04_MAX_BRIGHTNESS,
	};
	struct backlight_device *bl;
	struct ams644vk04 *ctx;
	int i, ret;

	ctx = devm_drm_panel_alloc(dev, struct ams644vk04, panel,
				   &ams644vk04_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* The supplies are optional: a missing one is simply not controlled */
	for (i = 0; i < AMS644VK04_NUM_SUPPLIES; i++) {
		struct regulator *reg;

		reg = devm_regulator_get_optional(dev, ams644vk04_supply_info[i].name);
		if (IS_ERR(reg)) {
			if (PTR_ERR(reg) != -ENODEV)
				return dev_err_probe(dev, PTR_ERR(reg),
						     "Failed to get %s supply\n",
						     ams644vk04_supply_info[i].name);
			continue;
		}

		ctx->supplies[i] = reg;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS |
			  MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;

	/*
	 * DSC 1.1, 8 bpc, 8 bpp (4 fractional bits), block prediction, two
	 * 540x48 slices per line, both in the same packet.  The DSI host
	 * derives the remaining rate control parameters from this.
	 */
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_width = 540;
	ctx->dsc.slice_height = 48;
	ctx->dsc.slice_count = AMS644VK04_WIDTH / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	dsi->dsc = &ctx->dsc;

	ctx->panel.prepare_prev_first = true;

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					    &ams644vk04_bl_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl), "Failed to register backlight\n");
	ctx->panel.backlight = bl;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void ams644vk04_remove(struct mipi_dsi_device *dsi)
{
	struct ams644vk04 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ams644vk04_of_match[] = {
	{ .compatible = "samsung,ams644vk04" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ams644vk04_of_match);

static struct mipi_dsi_driver ams644vk04_driver = {
	.probe = ams644vk04_probe,
	.remove = ams644vk04_remove,
	.driver = {
		.name = "panel-samsung-ams644vk04",
		.of_match_table = ams644vk04_of_match,
	},
};
module_mipi_dsi_driver(ams644vk04_driver);

MODULE_DESCRIPTION("Panel driver for the Samsung AMS644VK04 AMOLED DSI panel");
MODULE_LICENSE("GPL");
