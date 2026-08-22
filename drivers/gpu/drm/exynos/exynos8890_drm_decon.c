// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Exynos8890 DECON-F + IDMA_G1 inherited-display takeover.
 *
 * Probe is deliberately read-only.  The driver exposes a single immutable
 * logical mode and does not install a DRM client.  Hardware is touched only by
 * the first explicit userspace atomic commit; DECON, MIC, DSIM and the panel
 * are never reset or stopped.
 */

#include <linux/arm-smccc.h>
#include <linux/aperture.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/iopoll.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "regs-decon8890.h"
#include "regs-vpp8890.h"

#define EXYNOS8890_WIDTH	1440
#define EXYNOS8890_HEIGHT	2560
#define EXYNOS8890_PITCH	(EXYNOS8890_WIDTH * 4)
#define EXYNOS8890_FB_SIZE	(EXYNOS8890_PITCH * EXYNOS8890_HEIGHT)
#define EXYNOS8890_SPLASH_BASE	0xe2a00000U
#define EXYNOS8890_SYSMMU_BASE	0x13a10000U
#define EXYNOS8890_SYSMMU_SIZE	0x1000
#define EXYNOS8890_SYSMMU_CTRL	0x000
#define EXYNOS8890_SYSMMU_PT_BASE	0x00c
#define EXYNOS8890_SYSMMU_INT_STATUS	0x060
#define EXYNOS8890_SYSMMU_FAULT_MASK	0x001f001f
#define EXYNOS8890_TIMEOUT_US	50000

/* Exact ABI and success value used by the pinned Exynos8890 vendor kernel. */
#define EXYNOS8890_MC_FC_SET_CFW_PROT	0x82002040
#define EXYNOS8890_MC_FC_DRM_SET_CFW_PROT	0x10000000
#define EXYNOS8890_PROT_G1	4
#define EXYNOS8890_CFW_SUCCESS	2

struct exynos8890_drm {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct device *dev;
	void __iomem *decon;
	void __iomem *vpp;
	void __iomem *sysmmu;
	struct mutex commit_lock;
	struct regulator_bulk_data supplies[2];
	u32 scanout_base;
	bool taken_over;
	atomic_t terminal_fault;
	atomic_t shutting_down;
	struct drm_framebuffer *fault_fb;
	bool supplies_enabled;
	bool aperture_owned;
};

static const struct drm_driver exynos8890_drm_driver;

static inline struct exynos8890_drm *to_exynos8890(struct drm_device *drm)
{
	return container_of(drm, struct exynos8890_drm, drm);
}

static const struct drm_display_mode exynos8890_mode = {
	.clock = 223754,
	.hdisplay = 1440,
	.hsync_start = 1442,
	.hsync_end = 1444,
	.htotal = 1446,
	.vdisplay = 2560,
	.vsync_start = 2575,
	.vsync_end = 2578,
	.vtotal = 2579,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	.name = "1440x2560",
};

static bool exynos8890_decon_exact(struct exynos8890_drm *ctx)
{
	u32 global, frame0, frame1;
	unsigned int i;

	global = readl_relaxed(ctx->decon + DECON8890_GLOBAL_CONTROL);
	if ((global & ~(DECON8890_GLOBAL_IDLE | DECON8890_GLOBAL_RUN |
			DECON8890_GLOBAL_URGENT)) !=
	    (DECON8890_INHERITED_GLOBAL & ~(DECON8890_GLOBAL_IDLE |
			DECON8890_GLOBAL_RUN | DECON8890_GLOBAL_URGENT)) ||
	    !(global & DECON8890_GLOBAL_RUN) ||
	    !(global & DECON8890_GLOBAL_COMMAND_MODE) ||
	    readl_relaxed(ctx->decon + DECON8890_INTERRUPT_ENABLE) !=
		DECON8890_INHERITED_IRQ_ENABLE ||
	    readl_relaxed(ctx->decon + DECON8890_SHADOW_UPDATE) ||
	    readl_relaxed(ctx->decon + DECON8890_TRIGGER_CONTROL) !=
		DECON8890_INHERITED_TRIGGER ||
	    readl_relaxed(ctx->decon + DECON8890_DATA_PATH_CONTROL) !=
		DECON8890_INHERITED_PATH ||
	    readl_relaxed(ctx->decon + DECON8890_DISPIF_SIZE) !=
		DECON8890_INHERITED_SIZE)
		return false;

	if (readl_relaxed(ctx->decon + DECON8890_WIN_CONTROL(0)) !=
		DECON8890_INHERITED_WIN0 ||
	    readl_relaxed(ctx->decon + DECON8890_WIN_START(0)) !=
		DECON8890_INHERITED_WIN_START ||
	    readl_relaxed(ctx->decon + DECON8890_WIN_END(0)) !=
		DECON8890_INHERITED_WIN_END ||
	    readl_relaxed(ctx->decon + DECON8890_WIN_PIXEL_COUNT(0)) !=
		DECON8890_INHERITED_PIXELS)
		return false;

	for (i = 1; i < 8; i++)
		if (readl_relaxed(ctx->decon + DECON8890_WIN_CONTROL(i)) !=
			DECON8890_INHERITED_DISABLED_WIN ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_START(i)) !=
			DECON8890_INHERITED_DISABLED_START ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_END(i)) !=
			DECON8890_INHERITED_DISABLED_END ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_PIXEL_COUNT(i)) !=
			DECON8890_INHERITED_DISABLED_PIXELS)
			return false;

	/* A clear request register makes direct and shadow state comparable. */
	for (i = 0; i < 8; i++)
		if (readl_relaxed(ctx->decon + DECON8890_WIN_CONTROL(i)) !=
		    readl_relaxed(ctx->decon + DECON8890_SHADOW_OFFSET +
				  DECON8890_WIN_CONTROL(i)) ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_START(i)) !=
		    readl_relaxed(ctx->decon + DECON8890_SHADOW_OFFSET +
				  DECON8890_WIN_START(i)) ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_END(i)) !=
		    readl_relaxed(ctx->decon + DECON8890_SHADOW_OFFSET +
				  DECON8890_WIN_END(i)) ||
		    readl_relaxed(ctx->decon + DECON8890_WIN_PIXEL_COUNT(i)) !=
		    readl_relaxed(ctx->decon + DECON8890_SHADOW_OFFSET +
				  DECON8890_WIN_PIXEL_COUNT(i)))
			return false;

	frame0 = readl_relaxed(ctx->decon + DECON8890_FRAME_ID);
	msleep(100);
	frame1 = readl_relaxed(ctx->decon + DECON8890_FRAME_ID);
	return frame0 != frame1;
}

