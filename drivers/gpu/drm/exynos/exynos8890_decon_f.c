// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 DECON-F CRTC driver.
 *
 * DECON-F is the internal-panel display controller on the DISP0 domain. It
 * owns eight blending windows but no DMA of its own - every window sources
 * from a separate VPP IP (see exynos8890_vpp.c), selected per window through
 * WIN_CONTROL.CHMAP. Only the four DISP0 VPPs exist on herolte, so four
 * planes are exposed.
 *
 * The LCD-side configuration (MIC 1/2 compression, splitter, DISPIF timing
 * and porches) is inherited from the bootloader rather than reprogrammed:
 * vendor's own decon_reg_init() takes the same path when it finds DECON
 * already running, and the MIC block inside the DECON register window has no
 * public programming model. The driver therefore refuses to attach if the
 * handoff signature is not present.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>

#include <drm/exynos_drm.h>

#include "exynos8890_vpp.h"
#include "exynos_drm_crtc.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_plane.h"
#include "regs-decon8890.h"

#define WINDOWS_NR		VPP8890_CH_NR
#define DECON_F_CLK_NR		6
#define DECON_F_CLK_PCLK	1
#define DECON_F_IRQ_NR		4

#define DECON_F_UPDATE_TIMEOUT_US	100000

/* window index -> IDMA channel; window 0 keeps the channel the bootloader used */
static const unsigned int decon_f_win_ch[WINDOWS_NR] = {
	VPP8890_CH_G1,
	VPP8890_CH_G0,
	VPP8890_CH_VG0,
	VPP8890_CH_VG1,
};

static const uint32_t decon_f_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_RGB565,
};

static const char * const decon_f_clk_names[DECON_F_CLK_NR] = {
	"disp_pll", "decon_pclk", "eclk_user", "eclk_leaf",
	"vclk_user", "vclk_leaf",
};

struct decon_f_context {
	struct device *dev;
	struct drm_device *drm_dev;
	struct exynos_drm_crtc *crtc;
	struct exynos_drm_plane planes[WINDOWS_NR];
	struct exynos_drm_plane_config configs[WINDOWS_NR];
	struct device *vpp_dev;
	void __iomem *regs;
	struct clk *clks[DECON_F_CLK_NR];
	spinlock_t vblank_lock;
	bool suspended;
};

static inline u32 decon_read(struct decon_f_context *ctx, u32 reg)
{
	return readl(ctx->regs + reg);
}

static inline void decon_write(struct decon_f_context *ctx, u32 reg, u32 val)
{
	writel(val, ctx->regs + reg);
}

static inline void decon_update(struct decon_f_context *ctx, u32 reg, u32 val, u32 mask)
{
	decon_write(ctx, reg, (decon_read(ctx, reg) & ~mask) | (val & mask));
}

/*
 * The bootloader leaves DECON scanning out of cont_splash_mem. Everything
 * below the window layer - MIC, splitter, DISPIF timing - is inherited, so
 * refuse to drive hardware whose configuration we did not expect.
 */
static bool decon_f_handoff_valid(struct decon_f_context *ctx)
{
	static const u32 status = DECON8890_GLOBAL_IDLE | DECON8890_GLOBAL_RUN |
				  DECON8890_GLOBAL_URGENT;
	u32 global = decon_read(ctx, DECON8890_GLOBAL_CONTROL);

	if (!(global & DECON8890_GLOBAL_RUN))
		return false;
	if ((global & ~status) != (DECON8890_INHERITED_GLOBAL & ~status))
		return false;
	if (decon_read(ctx, DECON8890_TRIGGER_CONTROL) != DECON8890_INHERITED_TRIGGER)
		return false;
	if (decon_read(ctx, DECON8890_DATA_PATH_CONTROL) != DECON8890_INHERITED_PATH)
		return false;
	if (decon_read(ctx, DECON8890_DISPIF_SIZE) != DECON8890_INHERITED_SIZE)
		return false;

	return true;
}

