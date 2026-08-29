// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 VPP - the window DMA engines behind DECON-F.
 *
 * DISP0 has four of them (IDMA_G0/G1/VG0/VG1) at 0x13951000..0x13954000.
 * They are modelled as one device because they share two ACLK gates, one
 * power domain and a pair of SysMMUs, and because a per-IP device would put
 * two masters in each IOMMU group - which iommu_attach_device() rejects.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <linux/arm-smccc.h>

#include <drm/drm_fourcc.h>
#include <drm/exynos_drm.h>

#include "exynos8890_vpp.h"
#include "exynos_drm_drv.h"
#include "regs-vpp8890.h"

/* Content Firewall protection ids, vendor smc.h - display-side entries only */
#define MC_FC_SET_CFW_PROT		0x82002040
#define MC_FC_DRM_SET_CFW_PROT		0x10000000
#define CFW_PROT_G0			3
#define CFW_PROT_G1			4
#define CFW_SMC_TZPC_OK			2

#define VPP8890_OP_TIMEOUT_US		1000000
#define VPP8890_RESET_TIMEOUT_US	1000000
#define VPP8890_PINGPONG_TIMEOUT_US	17000

struct vpp_context {
	struct device *dev;
	struct drm_device *drm_dev;
	void *dma_priv;
	void __iomem *regs[VPP8890_CH_NR];
	struct clk *aclk[2];
	bool enabled;
};

static inline u32 vpp_read(struct vpp_context *ctx, unsigned int ch, u32 reg)
{
	return readl(ctx->regs[ch] + reg);
}

static inline void vpp_write(struct vpp_context *ctx, unsigned int ch, u32 reg, u32 val)
{
	writel(val, ctx->regs[ch] + reg);
}

static inline void vpp_update(struct vpp_context *ctx, unsigned int ch, u32 reg,
			      u32 val, u32 mask)
{
	u32 old = vpp_read(ctx, ch, reg);

	vpp_write(ctx, ch, reg, (old & ~mask) | (val & mask));
}

u32 exynos8890_vpp_chmap(unsigned int ch)
{
	static const u32 chmap[VPP8890_CH_NR] = {
		[VPP8890_CH_G0]  = 7,
		[VPP8890_CH_G1]  = 0,
		[VPP8890_CH_VG0] = 3,
		[VPP8890_CH_VG1] = 4,
	};

	return chmap[ch];
}

static int vpp_wait_idle(struct vpp_context *ctx, unsigned int ch)
{
	u32 val;

	return readl_poll_timeout(ctx->regs[ch] + VPP8890_ENABLE, val,
				  !(val & VPP8890_ENABLE_OP_STATUS), 10,
				  VPP8890_OP_TIMEOUT_US);
}

static int vpp_sw_reset(struct vpp_context *ctx, unsigned int ch)
{
	u32 val;

	vpp_update(ctx, ch, VPP8890_ENABLE, ~0, VPP8890_ENABLE_SRESET);

	return readl_poll_timeout(ctx->regs[ch] + VPP8890_ENABLE, val,
				  !(val & VPP8890_ENABLE_SRESET), 10,
				  VPP8890_RESET_TIMEOUT_US);
}

static void vpp_wait_pingpong(struct vpp_context *ctx, unsigned int ch)
{
	u32 val;

	if (readl_poll_timeout(ctx->regs[ch] + VPP8890_PINGPONG_UPDATE, val,
			       !(val & VPP8890_PINGPONG_REQUEST), 10,
			       VPP8890_PINGPONG_TIMEOUT_US))
		dev_warn(ctx->dev, "vpp%u pingpong update did not clear\n", ch);
}

static void vpp_hw_init(struct vpp_context *ctx, unsigned int ch)
{
	if (vpp_sw_reset(ctx, ch))
		dev_warn(ctx->dev, "vpp%u soft reset timed out\n", ch);

	vpp_update(ctx, ch, VPP8890_ENABLE, ~0, VPP8890_ENABLE_RT_PATH);

	/* all sources unmasked, then the master enable */
	vpp_update(ctx, ch, VPP8890_IRQ, 0,
		   VPP8890_IRQ_FRAMEDONE_MASK | VPP8890_IRQ_DEADLOCK_MASK |
		   VPP8890_IRQ_READ_ERROR_MASK | VPP8890_IRQ_RESET_DONE_MASK);
	vpp_update(ctx, ch, VPP8890_IRQ, ~0, VPP8890_IRQ_ENABLE);

	vpp_write(ctx, ch, VPP8890_QOS_LUT07_00, VPP8890_QOS_LUT_DEFAULT);
	vpp_write(ctx, ch, VPP8890_QOS_LUT15_08, VPP8890_QOS_LUT_DEFAULT);
	vpp_write(ctx, ch, VPP8890_DYNAMIC_GATING_ENABLE, VPP8890_DYNAMIC_GATING_ALL);
	vpp_write(ctx, ch, VPP8890_DEADLOCK_NUM, 0xffffffff);
	vpp_update(ctx, ch, VPP8890_OUT_CON, VPP8890_OUT_CON_FRAME_ALPHA(0xff),
		   VPP8890_OUT_CON_FRAME_ALPHA_MASK);
}