static bool exynos8890_vpp_exact(struct exynos8890_drm *ctx)
{
	return readl_relaxed(ctx->vpp + VPP8890_ENABLE) ==
			VPP8890_INHERITED_ENABLE &&
	       readl_relaxed(ctx->vpp + VPP8890_IRQ) ==
			VPP8890_INHERITED_IRQ &&
	       readl_relaxed(ctx->vpp + VPP8890_IN_CON) ==
			VPP8890_INHERITED_IN_CON &&
	       readl_relaxed(ctx->vpp + VPP8890_OUT_CON) ==
			VPP8890_INHERITED_OUT_CON &&
	       readl_relaxed(ctx->vpp + VPP8890_SRC_SIZE) ==
			VPP8890_INHERITED_SIZE &&
	       readl_relaxed(ctx->vpp + VPP8890_SRC_OFFSET) ==
			VPP8890_INHERITED_OFFSET &&
	       readl_relaxed(ctx->vpp + VPP8890_IMG_SIZE) ==
			VPP8890_INHERITED_SIZE &&
	       readl_relaxed(ctx->vpp + VPP8890_PINGPONG_UPDATE) == 0 &&
	       readl_relaxed(ctx->vpp + VPP8890_CHROMINANCE_STRIDE) == 0 &&
	       readl_relaxed(ctx->vpp + VPP8890_BLOCK_OFFSET) == 0 &&
	       readl_relaxed(ctx->vpp + VPP8890_BLOCK_SIZE) == 0 &&
	       readl_relaxed(ctx->vpp + VPP8890_SCALED_SIZE) ==
			VPP8890_INHERITED_SCALED &&
	       readl_relaxed(ctx->vpp + VPP8890_H_RATIO) ==
			VPP8890_INHERITED_RATIO &&
	       readl_relaxed(ctx->vpp + VPP8890_V_RATIO) ==
			VPP8890_INHERITED_RATIO &&
	       readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_CON) ==
			VPP8890_INHERITED_BASE_CON &&
	       readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_Y0) ==
			VPP8890_INHERITED_BASE &&
	       readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_CB0) ==
			VPP8890_INHERITED_CB_BASE &&
	       readl_relaxed(ctx->vpp + VPP8890_SHADOW_BASE_ADDR_Y) ==
			VPP8890_INHERITED_BASE &&
	       readl_relaxed(ctx->vpp + VPP8890_SHADOW_BASE_ADDR_CB) ==
			VPP8890_INHERITED_CB_BASE &&
	       readl_relaxed(ctx->vpp + VPP8890_QOS_LUT07_00) ==
			VPP8890_INHERITED_QOS &&
	       readl_relaxed(ctx->vpp + VPP8890_QOS_LUT15_08) ==
			VPP8890_INHERITED_QOS &&
	       readl_relaxed(ctx->vpp + VPP8890_SMART_IF_PIXEL_NUM) ==
			VPP8890_INHERITED_SMART_PIXELS &&
	       readl_relaxed(ctx->vpp + VPP8890_DEADLOCK_NUM) ==
			VPP8890_INHERITED_DEADLOCK &&
	       readl_relaxed(ctx->vpp + VPP8890_DYNAMIC_GATING_ENABLE) ==
			VPP8890_INHERITED_GATING;
}

static bool exynos8890_sysmmu_exact(struct exynos8890_drm *ctx)
{
	return readl_relaxed(ctx->sysmmu + EXYNOS8890_SYSMMU_CTRL) == 0 &&
	       readl_relaxed(ctx->sysmmu + EXYNOS8890_SYSMMU_PT_BASE) == 0 &&
	       !(readl_relaxed(ctx->sysmmu + EXYNOS8890_SYSMMU_INT_STATUS) &
		 EXYNOS8890_SYSMMU_FAULT_MASK);
}