static u32 decon_f_vblank_source(struct exynos_drm_crtc *crtc)
{
	return crtc->i80_mode ? DECON8890_INT_FRAME_DONE : DECON8890_INT_VSTATUS;
}

static void decon_f_set_trigger(struct decon_f_context *ctx, bool enable)
{
	/*
	 * HW trigger polarity is inverted: arming it clears the mask. TE
	 * drives the trigger in hardware, so nothing else is needed here.
	 */
	decon_update(ctx, DECON8890_TRIGGER_CONTROL,
		     enable ? 0 : ~0, DECON8890_TRIGGER_HW_MASK);
}

static void decon_f_wait_update(struct decon_f_context *ctx)
{
	u32 val;

	if (readl_poll_timeout_atomic(ctx->regs + DECON8890_SHADOW_UPDATE, val,
				      !val, 10, DECON_F_UPDATE_TIMEOUT_US))
		dev_warn(ctx->dev, "shadow update did not drain\n");
}

static irqreturn_t decon_f_irq_handler(int irq, void *dev_data)
{
	struct decon_f_context *ctx = dev_data;
	u32 pending;

	spin_lock(&ctx->vblank_lock);

	/* ack even while suspended: these are level-triggered */
	pending = decon_read(ctx, DECON8890_INTERRUPT_PENDING);
	if (!pending)
		goto out;
	decon_write(ctx, DECON8890_INTERRUPT_PENDING, pending);

	if (ctx->suspended)
		goto out;

	if (pending & DECON8890_INT_FIFO_LEVEL)
		dev_warn_ratelimited(ctx->dev, "DISPIF underrun\n");
	if (pending & DECON8890_INT_RESOURCE_CONFLICT)
		dev_warn_ratelimited(ctx->dev, "window resource conflict\n");

	if (pending & decon_f_vblank_source(ctx->crtc))
		drm_crtc_handle_vblank(&ctx->crtc->base);
out:
	spin_unlock(&ctx->vblank_lock);
	return IRQ_HANDLED;
}

static enum drm_mode_status decon_f_mode_valid(struct exynos_drm_crtc *crtc,
					       const struct drm_display_mode *mode)
{
	struct decon_f_context *ctx = crtc->ctx;
	u32 dispif = decon_read(ctx, DECON8890_DISPIF_SIZE);
	u16 height = dispif >> 16;

	/*
	 * MIC halves the width before the display interface, so DISPIF only
	 * pins down the height. Anything else would need the inherited MIC
	 * configuration rewritten.
	 */
	if (mode->vdisplay != height)
		return MODE_BAD_VVALUE;
	if (mode->hdisplay != (dispif & 0x3fff) * 2)
		return MODE_BAD_HVALUE;

	return MODE_OK;
}

static void decon_f_atomic_enable(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;
	unsigned long flags;
	int i, ret;

	if (!ctx->suspended)
		return;

	ret = pm_runtime_resume_and_get(ctx->dev);
	if (ret) {
		dev_err(ctx->dev, "failed to resume: %d\n", ret);
		return;
	}

	for (i = 0; i < DECON_F_CLK_NR; i++) {
		ret = clk_prepare_enable(ctx->clks[i]);
		if (ret) {
			dev_err(ctx->dev, "failed to enable %s: %d\n",
				decon_f_clk_names[i], ret);
			goto err_clk;
		}
	}

	ret = exynos8890_vpp_resume(ctx->vpp_dev);
	if (ret) {
		dev_err(ctx->dev, "failed to enable VPPs: %d\n", ret);
		goto err_clk;
	}

	decon_f_set_trigger(ctx, false);
	decon_write(ctx, DECON8890_INTERRUPT_PENDING,
		    decon_read(ctx, DECON8890_INTERRUPT_PENDING));
	decon_write(ctx, DECON8890_INTERRUPT_ENABLE,
		    DECON8890_INT_EN | DECON8890_INT_FIFO_LEVEL |
		    DECON8890_INT_RESOURCE_CONFLICT |
		    DECON8890_INT_VSTATUS_SEL_VSA);

	spin_lock_irqsave(&ctx->vblank_lock, flags);
	ctx->suspended = false;
	spin_unlock_irqrestore(&ctx->vblank_lock, flags);

	drm_crtc_vblank_on(&crtc->base);
	return;

err_clk:
	while (--i >= 0)
		clk_disable_unprepare(ctx->clks[i]);
	pm_runtime_put(ctx->dev);
}

