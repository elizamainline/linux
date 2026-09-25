// SPDX-License-Identifier: GPL-2.0-only
/*
 * BOE RM69260 panel used in the Nothing Phone (4a) Pro.
 * Initialization and timings are from the stock device tree.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct boe_rm69260 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator *vddio;
	struct regulator *dvdd;
	struct regulator *vci;
	struct gpio_desc *reset_gpio;
};

static inline struct boe_rm69260 *to_boe_rm69260(struct drm_panel *panel)
{
	return container_of(panel, struct boe_rm69260, panel);
}

static int boe_rm69260_power_on(struct boe_rm69260 *ctx)
{
	int ret;

	ret = regulator_enable(ctx->vddio);
	if (ret)
		return ret;

	usleep_range(2000, 3000);
	ret = regulator_enable(ctx->dvdd);
	if (ret)
		goto disable_vddio;

	usleep_range(4000, 5000);
	ret = regulator_enable(ctx->vci);
	if (ret)
		goto disable_dvdd;

	return 0;

disable_dvdd:
	regulator_disable(ctx->dvdd);
disable_vddio:
	regulator_disable(ctx->vddio);
	return ret;
}

static void boe_rm69260_power_off(struct boe_rm69260 *ctx)
{
	regulator_disable(ctx->vci);
	usleep_range(2000, 3000);
	regulator_disable(ctx->dvdd);
	usleep_range(4000, 5000);
	regulator_disable(ctx->vddio);
}

static int boe_rm69260_prepare(struct drm_panel *panel)
{
	struct boe_rm69260 *ctx = to_boe_rm69260(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = boe_rm69260_power_on(ctx);
	if (ret)
		return ret;

	/* Stock reset sequence: high, low, high, each held for 5 ms. */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0xa0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x06, 0x76);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x7c, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x42);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x78, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0xd4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x40, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x42, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0xf7);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x83, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0xa1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x74, 0x72);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc4, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x3f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x67);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd0, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0xa4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x9c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x12, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbc, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x23, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x9a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x64, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbf, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x9b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x52, 0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x14, 0x32);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3a, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x37, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfa, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 105);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);
	mipi_dsi_msleep(&dsi_ctx, 28);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		boe_rm69260_power_off(ctx);
	}

	return dsi_ctx.accum_err;
}

static int boe_rm69260_unprepare(struct drm_panel *panel)
{
	struct boe_rm69260 *ctx = to_boe_rm69260(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 35);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 80);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	boe_rm69260_power_off(ctx);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode boe_rm69260_mode = {
	.clock = (1260 + 204 + 12 + 4) * (2800 + 88 + 62 + 2) * 120 / 1000,
	.hdisplay = 1260,
	.hsync_start = 1260 + 204,
	.hsync_end = 1260 + 204 + 4,
	.htotal = 1260 + 204 + 4 + 12,
	.vdisplay = 2800,
	.vsync_start = 2800 + 88,
	.vsync_end = 2800 + 88 + 2,
	.vtotal = 2800 + 88 + 2 + 62,
	.width_mm = 71,
	.height_mm = 158,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int boe_rm69260_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &boe_rm69260_mode);
}

static const struct drm_panel_funcs boe_rm69260_panel_funcs = {
	.prepare = boe_rm69260_prepare,
	.unprepare = boe_rm69260_unprepare,
	.get_modes = boe_rm69260_get_modes,
};

static int boe_rm69260_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	unsigned long mode_flags = dsi->mode_flags;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags = mode_flags;
	return ret < 0 ? ret : 0;
}

static const struct backlight_ops boe_rm69260_bl_ops = {
	.update_status = boe_rm69260_bl_update_status,
};

static int boe_rm69260_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1024,
		.max_brightness = 16383,
	};
	struct boe_rm69260 *ctx;
	struct backlight_device *backlight;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct boe_rm69260, panel,
				   &boe_rm69260_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio), "Failed to get vddio\n");

	ctx->dvdd = devm_regulator_get(dev, "dvdd");
	if (IS_ERR(ctx->dvdd))
		return dev_err_probe(dev, PTR_ERR(ctx->dvdd), "Failed to get dvdd\n");

	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci), "Failed to get vci\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 8;
	ctx->dsc.slice_width = 630;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;

	dsi->dsc = &ctx->dsc;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;
	backlight = devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
						   &boe_rm69260_bl_ops, &props);
	if (IS_ERR(backlight))
		return dev_err_probe(dev, PTR_ERR(backlight),
				     "Failed to register backlight\n");
	ctx->panel.backlight = backlight;

	ret = devm_drm_panel_add(dev, &ctx->panel);
	if (ret)
		return ret;

	return devm_mipi_dsi_attach(dev, dsi);
}

static const struct of_device_id boe_rm69260_of_match[] = {
	{ .compatible = "boe,rm69260-froggerpro" },
	{ }
};
MODULE_DEVICE_TABLE(of, boe_rm69260_of_match);

static struct mipi_dsi_driver boe_rm69260_driver = {
	.probe = boe_rm69260_probe,
	.driver = {
		.name = "panel-boe-rm69260",
		.of_match_table = boe_rm69260_of_match,
	},
};
module_mipi_dsi_driver(boe_rm69260_driver);

MODULE_DESCRIPTION("BOE RM69260 panel for Nothing Phone (4a) Pro");
MODULE_LICENSE("GPL");