static bool exynos8890_dma_path_exact(struct exynos8890_drm *ctx)
{
	return exynos8890_sysmmu_exact(ctx) &&
	       !iommu_get_domain_for_dev(ctx->dev);
}

static int exynos8890_dma_addr_valid(dma_addr_t dma_addr)
{
	if (dma_addr > (dma_addr_t)U32_MAX - (EXYNOS8890_FB_SIZE - 1))
		return -ERANGE;

	return 0;
}

static int exynos8890_atomic_errno(int ret)
{
	switch (ret) {
	case -EBUSY:
	case -ENOMEM:
	case -ENOSPC:
	case -EIO:
	case -EINTR:
	case -EAGAIN:
	case -ERESTARTSYS:
		return ret;
	default:
		return -EIO;
	}
}

static int exynos8890_poll_clear(void __iomem *addr, u32 mask)
{
	u32 value;

	return readl_poll_timeout(addr, value, !(value & mask), 10,
				  EXYNOS8890_TIMEOUT_US);
}

static int exynos8890_unprotect_g1(struct exynos8890_drm *ctx)
{
	struct arm_smccc_res res;

	arm_smccc_smc(EXYNOS8890_MC_FC_SET_CFW_PROT,
		      EXYNOS8890_MC_FC_DRM_SET_CFW_PROT,
		      EXYNOS8890_PROT_G1, 0, 0, 0, 0, 0, &res);
	if (res.a0 != EXYNOS8890_CFW_SUCCESS) {
		dev_err(ctx->dev, "G1 CFW unprotect SMC returned %#lx\n",
			res.a0);
		return -EACCES;
	}

	return 0;
}

static int exynos8890_vpp_init(struct exynos8890_drm *ctx, u32 base)
{
	u32 value;
	int ret;

	if (!exynos8890_dma_path_exact(ctx))
		return -ENODEV;

	value = readl_relaxed(ctx->vpp + VPP8890_ENABLE);
	writel_relaxed(value | VPP8890_ENABLE_SRESET,
		       ctx->vpp + VPP8890_ENABLE);
	ret = exynos8890_poll_clear(ctx->vpp + VPP8890_ENABLE,
				   VPP8890_ENABLE_SRESET);
	if (ret)
		return ret;

	writel_relaxed(VPP8890_IRQ_STATUS_MASK, ctx->vpp + VPP8890_IRQ);
	writel_relaxed(VPP8890_IRQ_ALL_MASKED, ctx->vpp + VPP8890_IRQ);
	writel_relaxed(VPP8890_ENABLE_RT_PATH, ctx->vpp + VPP8890_ENABLE);
	writel_relaxed(VPP8890_INHERITED_IN_CON, ctx->vpp + VPP8890_IN_CON);
	writel_relaxed(VPP8890_INHERITED_OUT_CON, ctx->vpp + VPP8890_OUT_CON);
	writel_relaxed(VPP8890_INHERITED_SIZE, ctx->vpp + VPP8890_SRC_SIZE);
	writel_relaxed(VPP8890_INHERITED_OFFSET, ctx->vpp + VPP8890_SRC_OFFSET);
	writel_relaxed(VPP8890_INHERITED_SIZE, ctx->vpp + VPP8890_IMG_SIZE);
	writel_relaxed(0, ctx->vpp + VPP8890_CHROMINANCE_STRIDE);
	writel_relaxed(0, ctx->vpp + VPP8890_BLOCK_OFFSET);
	writel_relaxed(0, ctx->vpp + VPP8890_BLOCK_SIZE);
	writel_relaxed(VPP8890_INHERITED_SCALED, ctx->vpp + VPP8890_SCALED_SIZE);
	writel_relaxed(VPP8890_INHERITED_RATIO, ctx->vpp + VPP8890_H_RATIO);
	writel_relaxed(VPP8890_INHERITED_RATIO, ctx->vpp + VPP8890_V_RATIO);
	writel_relaxed(VPP8890_INHERITED_QOS,
		       ctx->vpp + VPP8890_QOS_LUT07_00);
	writel_relaxed(VPP8890_INHERITED_QOS,
		       ctx->vpp + VPP8890_QOS_LUT15_08);
	/* BASE_ADDR_CON is undocumented and is intentionally preserved. */
	if (!exynos8890_dma_path_exact(ctx))
		return -ENODEV;
	writel_relaxed(base, ctx->vpp + VPP8890_BASE_ADDR_Y0);
	writel_relaxed(VPP8890_INHERITED_CB_BASE,
		       ctx->vpp + VPP8890_BASE_ADDR_CB0);
	writel_relaxed(VPP8890_INHERITED_DEADLOCK,
		       ctx->vpp + VPP8890_DEADLOCK_NUM);
	writel_relaxed(VPP8890_INHERITED_SMART_PIXELS,
		       ctx->vpp + VPP8890_SMART_IF_PIXEL_NUM);
	writel_relaxed(VPP8890_INHERITED_GATING,
		       ctx->vpp + VPP8890_DYNAMIC_GATING_ENABLE);
	writel_relaxed(VPP8890_PINGPONG_REQUEST,
		       ctx->vpp + VPP8890_PINGPONG_UPDATE);
	wmb();
	return 0;
}