static void vpp_hw_deinit(struct vpp_context *ctx, unsigned int ch)
{
	vpp_write(ctx, ch, VPP8890_IRQ,
		  vpp_read(ctx, ch, VPP8890_IRQ) & VPP8890_IRQ_STATUS_MASK);
	vpp_update(ctx, ch, VPP8890_IRQ, ~0, VPP8890_IRQ_ALL_MASKED);
	if (vpp_wait_idle(ctx, ch))
		dev_warn(ctx->dev, "vpp%u still busy at shutdown\n", ch);
	vpp_sw_reset(ctx, ch);
}

static int vpp_format(u32 fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_ARGB8888:
		return VPP8890_IN_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return VPP8890_IN_FORMAT_XRGB8888;
	case DRM_FORMAT_ABGR8888:
		return VPP8890_IN_FORMAT_ABGR8888;
	case DRM_FORMAT_XBGR8888:
		return VPP8890_IN_FORMAT_XBGR8888;
	case DRM_FORMAT_RGBA8888:
		return VPP8890_IN_FORMAT_RGBA8888;
	case DRM_FORMAT_RGBX8888:
		return VPP8890_IN_FORMAT_RGBX8888;
	case DRM_FORMAT_BGRA8888:
		return VPP8890_IN_FORMAT_BGRA8888;
	case DRM_FORMAT_BGRX8888:
		return VPP8890_IN_FORMAT_BGRX8888;
	case DRM_FORMAT_RGB565:
		return VPP8890_IN_FORMAT_RGB565;
	default:
		return -EINVAL;
	}
}

int exynos8890_vpp_setup(struct device *dev, unsigned int ch,
			 const struct exynos8890_vpp_cfg *cfg)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);
	int fmt;

	if (WARN_ON(ch >= VPP8890_CH_NR))
		return -EINVAL;

	fmt = vpp_format(cfg->fourcc);
	if (fmt < 0)
		return fmt;

	vpp_wait_pingpong(ctx, ch);

	vpp_update(ctx, ch, VPP8890_IN_CON, fmt,
		   VPP8890_IN_FORMAT_MASK | VPP8890_IN_CHROMA_STRIDE |
		   VPP8890_IN_AFBC | VPP8890_IN_ROTATION_MASK |
		   VPP8890_IN_BLOCKING);

	vpp_write(ctx, ch, VPP8890_SRC_OFFSET, VPP8890_SIZE(cfg->src_x, cfg->src_y));
	vpp_write(ctx, ch, VPP8890_SRC_SIZE, VPP8890_SIZE(cfg->fb_w, cfg->fb_h));
	vpp_write(ctx, ch, VPP8890_IMG_SIZE, VPP8890_SIZE(cfg->src_w, cfg->src_h));

	/* G0/G1/VG0/VG1 have no scaler - the output is always 1:1 */
	vpp_write(ctx, ch, VPP8890_SCALED_SIZE, VPP8890_SIZE(cfg->src_w, cfg->src_h));
	vpp_write(ctx, ch, VPP8890_YHPOSITION0, 0);
	vpp_write(ctx, ch, VPP8890_YVPOSITION0, 0);
	vpp_write(ctx, ch, VPP8890_CHPOSITION0, 0);
	vpp_write(ctx, ch, VPP8890_CVPOSITION0, 0);

	vpp_write(ctx, ch, VPP8890_SMART_IF_PIXEL_NUM, cfg->src_w * cfg->src_h);

	vpp_write(ctx, ch, VPP8890_BASE_ADDR_Y(0), lower_32_bits(cfg->dma_addr));
	vpp_write(ctx, ch, VPP8890_BASE_ADDR_CB(0), 0);
	vpp_write(ctx, ch, VPP8890_PINGPONG_UPDATE, VPP8890_PINGPONG_REQUEST);

	return 0;
}

void exynos8890_vpp_disable(struct device *dev, unsigned int ch)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);

	if (WARN_ON(ch >= VPP8890_CH_NR))
		return;

	if (vpp_wait_idle(ctx, ch))
		dev_warn(ctx->dev, "vpp%u did not go idle\n", ch);
	vpp_sw_reset(ctx, ch);
}