static void decon_f_atomic_disable(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;
	unsigned long flags;
	int i;

	if (ctx->suspended)
		return;

	drm_crtc_vblank_off(&crtc->base);

	for (i = 0; i < WINDOWS_NR; i++)
		decon_update(ctx, DECON8890_WIN_CONTROL(i), 0, DECON8890_WIN_ENABLE);
	decon_write(ctx, DECON8890_SHADOW_UPDATE,
		    DECON8890_UPDATE_GLOBAL | DECON8890_UPDATE_ALL_WIN);
	decon_f_wait_update(ctx);
	decon_f_set_trigger(ctx, false);

	spin_lock_irqsave(&ctx->vblank_lock, flags);
	ctx->suspended = true;
	spin_unlock_irqrestore(&ctx->vblank_lock, flags);

	decon_write(ctx, DECON8890_INTERRUPT_ENABLE, 0);

	exynos8890_vpp_suspend(ctx->vpp_dev);

	for (i = DECON_F_CLK_NR - 1; i >= 0; i--)
		clk_disable_unprepare(ctx->clks[i]);

	pm_runtime_put(ctx->dev);
}

static int decon_f_enable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;

	decon_update(ctx, DECON8890_INTERRUPT_ENABLE, ~0,
		     decon_f_vblank_source(crtc));
	return 0;
}

static void decon_f_disable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;

	decon_update(ctx, DECON8890_INTERRUPT_ENABLE, 0,
		     decon_f_vblank_source(crtc));
}

static void decon_f_atomic_begin(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;

	/* let the previous frame land, then freeze the trigger while we write */
	decon_f_wait_update(ctx);
	decon_f_set_trigger(ctx, false);
}

static void decon_f_update_plane(struct exynos_drm_crtc *crtc,
				 struct exynos_drm_plane *plane)
{
	struct exynos_drm_plane_state *state =
			to_exynos_plane_state(plane->base.state);
	struct decon_f_context *ctx = crtc->ctx;
	struct drm_framebuffer *fb = state->base.fb;
	unsigned int win = plane->index;
	unsigned int ch = decon_f_win_ch[win];
	struct exynos8890_vpp_cfg cfg = {
		.dma_addr	= exynos_drm_fb_dma_addr(fb, 0),
		.fourcc		= fb->format->format,
		.src_x		= state->src.x,
		.src_y		= state->src.y,
		.src_w		= state->src.w,
		.src_h		= state->src.h,
		.fb_w		= fb->pitches[0] / fb->format->cpp[0],
		.fb_h		= fb->height,
	};
	u32 alpha = plane->base.state->alpha >> 8;
	u32 val;

	if (exynos8890_vpp_setup(ctx->vpp_dev, ch, &cfg)) {
		dev_err_ratelimited(ctx->dev, "vpp%u setup failed for window %u\n",
				    ch, win);
		return;
	}

	decon_write(ctx, DECON8890_WIN_START(win),
		    DECON8890_WIN_POS_X(state->crtc.x) |
		    DECON8890_WIN_POS_Y(state->crtc.y));
	decon_write(ctx, DECON8890_WIN_END(win),
		    DECON8890_WIN_POS_X(state->crtc.x + state->crtc.w - 1) |
		    DECON8890_WIN_POS_Y(state->crtc.y + state->crtc.h - 1));
	decon_write(ctx, DECON8890_WIN_PIXEL_COUNT(win),
		    state->crtc.w * state->crtc.h);
	decon_write(ctx, DECON8890_WIN_START_TIME(win), 0);

