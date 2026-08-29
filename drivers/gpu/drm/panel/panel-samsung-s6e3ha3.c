// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S6E3HA3 1440x2560 command-mode AMOLED panel driver.
 *
 * Used on the Galaxy S7 (herolte) behind Exynos8890's DECON-F/DSIM0. The
 * link runs 4 lanes at 897Mbps with MIC 1/2 compression, so the panel is
 * told to expect a compressed 4-lane stream (0xc4/0xf9) rather than raw
 * RGB888. Command bytes follow vendor's s6e3ha3_wqhd_init().
 *
 * Brightness is left at the level the DDI powers up with: the vendor AID
 * dimming tables are not ported.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct s6e3ha3 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
};

static const struct drm_display_mode s6e3ha3_mode = {
	.clock = 223754,
	.hdisplay = 1440,
	.hsync_start = 1440 + 2,
	.hsync_end = 1440 + 2 + 2,
	.htotal = 1440 + 2 + 2 + 2,
	.vdisplay = 2560,
	.vsync_start = 2560 + 3,
	.vsync_end = 2560 + 3 + 1,
	.vtotal = 2560 + 3 + 1 + 15,
	.width_mm = 63,
	.height_mm = 113,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static inline struct s6e3ha3 *to_s6e3ha3(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3ha3, panel);
}

static int s6e3ha3_prepare(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;
}

static int s6e3ha3_unprepare(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int s6e3ha3_enable(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* level 2 and level 3 command access keys */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	mipi_dsi_msleep(&dsi_ctx, 5);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 5);

	/* 4 lanes, MIC 1/2 compressed input */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc4, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf9, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0xbc, 0x4a);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x01, 0x09, 0xff, 0x00, 0x0a);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x4c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2,
				     0x00, 0x00, 0xd8, 0xd8, 0x00, 0x80, 0x2b, 0x05,
				     0x08, 0x0e, 0x07, 0x0b, 0x05, 0x0d, 0x0a, 0x15,
				     0x13, 0x20, 0x1e);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int s6e3ha3_disable(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int s6e3ha3_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &s6e3ha3_mode);
}

static const struct drm_panel_funcs s6e3ha3_funcs = {
	.prepare	= s6e3ha3_prepare,
	.unprepare	= s6e3ha3_unprepare,
	.enable		= s6e3ha3_enable,
	.disable	= s6e3ha3_disable,
	.get_modes	= s6e3ha3_get_modes,
};

static int s6e3ha3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct s6e3ha3 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct s6e3ha3, panel, &s6e3ha3_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	ctx->supplies[0].supply = "vdd3";
	ctx->supplies[1].supply = "vci";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable-gpios\n");

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;
	dsi->hs_rate = 897000000;

	ctx->panel.prepare_prev_first = true;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	return 0;
}

static void s6e3ha3_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3ha3 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id s6e3ha3_of_match[] = {
	{ .compatible = "samsung,s6e3ha3" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3ha3_of_match);

static struct mipi_dsi_driver s6e3ha3_driver = {
	.probe = s6e3ha3_probe,
	.remove = s6e3ha3_remove,
	.driver = {
		.name = "panel-samsung-s6e3ha3",
		.of_match_table = s6e3ha3_of_match,
	},
};
module_mipi_dsi_driver(s6e3ha3_driver);

MODULE_DESCRIPTION("Samsung S6E3HA3 WQHD command-mode panel driver");
MODULE_LICENSE("GPL");