/*
 * The Content Firewall keeps every IDMA in the protected state after reset.
 * One SMC per pair unprotects it - G0 covers VPP0/VPP2, G1 covers VPP1/VPP3.
 * Without this every read from a non-secure buffer faults.
 */
static int vpp_cfw_unprotect(struct vpp_context *ctx)
{
	static const u32 prot[] = { CFW_PROT_G0, CFW_PROT_G1 };
	struct arm_smccc_res res;
	int i;

	for (i = 0; i < ARRAY_SIZE(prot); i++) {
		arm_smccc_smc(MC_FC_SET_CFW_PROT, MC_FC_DRM_SET_CFW_PROT,
			      prot[i], 0, 0, 0, 0, 0, &res);
		if (res.a0 != CFW_SMC_TZPC_OK) {
			dev_err(ctx->dev, "CFW unprotect failed for prot %u: %ld\n",
				prot[i], res.a0);
			return -EIO;
		}
	}

	return 0;
}

int exynos8890_vpp_resume(struct device *dev)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);
	unsigned int ch;
	int ret;

	if (ctx->enabled)
		return 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ctx->aclk[0]);
	if (ret)
		goto err_pm;
	ret = clk_prepare_enable(ctx->aclk[1]);
	if (ret)
		goto err_clk0;

	ret = vpp_cfw_unprotect(ctx);
	if (ret)
		goto err_clk1;

	for (ch = 0; ch < VPP8890_CH_NR; ch++)
		vpp_hw_init(ctx, ch);

	ctx->enabled = true;
	return 0;

err_clk1:
	clk_disable_unprepare(ctx->aclk[1]);
err_clk0:
	clk_disable_unprepare(ctx->aclk[0]);
err_pm:
	pm_runtime_put(dev);
	return ret;
}

void exynos8890_vpp_suspend(struct device *dev)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);
	unsigned int ch;

	if (!ctx->enabled)
		return;

	for (ch = 0; ch < VPP8890_CH_NR; ch++)
		vpp_hw_deinit(ctx, ch);

	clk_disable_unprepare(ctx->aclk[1]);
	clk_disable_unprepare(ctx->aclk[0]);
	pm_runtime_put(dev);
	ctx->enabled = false;
}

struct device *exynos8890_vpp_get(struct device_node *np)
{
	struct platform_device *pdev = of_find_device_by_node(np);

	if (!pdev)
		return NULL;
	if (!platform_get_drvdata(pdev)) {
		put_device(&pdev->dev);
		return NULL;
	}

	return &pdev->dev;
}

static int vpp_bind(struct device *dev, struct device *master, void *data)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);

	ctx->drm_dev = data;

	return exynos_drm_register_dma(ctx->drm_dev, dev, &ctx->dma_priv);
}

static void vpp_unbind(struct device *dev, struct device *master, void *data)
{
	struct vpp_context *ctx = dev_get_drvdata(dev);

	exynos8890_vpp_suspend(dev);
	exynos_drm_unregister_dma(ctx->drm_dev, dev, &ctx->dma_priv);
}

static const struct component_ops vpp_component_ops = {
	.bind	= vpp_bind,
	.unbind	= vpp_unbind,
};

static int vpp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vpp_context *ctx;
	unsigned int ch;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;

	for (ch = 0; ch < VPP8890_CH_NR; ch++) {
		ctx->regs[ch] = devm_platform_ioremap_resource(pdev, ch);
		if (IS_ERR(ctx->regs[ch]))
			return PTR_ERR(ctx->regs[ch]);
	}

	ctx->aclk[0] = devm_clk_get(dev, "aclk_vpp0");
	if (IS_ERR(ctx->aclk[0]))
		return dev_err_probe(dev, PTR_ERR(ctx->aclk[0]),
				     "failed to get aclk_vpp0\n");
	ctx->aclk[1] = devm_clk_get(dev, "aclk_vpp1");
	if (IS_ERR(ctx->aclk[1]))
		return dev_err_probe(dev, PTR_ERR(ctx->aclk[1]),
				     "failed to get aclk_vpp1\n");

	platform_set_drvdata(pdev, ctx);
	pm_runtime_enable(dev);

	return component_add(dev, &vpp_component_ops);
}

static void vpp_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vpp_component_ops);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id vpp_of_match[] = {
	{ .compatible = "samsung,exynos8890-vpp" },
	{ },
};
MODULE_DEVICE_TABLE(of, vpp_of_match);

struct platform_driver exynos8890_vpp_driver = {
	.probe	= vpp_probe,
	.remove	= vpp_remove,
	.driver	= {
		.name		= "exynos8890-vpp",
		.of_match_table	= vpp_of_match,
	},
};
