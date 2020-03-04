// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_dsc.h>

#include <video/mipi_display.h>

struct panel_cmd {
	size_t len;
	const char *data;
};

#define _INIT_CMD(...) { \
	.len = sizeof((char[]){__VA_ARGS__}), \
	.data = (char[]){__VA_ARGS__} }

static const char * const regulator_names[] = {
	"vddi",
	"vpnl",
	"lab",
};

static unsigned long const regulator_enable_loads[] = {
	62000,
	857000,
	100000,
};

static unsigned long const regulator_disable_loads[] = {
	80,
	0,
	100,
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	const char *panel_name;

	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	const struct panel_cmd *on_cmds_1;
	const struct panel_cmd *on_cmds_2;
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct panel_desc *desc;

	struct backlight_device *backlight;
	u32 brightness;
	u32 max_brightness;

	u32 init_delay_us;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *reset_gpio;

	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;

	bool prepared;
	bool enabled;
	bool first_enable;
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, base);
}


/*
 * Need to reset gpios and regulators once in the beginning,
 */
static int panel_reset_at_beginning(struct panel_info * pinfo)
{
	int ret = 0, i;

	DRM_DEV_ERROR(pinfo->base.dev,
					"panel_reset_at_beginning\n");
	/* enable supplies */
	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	/* Disable supplies */
	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(pinfo->base.dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Reset sequence of LG sw43408 panel requires the panel to be
	 * out of reset for 9ms, followed by being held in reset
	 * for 1ms and then out again
	 */
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(9000, 10000);
	gpiod_set_value(pinfo->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(9000, 10000);

	return 0;
}

static int send_mipi_cmds(struct drm_panel *panel, const struct panel_cmd *cmds)
{
	struct panel_info *pinfo = to_panel_info(panel);
	unsigned int i = 0;
	int err;

	if (!cmds)
		return -EFAULT;

	for (i = 0; cmds[i].len != 0; i++) {
		const struct panel_cmd *cmd = &cmds[i];

		if (cmd->len == 2)
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], NULL, 0);
		else
			err = mipi_dsi_dcs_write(pinfo->link,
						    cmd->data[1], cmd->data + 2,
						    cmd->len - 2);

		if (err < 0)
			return err;

		usleep_range((cmd->data[0]) * 1000,
			    (1 + cmd->data[0]) * 1000);
	}

	return 0;
}

static int panel_set_pinctrl_state(struct panel_info *panel, bool enable)
{
	int rc = 0;
	struct pinctrl_state *state;

	if (enable)
		state = panel->active;
	else
		state = panel->suspend;

	rc = pinctrl_select_state(panel->pinctrl, state);
	if (rc)
		pr_err("[%s] failed to set pin state, rc=%d\n", panel->desc->panel_name,
			rc);
	return rc;
}

static int lg_panel_disable(struct drm_panel *panel)
{
/* HACK: return 0 for now */

#if 0
	struct panel_info *pinfo = to_panel_info(panel);

	backlight_disable(pinfo->backlight);

	pinfo->enabled = false;
#endif
	return 0;
}

static int lg_panel_power_off(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i, ret = 0;
	gpiod_set_value(pinfo->reset_gpio, 0);

        ret = panel_set_pinctrl_state(pinfo, false);
        if (ret) {
                pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
        }

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
				regulator_disable_loads[i]);
		if (ret) {
			DRM_DEV_ERROR(panel->dev,
				"regulator_set_load failed %d\n", ret);
			return ret;
		}
	}

	ret = regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
			"regulator_bulk_disable failed %d\n", ret);
	}
	return ret;
}

static int lg_panel_unprepare(struct drm_panel *panel)
{
/* HACK : Currently, after a suspend, the resume doesn't enable screen, so
 *        don't disable the panel until we figure out why that is.
 */
return 0;

#if 0
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	if (!pinfo->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"set_display_off cmd failed ret = %d\n",
			ret);
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_enter_sleep_mode(pinfo->link);
	if (ret < 0) {
		DRM_DEV_ERROR(panel->dev,
			"enter_sleep cmd failed ret = %d\n", ret);
	}
	/* 0x64 = 100ms delay */
	msleep(100);

	ret = lg_panel_power_off(panel);
	if (ret < 0)
		DRM_DEV_ERROR(panel->dev, "power_off failed ret = %d\n", ret);

	pinfo->prepared = false;

	return ret;
#endif
}