static void exynos8890_trigger(struct exynos8890_drm *ctx)
{
	u32 trigger = readl_relaxed(ctx->decon + DECON8890_TRIGGER_CONTROL);

	if (trigger & DECON8890_TRIGGER_HW_ENABLE)
		writel_relaxed(trigger & ~DECON8890_TRIGGER_HW_MASK,
			       ctx->decon + DECON8890_TRIGGER_CONTROL);
	else
		writel_relaxed(trigger | DECON8890_TRIGGER_SW,
			       ctx->decon + DECON8890_TRIGGER_CONTROL);
}

static int exynos8890_verify_commit(struct exynos8890_drm *ctx, u32 base,
				   u32 old_frame)
{
	u32 value;
	int ret;

	ret = exynos8890_poll_clear(ctx->vpp + VPP8890_PINGPONG_UPDATE,
				   VPP8890_PINGPONG_REQUEST);
	if (ret)
		return ret;
	ret = readl_poll_timeout(ctx->vpp + VPP8890_SHADOW_BASE_ADDR_Y,
				 value, value == base, 10,
				 EXYNOS8890_TIMEOUT_US);
	if (ret)
		return ret;
	ret = exynos8890_poll_clear(ctx->decon + DECON8890_SHADOW_UPDATE,
				   DECON8890_UPDATE_GLOBAL |
				   DECON8890_UPDATE_WIN(0));
	if (ret)
		return ret;
	return readl_poll_timeout(ctx->decon + DECON8890_FRAME_ID,
				 value, value != old_frame, 10,
				 EXYNOS8890_TIMEOUT_US);
}

/* The caller keeps the DECON trigger masked around programming and rollback. */
static int exynos8890_restore_base_masked(struct exynos8890_drm *ctx, u32 base)
{
	u32 frame;
	int ret;

	ret = exynos8890_vpp_init(ctx, base);
	if (ret)
		return ret;
	frame = readl_relaxed(ctx->decon + DECON8890_FRAME_ID);
	exynos8890_trigger(ctx);
	return exynos8890_verify_commit(ctx, base, frame);
}

static void exynos8890_vpp_restore_stopped(struct exynos8890_drm *ctx)
{
	/* Restore the exact bootloader-retained, inactive G1 state. */
	writel_relaxed(VPP8890_INHERITED_IRQ, ctx->vpp + VPP8890_IRQ);
	writel_relaxed(VPP8890_INHERITED_ENABLE, ctx->vpp + VPP8890_ENABLE);
	wmb();
}

static int exynos8890_program_first(struct exynos8890_drm *ctx, u32 base)
{
	u32 frame, trigger;
	bool vpp_touched = false;
	int rollback_ret;
	int ret;

	/* Probe was read-only; reject any inherited-state drift at takeover. */
	if (!exynos8890_dma_path_exact(ctx) || !exynos8890_vpp_exact(ctx) ||
	    !exynos8890_decon_exact(ctx))
		return -ENODEV;

	trigger = readl_relaxed(ctx->decon + DECON8890_TRIGGER_CONTROL);
	writel_relaxed(trigger | DECON8890_TRIGGER_HW_MASK,
		       ctx->decon + DECON8890_TRIGGER_CONTROL);
	wmb();

	ret = exynos8890_poll_clear(ctx->decon + DECON8890_SHADOW_UPDATE,
				   ~0U);
	if (ret)
		goto out_trigger;
	ret = exynos8890_unprotect_g1(ctx);
	if (ret)
		goto out_trigger;

	vpp_touched = true;
	ret = exynos8890_vpp_init(ctx, base);
	if (ret)
		goto rollback;

	writel_relaxed(DECON8890_INHERITED_WIN0,
		       ctx->decon + DECON8890_WIN_CONTROL(0));
	writel_relaxed(DECON8890_INHERITED_WIN_START,
		       ctx->decon + DECON8890_WIN_START(0));
	writel_relaxed(DECON8890_INHERITED_WIN_END,
		       ctx->decon + DECON8890_WIN_END(0));
	writel_relaxed(DECON8890_INHERITED_PIXELS,
		       ctx->decon + DECON8890_WIN_PIXEL_COUNT(0));
	writel_relaxed(DECON8890_UPDATE_WIN(0),
		       ctx->decon + DECON8890_SHADOW_UPDATE);
	writel_relaxed(readl_relaxed(ctx->decon + DECON8890_GLOBAL_CONTROL) |
		       DECON8890_GLOBAL_ENABLE | DECON8890_GLOBAL_ENABLE_F,
		       ctx->decon + DECON8890_GLOBAL_CONTROL);
	writel_relaxed(DECON8890_UPDATE_GLOBAL,
		       ctx->decon + DECON8890_SHADOW_UPDATE);
	wmb();
	frame = readl_relaxed(ctx->decon + DECON8890_FRAME_ID);
	exynos8890_trigger(ctx);
	ret = exynos8890_verify_commit(ctx, base, frame);
	if (!ret)
		goto out_trigger;

rollback:
	dev_err(ctx->dev, "takeover failed (%d), restoring splash base\n", ret);
	if (vpp_touched) {
		writel_relaxed(trigger | DECON8890_TRIGGER_HW_MASK,
			       ctx->decon + DECON8890_TRIGGER_CONTROL);
		rollback_ret = exynos8890_restore_base_masked(ctx,
						      EXYNOS8890_SPLASH_BASE);
		if (rollback_ret) {
			atomic_set(&ctx->terminal_fault, 1);
			dev_crit(ctx->dev,
				 "splash rollback could not be verified: %d\n",
				 rollback_ret);
		} else {
			exynos8890_vpp_restore_stopped(ctx);
		}
	}

out_trigger:
	writel_relaxed(trigger, ctx->decon + DECON8890_TRIGGER_CONTROL);
	wmb();
	return ret;
}

