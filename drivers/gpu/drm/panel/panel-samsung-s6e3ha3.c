// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S6E3HA3 1440x2560 command-mode AMOLED panel driver.
 *
 * Used on the Galaxy S7 (herolte) behind Exynos8890's DECON-F/DSIM0. The
 * link runs 4 lanes at 897Mbps with MIC 1/2 compression, so the panel is
 * told to expect a compressed 4-lane stream (0xc4/0xf9) rather than raw
 * RGB888. Command bytes follow vendor's s6e3ha3_wqhd_init().
 *
 * Normal-range AID drive and AUTO_UI follow Samsung's S6E3HA3_DYNAMIC
 * Daisy path (smart_on=0). Calibration is read from each panel, never
 * programmed. HBM/SMART/HMT and non-Daisy panel variants are not exposed.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "panel-samsung-s6e3ha3-dimming.h"
#include "panel-samsung-s6e3ha3-mdnie.h"

struct s6e3ha3 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	/* Serializes backlight traffic with every power/lifecycle transition. */
	struct mutex lock;
	/* Bulk-disable failure retains regulator refs, but reset is asserted. */
	bool supplies_enabled;
	bool powered;
	bool enabled;
	bool displaying;
	struct s6e3ha3_dimming dimming;
	u8 gamma[ARRAY_SIZE(s6e3ha3_levels)][OLED_CMD_GAMMA_CNT];
	u8 elvss[30];
	u8 mdnie[sizeof(AUTO_UI_1)];
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
	int ret = 0;

	mutex_lock(&ctx->lock);
	if (ctx->powered)
		goto out;
	if (!ctx->supplies_enabled) {
		ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		if (ret)
			goto out;
		ctx->supplies_enabled = true;
	}
	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	ctx->powered = true;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int s6e3ha3_unprepare(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	int ret = 0;

	mutex_lock(&ctx->lock);
	ctx->enabled = false;
	ctx->displaying = false;
	if (ctx->powered) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);
		ctx->powered = false;
	}
	if (ctx->supplies_enabled) {
		ret = regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		if (!ret)
			ctx->supplies_enabled = false;
	}
	mutex_unlock(&ctx->lock);
	if (ret)
		dev_err(panel->dev, "failed to disable supplies: %d\n", ret);
	/* Reset completes logical unprepare; retain failed rail refs for reuse. */
	return 0;
}

/* Exact lengths matter: never calculate drive data from a partial reply. */
static int s6e3ha3_read(struct s6e3ha3 *ctx, u8 command, void *data, size_t len)
{
	int ret;

	/* DCS read does not negotiate the return size; vendor does this per read. */
	ret = mipi_dsi_set_maximum_return_packet_size(ctx->dsi, len);
	if (ret < 0)
		return ret;
	ret = mipi_dsi_dcs_read(ctx->dsi, command, data, len);
	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int s6e3ha3_read_calibration(struct s6e3ha3 *ctx)
{
	u8 id[3], mtp[47], coordinate[4];
	s64 x, y;
	int ret, i, f1, f2, f3, f4, tune;

	ret = s6e3ha3_read(ctx, 0x04, id, sizeof(id));
	if (ret)
		return ret;
	/* The fitted HA3 Daisy ID starts with zero; that is NOT disconnection. */
	if (id[0] != 0x00 || id[1] != 0x40 || id[2] != 0x44)
		return -ENODEV;
	ret = s6e3ha3_read(ctx, 0xc8, mtp, sizeof(mtp));
	if (ret)
		return ret;
	ctx->elvss[0] = 0xb5;
	ret = s6e3ha3_read(ctx, 0xb5, ctx->elvss + 1, sizeof(ctx->elvss) - 1);
	if (ret)
		return ret;
	ret = s6e3ha3_read(ctx, 0xa1, coordinate, sizeof(coordinate));
	if (ret)
		return ret;

	s6e3ha3_decode_mtp(&ctx->dimming, mtp);
	ret = s6e3ha3_generate_voltages(&ctx->dimming);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(ctx->gamma); i++) {
		ret = s6e3ha3_calculate_gamma(&ctx->dimming, i, ctx->gamma[i]);
		if (ret)
			return ret;
	}

	/* hero1 AUTO_UI: same coordinate regions as mdnie_lite.c. */
	memcpy(ctx->mdnie, AUTO_UI_1, sizeof(ctx->mdnie));
	/* Vendor resume disables ASCR transition dimming for the first image. */
	ctx->mdnie[2] = 0;
	x = (coordinate[0] << 8) | coordinate[1];
	y = (coordinate[2] << 8) | coordinate[3];
	if (x || y) {
		f1 = ((y << 10) - div_s64((x << 10) * 43, 40) + (45 << 10)) >> 10;
		f2 = ((y << 10) - div_s64((x << 10) * 310, 297) - (3 << 10)) >> 10;
		f3 = ((y << 10) + div_s64((x << 10) * 367, 84) - (16305 << 10)) >> 10;
		f4 = ((y << 10) + div_s64((x << 10) * 333, 107) - (12396 << 10)) >> 10;
		if (f1 > 0)
			tune = f3 > 0 ? 3 : (f4 < 0 ? 1 : 2);
		else if (f2 < 0)
			tune = f3 > 0 ? 9 : (f4 < 0 ? 7 : 8);
		else
			tune = f3 > 0 ? 6 : (f4 < 0 ? 4 : 5);
		ctx->mdnie[52] = coordinate_data_1[tune * 3];
		ctx->mdnie[54] = coordinate_data_1[tune * 3 + 1];
		ctx->mdnie[56] = coordinate_data_1[tune * 3 + 2];
	}
	return 0;
}