static int lg_panel_power_on(struct panel_info *pinfo)
{
	int ret, i;
	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++) {
		ret = regulator_set_load(pinfo->supplies[i].consumer,
					regulator_enable_loads[i]);
		if (ret)
			return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0)
		return ret;

	ret = panel_set_pinctrl_state(pinfo, true);
	if (ret) {
		pr_err("[%s] failed to set pinctrl, rc=%d\n", pinfo->desc->panel_name, ret);
		return ret;
	}

	/*
	 * Reset sequence of LG sw43408 panel requires the panel to be
	 * out of reset for 9ms, followed by being held in reset
	 * for 1ms and then out again
	 * For now dont set this sequence as it causes panel to not come
	 * back
	 */
	//gpiod_set_value(pinfo->reset_gpio, 1);
	//usleep_range(9000, 10000);
	//gpiod_set_value(pinfo->reset_gpio, 0);
	//usleep_range(1000, 2000);
	//gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(9000, 10000);

	return 0;
}

static int lg_panel_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	if (unlikely(pinfo->first_enable)) {
		pinfo->first_enable = false;
		err = panel_reset_at_beginning(pinfo);
		if (err < 0) {
		pr_err("sw43408 panel_reset_at_beginning failed: %d\n", err);
			return err;
		}
	}

	if (pinfo->prepared)
		return 0;

	usleep_range(pinfo->init_delay_us, pinfo->init_delay_us);

	err = lg_panel_power_on(pinfo);
	if (err < 0)
		goto poweroff;

	/* send first part of init cmds */
	err = send_mipi_cmds(panel, pinfo->desc->on_cmds_1);

	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to send DCS Init 1st Code: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_exit_sleep_mode(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev, "failed to exit sleep mode: %d\n",
			      err);
		goto poweroff;
	}
	/* 0x87 = 135 ms delay */
	msleep(135);

	/* Set DCS_COMPRESSION_MODE */
	err = mipi_dsi_dcs_write(pinfo->link, MIPI_DSI_COMPRESSION_MODE, (u8[]){ 0x11 }, 0);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to set compression mode: %d\n", err);
		goto poweroff;
	}


	/* Send rest of the init cmds */
	err = send_mipi_cmds(panel, pinfo->desc->on_cmds_2);

	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to send DCS Init 2nd Code: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_set_display_on(pinfo->link);
	if (err < 0) {
		DRM_DEV_ERROR(panel->dev,
				"failed to Set Display ON: %d\n", err);
		goto poweroff;
	}

	/* 0x32 = 50ms delay */
	msleep(120);

	pinfo->prepared = true;

	return 0;

poweroff:
	gpiod_set_value(pinfo->reset_gpio, 1);
	return err;
}

static int lg_panel_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	if (pinfo->enabled)
		return 0;
	ret = backlight_enable(pinfo->backlight);
	if (ret) {
		DRM_DEV_ERROR(panel->dev,
				"Failed to enable backlight %d\n", ret);
		return ret;
	}

	if (panel->dsc) {
		/* this panel uses DSC so send the pps */
		drm_dsc_pps_payload_pack(&pps, panel->dsc);
		print_hex_dump(KERN_DEBUG, "DSC params:", DUMP_PREFIX_NONE,
                               16, 1, &pps, sizeof(pps), false);

		//ret = mipi_dsi_picture_parameter_set(pinfo->link, &pps);
		//if (ret < 0) {
		//	DRM_DEV_ERROR(panel->dev,
		//		      "failed to set pps: %d\n", ret);
		//	return ret;
		//}
	}

	pinfo->enabled = true;

	return 0;
}

static int lg_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;
	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		DRM_DEV_ERROR(panel->dev, "failed to add mode %ux%u\n",
				m->hdisplay, m->vdisplay);
		return -ENOMEM;
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static int lg_panel_backlight_update_status(struct backlight_device *bl)
{
	struct panel_info *pinfo = bl_get_data(bl);
	int ret = 0;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & BL_CORE_FBBLANK) {
		pinfo->brightness = 0;
	}
	else
		pinfo->brightness = bl->props.brightness;

	ret = mipi_dsi_dcs_set_display_brightness(pinfo->link,
					pinfo->brightness);
	return ret;
}