static int exynos8890_program_flip(struct exynos8890_drm *ctx, u32 base,
				  u32 old_base)
{
	u32 frame, trigger;
	bool base_written = false;
	int rollback_ret;
	int ret;

	/* Reject drift outside the one field this transaction owns. */
	if (!exynos8890_dma_path_exact(ctx) || !exynos8890_decon_exact(ctx) ||
	    readl_relaxed(ctx->vpp + VPP8890_ENABLE) != VPP8890_ENABLE_RT_PATH ||
	    readl_relaxed(ctx->vpp + VPP8890_IRQ) != VPP8890_IRQ_ALL_MASKED ||
	    readl_relaxed(ctx->vpp + VPP8890_IN_CON) != VPP8890_INHERITED_IN_CON ||
	    readl_relaxed(ctx->vpp + VPP8890_OUT_CON) != VPP8890_INHERITED_OUT_CON ||
	    readl_relaxed(ctx->vpp + VPP8890_SRC_SIZE) != VPP8890_INHERITED_SIZE ||
	    readl_relaxed(ctx->vpp + VPP8890_SRC_OFFSET) != VPP8890_INHERITED_OFFSET ||
	    readl_relaxed(ctx->vpp + VPP8890_IMG_SIZE) != VPP8890_INHERITED_SIZE ||
	    readl_relaxed(ctx->vpp + VPP8890_CHROMINANCE_STRIDE) != 0 ||
	    readl_relaxed(ctx->vpp + VPP8890_BLOCK_OFFSET) != 0 ||
	    readl_relaxed(ctx->vpp + VPP8890_BLOCK_SIZE) != 0 ||
	    readl_relaxed(ctx->vpp + VPP8890_SCALED_SIZE) != VPP8890_INHERITED_SCALED ||
	    readl_relaxed(ctx->vpp + VPP8890_H_RATIO) != VPP8890_INHERITED_RATIO ||
	    readl_relaxed(ctx->vpp + VPP8890_V_RATIO) != VPP8890_INHERITED_RATIO ||
	    readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_CON) != VPP8890_INHERITED_BASE_CON ||
	    readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_Y0) != old_base ||
	    readl_relaxed(ctx->vpp + VPP8890_SHADOW_BASE_ADDR_Y) != old_base ||
	    readl_relaxed(ctx->vpp + VPP8890_BASE_ADDR_CB0) !=
			VPP8890_INHERITED_CB_BASE ||
	    readl_relaxed(ctx->vpp + VPP8890_SHADOW_BASE_ADDR_CB) !=
			VPP8890_INHERITED_CB_BASE ||
	    readl_relaxed(ctx->vpp + VPP8890_PINGPONG_UPDATE) ||
	    readl_relaxed(ctx->vpp + VPP8890_QOS_LUT07_00) != VPP8890_INHERITED_QOS ||
	    readl_relaxed(ctx->vpp + VPP8890_QOS_LUT15_08) != VPP8890_INHERITED_QOS ||
	    readl_relaxed(ctx->vpp + VPP8890_DEADLOCK_NUM) != VPP8890_INHERITED_DEADLOCK ||
	    readl_relaxed(ctx->vpp + VPP8890_SMART_IF_PIXEL_NUM) !=
			VPP8890_INHERITED_SMART_PIXELS ||
	    readl_relaxed(ctx->vpp + VPP8890_DYNAMIC_GATING_ENABLE) != VPP8890_INHERITED_GATING)
		return -ENODEV;

	trigger = readl_relaxed(ctx->decon + DECON8890_TRIGGER_CONTROL);
	writel_relaxed(trigger | DECON8890_TRIGGER_HW_MASK,
		       ctx->decon + DECON8890_TRIGGER_CONTROL);
	wmb();

	ret = exynos8890_poll_clear(ctx->vpp + VPP8890_PINGPONG_UPDATE,
				   VPP8890_PINGPONG_REQUEST);
	if (ret)
		goto out_trigger;
	if (!exynos8890_dma_path_exact(ctx)) {
		ret = -ENODEV;
		goto out_trigger;
	}

	writel_relaxed(base, ctx->vpp + VPP8890_BASE_ADDR_Y0);
	base_written = true;
	writel_relaxed(VPP8890_INHERITED_CB_BASE,
		       ctx->vpp + VPP8890_BASE_ADDR_CB0);
	writel_relaxed(VPP8890_PINGPONG_REQUEST,
		       ctx->vpp + VPP8890_PINGPONG_UPDATE);
	wmb();
	frame = readl_relaxed(ctx->decon + DECON8890_FRAME_ID);
	exynos8890_trigger(ctx);
	ret = exynos8890_verify_commit(ctx, base, frame);
	if (!ret)
		goto out_trigger;

	dev_err(ctx->dev, "flip failed (%d), restoring base %#x\n", ret,
		old_base);
	if (base_written) {
		writel_relaxed(trigger | DECON8890_TRIGGER_HW_MASK,
			       ctx->decon + DECON8890_TRIGGER_CONTROL);
		rollback_ret = exynos8890_restore_base_masked(ctx, old_base);
		if (rollback_ret) {
			atomic_set(&ctx->terminal_fault, 1);
			dev_crit(ctx->dev,
				 "last-base rollback could not be verified: %d\n",
				 rollback_ret);
		}
	}