	val = DECON8890_WIN_ALPHA0(alpha) | DECON8890_WIN_ALPHA1(alpha) |
	      DECON8890_WIN_CHMAP(exynos8890_vpp_chmap(ch)) |
	      DECON8890_WIN_ENABLE;

	/* the bottom window has nothing to blend against */
	if (win == 0 || !fb->format->has_alpha)
		val |= DECON8890_WIN_FUNC(DECON8890_PD_FUNC_COPY) |
		       DECON8890_WIN_ALPHA_SEL(DECON8890_ALPHA_SEL_ALPHA0);
	else
		val |= DECON8890_WIN_FUNC(DECON8890_PD_FUNC_SOURCE_OVER) |
		       DECON8890_WIN_ALPHA_SEL(DECON8890_ALPHA_SEL_BYAEN);

	decon_write(ctx, DECON8890_WIN_CONTROL(win), val);
}

static void decon_f_disable_plane(struct exynos_drm_crtc *crtc,
				  struct exynos_drm_plane *plane)
{
	struct decon_f_context *ctx = crtc->ctx;
	unsigned int win = plane->index;

	decon_update(ctx, DECON8890_WIN_CONTROL(win), 0, DECON8890_WIN_ENABLE);
	exynos8890_vpp_disable(ctx->vpp_dev, decon_f_win_ch[win]);
}

static void decon_f_atomic_flush(struct exynos_drm_crtc *crtc)
{
	struct decon_f_context *ctx = crtc->ctx;
	unsigned long flags;

	spin_lock_irqsave(&ctx->vblank_lock, flags);

	decon_write(ctx, DECON8890_SHADOW_UPDATE,
		    DECON8890_UPDATE_GLOBAL | DECON8890_UPDATE_ALL_WIN);
	decon_f_set_trigger(ctx, true);

	exynos_crtc_handle_event(crtc);

	spin_unlock_irqrestore(&ctx->vblank_lock, flags);
}

static const struct exynos_drm_crtc_ops decon_f_crtc_ops = {
	.atomic_enable	= decon_f_atomic_enable,
	.atomic_disable	= decon_f_atomic_disable,
	.enable_vblank	= decon_f_enable_vblank,
	.disable_vblank	= decon_f_disable_vblank,
	.mode_valid	= decon_f_mode_valid,
	.atomic_begin	= decon_f_atomic_begin,
	.update_plane	= decon_f_update_plane,
	.disable_plane	= decon_f_disable_plane,
	.atomic_flush	= decon_f_atomic_flush,
};

static int decon_f_bind(struct device *dev, struct device *master, void *data)
{
	struct decon_f_context *ctx = dev_get_drvdata(dev);
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm_dev = data;
	int ret, i;

	ctx->drm_dev = drm_dev;

	for (i = 0; i < WINDOWS_NR; i++) {
		ctx->configs[i].pixel_formats = decon_f_formats;
		ctx->configs[i].num_pixel_formats = ARRAY_SIZE(decon_f_formats);
		ctx->configs[i].zpos = i;
		ctx->configs[i].type = i ? DRM_PLANE_TYPE_OVERLAY :
					   DRM_PLANE_TYPE_PRIMARY;
		/* window 0 is the bottom layer, it has nothing to blend with */
		if (i)
			ctx->configs[i].capabilities =
				EXYNOS_DRM_PLANE_CAP_PIX_BLEND |
				EXYNOS_DRM_PLANE_CAP_WIN_BLEND;

		ret = exynos_plane_init(drm_dev, &ctx->planes[i], i,
					&ctx->configs[i]);
		if (ret)
			return ret;
	}

	ctx->crtc = exynos_drm_crtc_create(drm_dev, &ctx->planes[0].base,
					   EXYNOS_DISPLAY_TYPE_LCD,
					   &decon_f_crtc_ops, ctx);
	if (IS_ERR(ctx->crtc))
		return PTR_ERR(ctx->crtc);

	/*
	 * The panel is live from the bootloader, so the sources are already
	 * asserting. Only arm the handler once the CRTC it dereferences exists.
	 */
	for (i = 0; i < DECON_F_IRQ_NR; i++) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0)
			return irq;
		ret = devm_request_irq(dev, irq, decon_f_irq_handler,
				       IRQF_NO_AUTOEN, dev_name(dev), ctx);
		if (ret)
			return ret;
		enable_irq(irq);
	}

	return 0;
}