static int lg_panel_backlight_get_brightness(struct backlight_device *bl)
{
	struct panel_info *pinfo = bl_get_data(bl);
	int ret = 0;
	u16 brightness = 0;

	ret = mipi_dsi_dcs_get_display_brightness(pinfo->link, &brightness);
	if (ret < 0)
		return ret;

	return brightness & 0xff;
}

const struct backlight_ops lg_panel_backlight_ops = {
	.update_status = lg_panel_backlight_update_status,
	.get_brightness = lg_panel_backlight_get_brightness,
};

static int lg_panel_backlight_init(struct panel_info *pinfo)
{
	struct backlight_properties props = {};
	struct backlight_device	*bl;
	struct device *dev = &pinfo->link->dev;

	props.type = BACKLIGHT_RAW;

	/* Set the max_brightness to 255 to begin with */
	props.max_brightness = pinfo->max_brightness = 255;
	props.brightness = pinfo->max_brightness;
	pinfo->brightness = pinfo->max_brightness;
	bl = devm_backlight_device_register(dev, "lg-sw43408", dev, pinfo,
					     &lg_panel_backlight_ops, &props);
	if (IS_ERR(bl)) {
		DRM_ERROR("failed to register backlight device\n");
		return PTR_ERR(bl);
	}
	pinfo->backlight = bl;

	return 0;
}


static const struct drm_panel_funcs panel_funcs = {
	.disable = lg_panel_disable,
	.unprepare = lg_panel_unprepare,
	.prepare = lg_panel_prepare,
	.enable = lg_panel_enable,
	.get_modes = lg_panel_get_modes,
};

static const struct panel_cmd lg_sw43408_on_cmds_1[] = {
	_INIT_CMD(0x00, 0x26, 0x02),	// MIPI_DCS_SET_GAMMA_CURVE, 0x02
	_INIT_CMD(0x00, 0x35, 0x00),	// MIPI_DCS_SET_TEAR_ON
	_INIT_CMD(0x00, 0x53, 0x0C, 0x30),
	_INIT_CMD(0x00, 0x55, 0x00, 0x70, 0xDF, 0x00, 0x70, 0xDF),
	_INIT_CMD(0x00, 0xF7, 0x01, 0x49, 0x0C),

	{},
};

static const struct panel_cmd lg_sw43408_on_cmds_2[] = {
	_INIT_CMD(0x00, 0xB0, 0xAC),
	_INIT_CMD(0x00, 0xCD,
			0x00, 0x00, 0x00, 0x19, 0x19, 0x19, 0x19, 0x19,
			0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
			0x16, 0x16),
	_INIT_CMD(0x00, 0xCB, 0x80, 0x5C, 0x07, 0x03, 0x28),
	_INIT_CMD(0x00, 0xC0, 0x02, 0x02, 0x0F),
	_INIT_CMD(0x00, 0xE5, 0x00, 0x3A, 0x00, 0x3A, 0x00, 0x0E, 0x10),
	_INIT_CMD(0x00, 0xB5,
			0x75, 0x60, 0x2D, 0x5D, 0x80, 0x00, 0x0A, 0x0B,
			0x00, 0x05, 0x0B, 0x00, 0x80, 0x0D, 0x0E, 0x40,
			0x00, 0x0C, 0x00, 0x16, 0x00, 0xB8, 0x00, 0x80,
			0x0D, 0x0E, 0x40, 0x00, 0x0C, 0x00, 0x16, 0x00,
			0xB8, 0x00, 0x81, 0x00, 0x03, 0x03, 0x03, 0x01,
			0x01),
	_INIT_CMD(0x00, 0x55, 0x04, 0x61, 0xDB, 0x04, 0x70, 0xDB),
	_INIT_CMD(0x00, 0xB0, 0xCA),

	{},
};