out_trigger:
	writel_relaxed(trigger, ctx->decon + DECON8890_TRIGGER_CONTROL);
	wmb();
	return ret;
}

static enum drm_mode_status
exynos8890_mode_valid(struct drm_simple_display_pipe *pipe,
		      const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &exynos8890_mode) ? MODE_OK : MODE_BAD;
}

static int exynos8890_pipe_check(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *plane_state,
				struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;

	if (!crtc_state->enable || !crtc_state->active || !fb ||
	    plane_state->crtc != &pipe->crtc)
		return -EINVAL;
	if (!drm_mode_equal(&crtc_state->adjusted_mode, &exynos8890_mode) ||
	    fb->format->format != DRM_FORMAT_ARGB8888 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    fb->width != EXYNOS8890_WIDTH || fb->height != EXYNOS8890_HEIGHT ||
	    fb->pitches[0] != EXYNOS8890_PITCH || fb->offsets[0] ||
	    plane_state->crtc_x || plane_state->crtc_y ||
	    plane_state->crtc_w != EXYNOS8890_WIDTH ||
	    plane_state->crtc_h != EXYNOS8890_HEIGHT ||
	    plane_state->src_x || plane_state->src_y ||
	    plane_state->src_w != (EXYNOS8890_WIDTH << 16) ||
	    plane_state->src_h != (EXYNOS8890_HEIGHT << 16))
		return -EINVAL;
	return 0;
}

static int exynos8890_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
				      struct drm_plane_state *state)
{
	dma_addr_t dma_addr;
	int ret;

	/* A disabled plane has no backing object to prepare. */
	if (!state->fb || !state->crtc)
		return 0;
	ret = drm_gem_plane_helper_prepare_fb(&pipe->plane, state);
	if (ret)
		return ret;

	dma_addr = drm_fb_dma_get_gem_addr(state->fb, state, 0);
	ret = exynos8890_dma_addr_valid(dma_addr);
	if (ret)
		drm_err(pipe->plane.dev,
			"rejecting scanout span outside 32-bit DMA: %pad + %#x\n",
			&dma_addr, EXYNOS8890_FB_SIZE);

	return ret;
}

static int exynos8890_take_aperture(struct exynos8890_drm *ctx)
{
	struct platform_device *pdev = to_platform_device(ctx->dev);
	int ret;

	aperture_remove_conflicting_devices(EXYNOS8890_SPLASH_BASE,
					    EXYNOS8890_FB_SIZE,
					    exynos8890_drm_driver.name);
	ret = devm_aperture_acquire_for_platform_device(pdev,
							EXYNOS8890_SPLASH_BASE,
							EXYNOS8890_FB_SIZE);
	if (!ret)
		ctx->aperture_owned = true;

	return ret;
}

static int exynos8890_atomic_check(struct drm_device *drm,
				  struct drm_atomic_commit *state)
{
	struct exynos8890_drm *ctx = to_exynos8890(drm);
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	int ret;

	ret = drm_atomic_helper_check(drm, state);
	if (ret)
		return ret;
	if (state->async_update)
		return -EINVAL;
	if (atomic_read(&ctx->shutting_down))
		return -ESHUTDOWN;

	plane_state = drm_atomic_get_new_plane_state(state, &ctx->pipe.plane);
	if (!plane_state)
		plane_state = ctx->pipe.plane.state;
	crtc_state = drm_atomic_get_new_crtc_state(state, &ctx->pipe.crtc);
	if (!crtc_state)
		crtc_state = ctx->pipe.crtc.state;

	/* Terminal faults reject new scanout but never block a draining disable. */
	if (atomic_read(&ctx->terminal_fault) && plane_state->fb &&
	    plane_state->crtc && crtc_state->active)
		return -EIO;

	return 0;
}

static void exynos8890_pipe_enable(struct drm_simple_display_pipe *pipe,
				   struct drm_crtc_state *crtc_state,
				   struct drm_plane_state *plane_state)
{
	/* The first explicit modeset reaches update; inherited output stays on. */
}

static void exynos8890_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	/* Intentionally never stop inherited output. */
}

static const struct drm_simple_display_pipe_funcs exynos8890_pipe_funcs = {
	.mode_valid = exynos8890_mode_valid,
	.check = exynos8890_pipe_check,
	.prepare_fb = exynos8890_pipe_prepare_fb,
	.enable = exynos8890_pipe_enable,
	.disable = exynos8890_pipe_disable,
};