static void decon_f_unbind(struct device *dev, struct device *master, void *data)
{
	struct decon_f_context *ctx = dev_get_drvdata(dev);

	if (!IS_ERR_OR_NULL(ctx->crtc))
		decon_f_atomic_disable(ctx->crtc);
}

static const struct component_ops decon_f_component_ops = {
	.bind	= decon_f_bind,
	.unbind	= decon_f_unbind,
};

static int decon_f_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_f_context *ctx;
	struct device_node *np;
	int ret, i;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	ctx->suspended = true;
	spin_lock_init(&ctx->vblank_lock);

	ctx->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->regs))
		return PTR_ERR(ctx->regs);

	for (i = 0; i < DECON_F_CLK_NR; i++) {
		ctx->clks[i] = devm_clk_get(dev, decon_f_clk_names[i]);
		if (IS_ERR(ctx->clks[i]))
			return dev_err_probe(dev, PTR_ERR(ctx->clks[i]),
					     "failed to get %s\n",
					     decon_f_clk_names[i]);
	}

	np = of_parse_phandle(dev->of_node, "samsung,vpp", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no samsung,vpp phandle\n");
	ctx->vpp_dev = exynos8890_vpp_get(np);
	of_node_put(np);
	if (!ctx->vpp_dev)
		return dev_err_probe(dev, -EPROBE_DEFER, "VPP not ready\n");

	platform_set_drvdata(pdev, ctx);
	pm_runtime_enable(dev);

	/*
	 * Only PCLK is needed to read the registers back, and toggling it does
	 * not disturb the pixel path the bootloader left running.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto err_pm;
	ret = clk_prepare_enable(ctx->clks[DECON_F_CLK_PCLK]);
	if (ret) {
		pm_runtime_put(dev);
		goto err_pm;
	}
	ret = decon_f_handoff_valid(ctx) ? 0 : -ENODEV;
	/*
	 * The bootloader leaves the sources armed. Silence them before any
	 * handler is attached, or the first enable_irq() storms.
	 */
	decon_write(ctx, DECON8890_INTERRUPT_ENABLE, 0);
	decon_write(ctx, DECON8890_INTERRUPT_PENDING,
		    decon_read(ctx, DECON8890_INTERRUPT_PENDING));
	clk_disable_unprepare(ctx->clks[DECON_F_CLK_PCLK]);
	pm_runtime_put(dev);
	if (ret) {
		dev_err_probe(dev, ret,
			      "no bootloader handoff: LCD-side setup is not ported\n");
		goto err_pm;
	}

	return component_add(dev, &decon_f_component_ops);

err_pm:
	pm_runtime_disable(dev);
	put_device(ctx->vpp_dev);
	return ret;
}

static void decon_f_remove(struct platform_device *pdev)
{
	struct decon_f_context *ctx = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &decon_f_component_ops);
	pm_runtime_disable(&pdev->dev);
	put_device(ctx->vpp_dev);
}

static const struct of_device_id decon_f_of_match[] = {
	{ .compatible = "samsung,exynos8890-decon-f" },
	{ },
};
MODULE_DEVICE_TABLE(of, decon_f_of_match);

struct platform_driver exynos8890_decon_f_driver = {
	.probe	= decon_f_probe,
	.remove	= decon_f_remove,
	.driver	= {
		.name		= "exynos8890-decon-f",
		.of_match_table	= decon_f_of_match,
	},
};
