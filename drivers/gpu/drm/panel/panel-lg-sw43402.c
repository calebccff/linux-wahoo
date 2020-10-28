// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2020 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct lg_sw43402 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct lg_sw43402 *to_lg_sw43402(struct drm_panel *panel)
{
	return container_of(panel, struct lg_sw43402, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void lg_sw43402_reset(struct lg_sw43402 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int lg_sw43402_on(struct lg_sw43402 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi_dcs_write_seq(dsi, 0xb0, 0x20, 0x43);
	dsi_dcs_write_seq(dsi, 0xb0, 0xa5, 0x00);
	dsi_dcs_write_seq(dsi, 0xb2,
			  0x5d, 0x01, 0x02, 0x80, 0x00, 0xff, 0xff, 0x15, 0x00,
			  0x00, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0x35);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(60);

	dsi_dcs_write_seq(dsi, 0xe7,
			  0x00, 0x0d, 0x76, 0x23, 0x00, 0x00, 0x5d, 0x44, 0x0d,
			  0x76, 0x0d, 0x0d, 0x00, 0x0d, 0x0d, 0x0d, 0x4a, 0x00);
	dsi_dcs_write_seq(dsi, 0x53, 0x00);
	dsi_dcs_write_seq(dsi, 0x55, 0x0c);
	dsi_dcs_write_seq(dsi, 0xfb, 0x03, 0x77);
	dsi_dcs_write_seq(dsi, 0xed, 0x13, 0x00, 0x06, 0x00, 0x00);
	dsi_dcs_write_seq(dsi, 0xe2,
			  0x20, 0x0d, 0x08, 0xa8, 0x0a, 0xaa, 0x04, 0x44, 0x80,
			  0x80, 0x80, 0x5c, 0x5c, 0x5c);
	msleep(90);
	dsi_dcs_write_seq(dsi, 0xe7,
			  0x00, 0x0d, 0x76, 0x23, 0x00, 0x00, 0x0d, 0x44, 0x0d,
			  0x76, 0x0d, 0x0d, 0x00, 0x0d, 0x0d, 0x0d, 0x4a, 0x00);
	msleep(20);

	return 0;
}

static int lg_sw43402_off(struct lg_sw43402 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi_dcs_write_seq(dsi, 0xe8, 0x08, 0x90, 0x18, 0x05);

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(100);

	return 0;
}

static int lg_sw43402_prepare(struct drm_panel *panel)
{
	struct lg_sw43402 *ctx = to_lg_sw43402(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	lg_sw43402_reset(ctx);

	ret = lg_sw43402_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int lg_sw43402_unprepare(struct drm_panel *panel)
{
	struct lg_sw43402 *ctx = to_lg_sw43402(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = lg_sw43402_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode lg_sw43402_mode = {
	.clock = (1440 + 20 + 32 + 20) * (2880 + 20 + 4 + 20) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 20,
	.hsync_end = 1440 + 20 + 32,
	.htotal = 1440 + 20 + 32 + 20,
	.vdisplay = 2880,
	.vsync_start = 2880 + 20,
	.vsync_end = 2880 + 20 + 4,
	.vtotal = 2880 + 20 + 4 + 20,
	.width_mm = 68,
	.height_mm = 136,
};

static int lg_sw43402_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &lg_sw43402_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs lg_sw43402_panel_funcs = {
	.prepare = lg_sw43402_prepare,
	.unprepare = lg_sw43402_unprepare,
	.get_modes = lg_sw43402_get_modes,
};

static int lg_sw43402_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lg_sw43402 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &lg_sw43402_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0) {
		dev_err(dev, "Failed to add panel: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		return ret;
	}

	return 0;
}

static int lg_sw43402_remove(struct mipi_dsi_device *dsi)
{
	struct lg_sw43402 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id lg_sw43402_of_match[] = {
	{ .compatible = "lge,sw43402" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lg_sw43402_of_match);

static struct mipi_dsi_driver lg_sw43402_driver = {
	.probe = lg_sw43402_probe,
	.remove = lg_sw43402_remove,
	.driver = {
		.name = "panel-sw43402",
		.of_match_table = lg_sw43402_of_match,
	},
};
module_mipi_dsi_driver(lg_sw43402_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>"); // FIXME
MODULE_DESCRIPTION("DRM driver for SW43402 cmd mode dsc dsi panel");
MODULE_LICENSE("GPL v2");