/*
 * A deliberately synchronous, pre-swap commit.  The new atomic state owns the
 * candidate framebuffer while hardware is programmed.  Software state is
 * swapped only after G1 shadow-base and DECON frame progress are verified; any
 * error therefore leaves the old DRM state installed and the transaction
 * helper has restored the old physical scanout before this function returns.
 */
static int exynos8890_atomic_commit(struct drm_device *drm,
				    struct drm_atomic_commit *state,
				    bool nonblock)
{
	struct exynos8890_drm *ctx = to_exynos8890(drm);
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	dma_addr_t dma_addr = 0;
	bool enable, first = false, supplies_started = false;
	u32 base = EXYNOS8890_SPLASH_BASE;
	int rollback_ret, ret;

	/* This fail-reporting transaction is deliberately blocking-only. */
	if (nonblock)
		return -EBUSY;

	ret = drm_atomic_helper_prepare_planes(drm, state);
	if (ret)
		return exynos8890_atomic_errno(ret);
	ret = drm_atomic_helper_wait_for_fences(drm, state, true);
	if (ret)
		goto out_unprepare;

	plane_state = drm_atomic_get_new_plane_state(state, &ctx->pipe.plane);
	if (!plane_state)
		plane_state = ctx->pipe.plane.state;
	crtc_state = drm_atomic_get_new_crtc_state(state, &ctx->pipe.crtc);
	if (!crtc_state)
		crtc_state = ctx->pipe.crtc.state;
	enable = plane_state->fb && plane_state->crtc && crtc_state->active;
	if (enable) {
		dma_addr = drm_fb_dma_get_gem_addr(plane_state->fb,
					       plane_state, 0);
		ret = exynos8890_dma_addr_valid(dma_addr);
		if (ret)
			goto out_unprepare;
		base = lower_32_bits(dma_addr);
	}

	mutex_lock(&ctx->commit_lock);
	if (atomic_read(&ctx->shutting_down)) {
		ret = -EIO;
		goto out_unlock;
	}
	if (enable && atomic_read(&ctx->terminal_fault)) {
		ret = -EIO;
		goto out_unlock;
	}

	if (!enable) {
		if (ctx->taken_over) {
			ret = exynos8890_program_flip(ctx, EXYNOS8890_SPLASH_BASE,
						      ctx->scanout_base);
			if (ret)
				goto out_unlock;
			exynos8890_vpp_restore_stopped(ctx);
		}
	} else {
		first = !ctx->taken_over;
		if (first && !ctx->supplies_enabled) {
			ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies),
						    ctx->supplies);
			if (ret)
				goto out_unlock;
			ctx->supplies_enabled = true;
			supplies_started = true;
		}

		if (first)
			ret = exynos8890_program_first(ctx, base);
		else
			ret = exynos8890_program_flip(ctx, base,
						      ctx->scanout_base);
		if (ret) {
			if (atomic_read(&ctx->terminal_fault) && plane_state->fb &&
			    !ctx->fault_fb) {
				drm_framebuffer_get(plane_state->fb);
				ctx->fault_fb = plane_state->fb;
			}
			goto out_disable_supplies;
		}

		/* Retire simplefb only after the first transaction is verified. */
		if (first && !ctx->aperture_owned) {
			ret = exynos8890_take_aperture(ctx);
			if (ret) {
				rollback_ret = exynos8890_program_flip(ctx,
						EXYNOS8890_SPLASH_BASE, base);
				if (rollback_ret) {
					atomic_set(&ctx->terminal_fault, 1);
					if (!ctx->fault_fb) {
						drm_framebuffer_get(plane_state->fb);
						ctx->fault_fb = plane_state->fb;
					}
					dev_crit(ctx->dev,
						 "aperture rollback unsafe: %d\n",
						 rollback_ret);
				} else {
					exynos8890_vpp_restore_stopped(ctx);
				}
				goto out_disable_supplies;
			}
		}
	}

	/* stall=false cannot fail; this driver has no asynchronous commit tails. */
	ret = drm_atomic_helper_swap_state(state, false);
	WARN_ON(ret);
	if (enable) {
		ctx->scanout_base = base;
		ctx->taken_over = true;
	} else {
		ctx->scanout_base = EXYNOS8890_SPLASH_BASE;
		ctx->taken_over = false;
	}
	mutex_unlock(&ctx->commit_lock);

	/* Events describe the already-verified transaction, never an attempted one. */
	crtc_state = ctx->pipe.crtc.state;
	crtc_state->no_vblank = true;
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_cleanup_planes(drm, state);
	return 0;

out_disable_supplies:
	if (supplies_started) {
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		ctx->supplies_enabled = false;
	}
out_unlock:
	mutex_unlock(&ctx->commit_lock);
out_unprepare:
	drm_atomic_helper_unprepare_planes(drm, state);
	return exynos8890_atomic_errno(ret);
}

static int exynos8890_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &exynos8890_mode);
}

static const struct drm_connector_helper_funcs exynos8890_connector_helper_funcs = {
	.get_modes = exynos8890_connector_get_modes,
};