static const struct drm_display_mode lg_panel_default_mode = {
	.clock		= 152340,

	.hdisplay	= 1080,
	.hsync_start	= 1080 + 20,
	.hsync_end	= 1080 + 20 + 32,
	.htotal		= 1080 + 20 + 32 + 20,

	.vdisplay	= 2160,
	.vsync_start	= 2160 + 20,
	.vsync_end	= 2160 + 20 + 4,
	.vtotal		= 2160 + 20 + 4 + 20,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct panel_desc lg_panel_desc = {
	.display_mode = &lg_panel_default_mode,

	.width_mm = 62,
	.height_mm = 124,

	.mode_flags = MIPI_DSI_MODE_LPM,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds_1 = lg_sw43408_on_cmds_1,
	.on_cmds_2 = lg_sw43408_on_cmds_2,
};


static const struct of_device_id panel_of_match[] = {
	{ .compatible = "lg,sw43408",
	  .data = &lg_panel_desc
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static int panel_pinctrl_init(struct panel_info *panel)
{
	struct device *dev = &panel->link->dev;
	int rc = 0;

	panel->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(panel->pinctrl)) {
		rc = PTR_ERR(panel->pinctrl);
		pr_err("failed to get pinctrl, rc=%d\n", rc);
		goto error;
	}

	panel->active = pinctrl_lookup_state(panel->pinctrl,
							"panel_active");
	if (IS_ERR_OR_NULL(panel->active)) {
		rc = PTR_ERR(panel->active);
		pr_err("failed to get pinctrl active state, rc=%d\n", rc);
		goto error;
	}

	panel->suspend =
		pinctrl_lookup_state(panel->pinctrl, "panel_suspend");

	if (IS_ERR_OR_NULL(panel->suspend)) {
		rc = PTR_ERR(panel->suspend);
		pr_err("failed to get pinctrl suspend state, rc=%d\n", rc);
		goto error;
	}

error:
	return rc;
}

static int panel_add(struct panel_info *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int i, ret;

	pinfo->init_delay_us = 5000;

	for (i = 0; i < ARRAY_SIZE(pinfo->supplies); i++)
		pinfo->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return ret;

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get reset gpio %ld\n",
			PTR_ERR(pinfo->reset_gpio));
		return PTR_ERR(pinfo->reset_gpio);
	}

	ret = panel_pinctrl_init(pinfo);
	if (ret < 0)
		return ret;

	ret = lg_panel_backlight_init(pinfo);
	if (ret < 0)
		return ret;

	drm_panel_init(&pinfo->base, dev, &panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&pinfo->base);
	return ret;
}

static void panel_del(struct panel_info *pinfo)
{
	if (pinfo->base.dev)
		drm_panel_remove(&pinfo->base);
}

static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo;
	const struct panel_desc *desc;
	struct drm_dsc_config *dsc;
	int err;

	pinfo = devm_kzalloc(&dsi->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;

	pinfo->link = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);

	err = panel_add(pinfo);
	if (err < 0)
		return err;

	/* The panel is DSC panel only, set the dsc params */
	dsc = devm_kzalloc(&dsi->dev, sizeof(*dsc), GFP_KERNEL);
	if (!dsc)
		return -ENOMEM;

	dsc->dsc_version_major = 0x1;
	dsc->dsc_version_minor = 0x1;

	dsc->slice_height = 16;
	dsc->slice_width = 540;
	dsc->slice_count = 1;
	dsc->bits_per_component = 8;
	dsc->bits_per_pixel = 8;
	dsc->block_pred_enable = true;

	pinfo->base.dsc = dsc;

	return mipi_dsi_attach(dsi);
}

static int panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = lg_panel_unprepare(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to unprepare panel: %d\n",
				err);

	err = lg_panel_disable(&pinfo->base);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to disable panel: %d\n", err);

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		DRM_DEV_ERROR(&dsi->dev, "failed to detach from DSI host: %d\n",
				err);

	panel_del(pinfo);

	return 0;
}

static void panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	lg_panel_disable(&pinfo->base);
	lg_panel_unprepare(&pinfo->base);
}

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-lg-sw43408",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
	.shutdown = panel_shutdown,
};
module_mipi_dsi_driver(panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_DESCRIPTION("LG SW436408 MIPI-DSI LED panel");
MODULE_LICENSE("GPL");