/* Always attempt both key locks, even after an earlier transfer failed. */
static int s6e3ha3_lock_keys(struct s6e3ha3 *ctx)
{
	static const u8 fc[] = { 0xfc, 0xa5, 0xa5 };
	static const u8 f0[] = { 0xf0, 0xa5, 0xa5 };
	int ret, next;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, fc, sizeof(fc));
	next = mipi_dsi_dcs_write_buffer(ctx->dsi, f0, sizeof(f0));
	return ret < 0 ? ret : (next < 0 ? next : 0);
}

/* Independent writes: a display-off failure must not suppress sleep-in. */
static int s6e3ha3_sleep(struct s6e3ha3 *ctx)
{
	static const u8 off[] = { 0x28 };
	static const u8 sleep[] = { 0x10 };
	int ret, next;

	ret = mipi_dsi_dcs_write_buffer(ctx->dsi, off, sizeof(off));
	msleep(20);
	next = mipi_dsi_dcs_write_buffer(ctx->dsi, sleep, sizeof(sleep));
	msleep(120);
	ctx->displaying = false;
	return ret < 0 ? ret : (next < 0 ? next : 0);
}

/* Caller holds lock, has read calibration, and has unlocked F0/FC. */
static int s6e3ha3_drive(struct s6e3ha3 *ctx,
		       struct mipi_dsi_multi_context *dsi_ctx, int brightness)
{
	u8 aid[sizeof(S6E3HA3_SEQ_AOR_CONTROL)];
	u8 elvss[sizeof(ctx->elvss)];
	u8 vint[sizeof(S6E3HA3_SEQ_VINT_SET)];
	const struct s6e3ha3_level *info;
	int level, i;

	level = s6e3ha3_brightness_index(brightness);
	if (level < 0)
		return level;
	info = &s6e3ha3_levels[level];
	memcpy(aid, S6E3HA3_SEQ_AOR_CONTROL, sizeof(aid));
	aid[9] = inter_aor_tbl_ha3_da[brightness * 2];
	aid[10] = inter_aor_tbl_ha3_da[brightness * 2 + 1];
	memcpy(elvss, ctx->elvss, sizeof(elvss));
	memcpy(elvss + 1, info->br >= 41 ? info->elvCaps : info->elv, 2);
	/* Daisy normal-range HA3_elvss_offset7 is zero at every temperature.
	 * No thermal source is wired: use vendor NORMAL_TEMPERATURE (25 C),
	 * not a claim of measured panel temperature. HBM is not exposed.
	 */
	elvss[29] = 25;
	memcpy(vint, S6E3HA3_SEQ_VINT_SET, sizeof(vint));
	for (i = 0; i < ARRAY_SIZE(VINT_DIM_TABLE_HA3) - 1; i++)
		if (info->br <= VINT_DIM_TABLE_HA3[i])
			break;
	vint[2] = VINT_TABLE_HA3[i];

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, ctx->gamma[level], sizeof(ctx->gamma[level]));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, aid, sizeof(aid));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, elvss, sizeof(elvss));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, vint, sizeof(vint));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, irc_table_HA3_da[brightness],
				      sizeof(irc_table_HA3_da[brightness]));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, SEQ_GAMMA_UPDATE, sizeof(SEQ_GAMMA_UPDATE));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, SEQ_GAMMA_UPDATE_L, sizeof(SEQ_GAMMA_UPDATE_L));
	/* Vendor default adaptive_control=5: ACL 15% for the whole UI range. */
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, S6E3HA3_SEQ_ACL_ON_OPR_15,
				      sizeof(S6E3HA3_SEQ_ACL_ON_OPR_15));
	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, SEQ_ACL_ON, sizeof(SEQ_ACL_ON));
	return dsi_ctx->accum_err;
}