static enum drm_connector_status
exynos8890_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_funcs exynos8890_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = exynos8890_connector_detect,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs exynos8890_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = exynos8890_atomic_check,
	.atomic_commit = exynos8890_atomic_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(exynos8890_fops);

static const struct drm_driver exynos8890_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &exynos8890_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	.name = "exynos8890-decon",
	.desc = "Exynos8890 inherited DECON/G1 takeover",
	.major = 1,
	.minor = 0,
};

static int exynos8890_drm_probe(struct platform_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_ARGB8888 };
	struct exynos8890_drm *ctx;
	struct resource *res;
	int ret;

	ctx = devm_drm_dev_alloc(&pdev->dev, &exynos8890_drm_driver,
				 struct exynos8890_drm, drm);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dev = &pdev->dev;
	mutex_init(&ctx->commit_lock);
	atomic_set(&ctx->terminal_fault, 0);
	atomic_set(&ctx->shutting_down, 0);

	ctx->decon = devm_platform_ioremap_resource_byname(pdev, "decon");
	if (IS_ERR(ctx->decon))
		return PTR_ERR(ctx->decon);
	ctx->vpp = devm_platform_ioremap_resource_byname(pdev, "vpp-g1");
	if (IS_ERR(ctx->vpp))
		return PTR_ERR(ctx->vpp);
	/* Do not reserve this range: the read-only SysMMU observer retains it. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sysmmu-disp01");
	if (!res)
		return -EINVAL;
	ctx->sysmmu = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!ctx->sysmmu)
		return -ENOMEM;

	if (!exynos8890_dma_path_exact(ctx))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "DISP01 SysMMU is not exact physical mode\n");
	if (!exynos8890_vpp_exact(ctx))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "G1 retained state is not exact\n");
	if (!exynos8890_decon_exact(ctx))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "DECON inherited state is not exact\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ctx->supplies[0].supply = "vci";
	ctx->supplies[1].supply = "vdd3";
	ret = devm_regulator_bulk_get(&pdev->dev, ARRAY_SIZE(ctx->supplies),
				       ctx->supplies);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "could not acquire panel supplies\n");
	platform_set_drvdata(pdev, ctx);

	ret = drmm_mode_config_init(&ctx->drm);
	if (ret)
		return ret;
	ctx->drm.mode_config.funcs = &exynos8890_mode_config_funcs;
	ctx->drm.mode_config.min_width = EXYNOS8890_WIDTH;
	ctx->drm.mode_config.max_width = EXYNOS8890_WIDTH;
	ctx->drm.mode_config.min_height = EXYNOS8890_HEIGHT;
	ctx->drm.mode_config.max_height = EXYNOS8890_HEIGHT;
	ctx->drm.mode_config.preferred_depth = 32;

	ret = drm_connector_init(&ctx->drm, &ctx->connector,
				 &exynos8890_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;
	drm_connector_helper_add(&ctx->connector,
				 &exynos8890_connector_helper_funcs);
	ctx->connector.polled = 0;

	ret = drm_simple_display_pipe_init(&ctx->drm, &ctx->pipe,
					   &exynos8890_pipe_funcs,
					   formats, ARRAY_SIZE(formats), NULL,
					   &ctx->connector);
	if (ret)
		return ret;
	drm_plane_create_zpos_immutable_property(&ctx->pipe.plane, 0);
	drm_mode_config_reset(&ctx->drm);

	ret = drm_dev_register(&ctx->drm, 0);
	if (ret)
		return ret;

	dev_info(&pdev->dev,
		 "registered read-only; takeover waits for explicit atomic commit\n");
	return 0;
}

static void exynos8890_drm_shutdown(struct platform_device *pdev)
{
	struct exynos8890_drm *ctx = platform_get_drvdata(pdev);
	int ret = 0;

	/* Stop newly admitted commits, then drain any transaction already running. */
	atomic_set(&ctx->shutting_down, 1);
	mutex_lock(&ctx->commit_lock);
	if (ctx->taken_over) {
		ret = exynos8890_program_flip(ctx, EXYNOS8890_SPLASH_BASE,
					      ctx->scanout_base);
		if (!ret) {
			exynos8890_vpp_restore_stopped(ctx);
			ctx->scanout_base = EXYNOS8890_SPLASH_BASE;
			ctx->taken_over = false;
		}
	}
	mutex_unlock(&ctx->commit_lock);
	if (ret) {
		atomic_set(&ctx->terminal_fault, 1);
		dev_crit(ctx->dev,
			 "shutdown could not restore splash safely: %d\n", ret);
	}
}

static const struct of_device_id exynos8890_drm_of_match[] = {
	{ .compatible = "samsung,exynos8890-decon-g1-takeover" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_drm_of_match);

static struct platform_driver exynos8890_drm_platform_driver = {
	.probe = exynos8890_drm_probe,
	.shutdown = exynos8890_drm_shutdown,
	.driver = {
		.name = "exynos8890-drm-decon",
		.of_match_table = exynos8890_drm_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos8890_drm_platform_driver);

MODULE_DESCRIPTION("Minimal Exynos8890 DECON/G1 inherited takeover DRM driver");
MODULE_LICENSE("GPL");