static int s6e3ha3_enable(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret = 0, brightness;

	mutex_lock(&ctx->lock);
	if (ctx->enabled)
		goto out;
	if (!ctx->powered) {
		ret = -EPERM;
		goto out;
	}
	brightness = panel->backlight->props.brightness;
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	mipi_dsi_msleep(&dsi_ctx, 5);
	ret = dsi_ctx.accum_err;
	if (ret)
		goto fail;
	/* Stock reads factory data before entering the sleep-out transition. */
	ret = s6e3ha3_read_calibration(ctx);
	if (ret)
		goto fail;
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 5);

	/* Existing WQHD DSU, 4-lane MIC 1/2 and command-only TE are intentional. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc4, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf9, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0xbc, 0x4a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba, 0x01);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x01, 0x09, 0xff, 0x00, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_TEAR_ON);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x4c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2,
				     0x00, 0x00, 0xd8, 0xd8, 0x00, 0x80, 0x2b, 0x05,
				     0x08, 0x0e, 0x07, 0x0b, 0x05, 0x0d, 0x0a, 0x15,
				     0x13, 0x20, 0x1e);
	ret = s6e3ha3_drive(ctx, &dsi_ctx, brightness);
	if (ret)
		goto fail;
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	/* Vendor hero1 MDNIE_SET(AUTO_UI): F0 unlock, DF/DE/DD, F0 lock.
	 * Program before display-on so the first frame has a complete baseline.
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, ctx->mdnie, sizeof(ctx->mdnie));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, AUTO_UI_2, sizeof(AUTO_UI_2));
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, AUTO_UI_3, sizeof(AUTO_UI_3));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	if (brightness)
		mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	ret = dsi_ctx.accum_err;
	if (ret)
		goto fail;
	ctx->displaying = brightness != 0;
	ctx->enabled = true;
	goto out;
fail:
	/* A failed multi-context suppresses subsequent writes: bypass it here. */
	s6e3ha3_lock_keys(ctx);
	s6e3ha3_sleep(ctx);
	ctx->enabled = false;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int s6e3ha3_backlight_update(struct backlight_device *backlight)
{
	struct s6e3ha3 *ctx = bl_get_data(backlight);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret = 0, next, brightness;

	mutex_lock(&ctx->lock);
	if (!ctx->powered || !ctx->enabled)
		goto out;
	brightness = backlight_get_brightness(backlight);
	if (!brightness) {
		mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
		ret = dsi_ctx.accum_err;
		if (!ret)
			ctx->displaying = false;
		goto out;
	}
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x5a, 0x5a);
	ret = s6e3ha3_drive(ctx, &dsi_ctx, brightness);
	next = s6e3ha3_lock_keys(ctx);
	if (!ret)
		ret = next;
	if (!ret && !ctx->displaying) {
		mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
		ret = dsi_ctx.accum_err;
		if (!ret)
			ctx->displaying = true;
	}
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct backlight_ops s6e3ha3_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = s6e3ha3_backlight_update,
};

static int s6e3ha3_disable(struct drm_panel *panel)
{
	struct s6e3ha3 *ctx = to_s6e3ha3(panel);
	int ret = 0;

	mutex_lock(&ctx->lock);
	if (ctx->powered && ctx->enabled)
		ret = s6e3ha3_sleep(ctx);
	ctx->enabled = false;
	mutex_unlock(&ctx->lock);
	if (ret)
		dev_err(panel->dev, "failed to enter sleep: %d\n", ret);
	/* Shutdown was best effort; let DRM complete the logical disable. */
	return 0;
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
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = 162,
	};
	struct s6e3ha3 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct s6e3ha3, panel, &s6e3ha3_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	mutex_init(&ctx->lock);
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
	ctx->panel.backlight = devm_backlight_device_register(dev, dev_name(dev),
							    dev, ctx,
							    &s6e3ha3_backlight_ops,
							    &props);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to register backlight\n");

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

	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
	/* DRM may already be unprepared with a failed rail release retained. */
	s6e3ha3_unprepare(&ctx->panel);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static void s6e3ha3_shutdown(struct mipi_dsi_device *dsi)
{
	struct s6e3ha3 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
}

static const struct of_device_id s6e3ha3_of_match[] = {
	{ .compatible = "samsung,s6e3ha3" },
	{ }
};
MODULE_DEVICE_TABLE(of, s6e3ha3_of_match);

static struct mipi_dsi_driver s6e3ha3_driver = {
	.probe = s6e3ha3_probe,
	.remove = s6e3ha3_remove,
	.shutdown = s6e3ha3_shutdown,
	.driver = {
		.name = "panel-samsung-s6e3ha3",
		.of_match_table = s6e3ha3_of_match,
	},
};
module_mipi_dsi_driver(s6e3ha3_driver);

MODULE_DESCRIPTION("Samsung S6E3HA3 WQHD command-mode panel driver");
MODULE_LICENSE("GPL");
