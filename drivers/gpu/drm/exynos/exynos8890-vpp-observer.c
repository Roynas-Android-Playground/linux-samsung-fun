// SPDX-License-Identifier: GPL-2.0-only
/* Non-destructive observer for the inherited Exynos8890 IDMA_G1/VPP1 path. */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "regs-vpp8890.h"

struct vpp8890_snapshot {
	u32 enable;
	u32 irq;
	u32 in_con;
	u32 out_con;
	u32 src_size;
	u32 src_offset;
	u32 img_size;
	u32 pingpong;
	u32 chroma_stride;
	u32 block_offset;
	u32 block_size;
	u32 scaled_size;
	u32 h_ratio;
	u32 v_ratio;
	u32 qos0;
	u32 qos1;
	u32 base_con;
	u32 base_y;
	u32 base_cb;
	u32 deadlock;
	u32 smart_pixels;
	u32 dynamic_gating;
	u32 shadow_base_y;
	u32 shadow_base_cb;
};

struct vpp8890_observer {
	struct device *dev;
	void __iomem *regs;
	struct vpp8890_snapshot first;
	struct delayed_work resample_work;
};

#define VPP8890_SAMPLE(member, reg) \
	(snapshot)->member = readl_relaxed(observer->regs + (reg))

static void vpp8890_take_snapshot(struct vpp8890_observer *observer,
				  struct vpp8890_snapshot *snapshot)
{
	VPP8890_SAMPLE(enable, VPP8890_ENABLE);
	VPP8890_SAMPLE(irq, VPP8890_IRQ);
	VPP8890_SAMPLE(in_con, VPP8890_IN_CON);
	VPP8890_SAMPLE(out_con, VPP8890_OUT_CON);
	VPP8890_SAMPLE(src_size, VPP8890_SRC_SIZE);
	VPP8890_SAMPLE(src_offset, VPP8890_SRC_OFFSET);
	VPP8890_SAMPLE(img_size, VPP8890_IMG_SIZE);
	VPP8890_SAMPLE(pingpong, VPP8890_PINGPONG_UPDATE);
	VPP8890_SAMPLE(chroma_stride, VPP8890_CHROMINANCE_STRIDE);
	VPP8890_SAMPLE(block_offset, VPP8890_BLOCK_OFFSET);
	VPP8890_SAMPLE(block_size, VPP8890_BLOCK_SIZE);
	VPP8890_SAMPLE(scaled_size, VPP8890_SCALED_SIZE);
	VPP8890_SAMPLE(h_ratio, VPP8890_H_RATIO);
	VPP8890_SAMPLE(v_ratio, VPP8890_V_RATIO);
	VPP8890_SAMPLE(qos0, VPP8890_QOS_LUT07_00);
	VPP8890_SAMPLE(qos1, VPP8890_QOS_LUT15_08);
	VPP8890_SAMPLE(base_con, VPP8890_BASE_ADDR_CON);
	VPP8890_SAMPLE(base_y, VPP8890_BASE_ADDR_Y0);
	VPP8890_SAMPLE(base_cb, VPP8890_BASE_ADDR_CB0);
	VPP8890_SAMPLE(deadlock, VPP8890_DEADLOCK_NUM);
	VPP8890_SAMPLE(smart_pixels, VPP8890_SMART_IF_PIXEL_NUM);
	VPP8890_SAMPLE(dynamic_gating, VPP8890_DYNAMIC_GATING_ENABLE);
	VPP8890_SAMPLE(shadow_base_y, VPP8890_SHADOW_BASE_ADDR_Y);
	VPP8890_SAMPLE(shadow_base_cb, VPP8890_SHADOW_BASE_ADDR_CB);
}

static void vpp8890_report_initial(struct vpp8890_observer *observer)
{
	const struct vpp8890_snapshot *s = &observer->first;

	dev_info(observer->dev,
		 "enable=%#08x operating=%u rt=%u irq=%#08x in=%#08x out=%#08x\n",
		 s->enable, !!(s->enable & VPP8890_ENABLE_OP_STATUS),
		 !!(s->enable & VPP8890_ENABLE_RT_PATH), s->irq,
		 s->in_con, s->out_con);
	dev_info(observer->dev,
		 "format=%u alpha=%u blocking=%u chroma-stride-en=%u scan=%u rotation=%u afbc=%u burst=%u\n",
		 VPP8890_IN_FORMAT(s->in_con), s->out_con >> 24,
		 !!(s->in_con & VPP8890_IN_BLOCKING),
		 !!(s->in_con & VPP8890_IN_CHROMA_STRIDE),
		 VPP8890_IN_SCAN_MODE(s->in_con),
		 VPP8890_IN_ROTATION(s->in_con),
		 !!(s->in_con & VPP8890_IN_AFBC),
		 VPP8890_IN_BURST(s->in_con));
	dev_info(observer->dev,
		 "source=%ux%u offset=%u,%u image=%ux%u scaled=%ux%u\n",
		 VPP8890_WIDTH(s->src_size), VPP8890_HEIGHT(s->src_size),
		 VPP8890_WIDTH(s->src_offset), VPP8890_HEIGHT(s->src_offset),
		 VPP8890_WIDTH(s->img_size), VPP8890_HEIGHT(s->img_size),
		 VPP8890_WIDTH(s->scaled_size), VPP8890_HEIGHT(s->scaled_size));
	dev_info(observer->dev,
		 "chroma-stride=%#08x block=%#08x/%#08x ratio=%#08x/%#08x\n",
		 s->chroma_stride, s->block_offset, s->block_size,
		 s->h_ratio, s->v_ratio);
	dev_info(observer->dev,
		 "base-y/cb=%#08x/%#08x shadow-y/cb=%#08x/%#08x base-control=%#08x pingpong=%#08x smart-pixels=%u\n",
		 s->base_y, s->base_cb, s->shadow_base_y, s->shadow_base_cb,
		 s->base_con, s->pingpong, s->smart_pixels);
	dev_info(observer->dev,
		 "qos=%#08x/%#08x deadlock=%#08x dynamic-gating=%#08x\n",
		 s->qos0, s->qos1, s->deadlock, s->dynamic_gating);
}

static void vpp8890_resample(struct work_struct *work)
{
	struct vpp8890_observer *observer = container_of(to_delayed_work(work),
					struct vpp8890_observer, resample_work);
	struct vpp8890_snapshot second;
	const struct vpp8890_snapshot *first = &observer->first;
	bool config_changed;
	u32 volatile_changed;

	vpp8890_take_snapshot(observer, &second);
	config_changed = ((first->enable ^ second.enable) &
			  VPP8890_ENABLE_CONFIG_MASK) ||
		((first->irq ^ second.irq) & VPP8890_IRQ_CONFIG_MASK) ||
		((first->base_con ^ second.base_con) &
		 VPP8890_BASE_CON_CONFIG_MASK) ||
		first->in_con != second.in_con ||
		first->out_con != second.out_con ||
		first->src_size != second.src_size ||
		first->src_offset != second.src_offset ||
		first->img_size != second.img_size ||
		first->chroma_stride != second.chroma_stride ||
		first->block_offset != second.block_offset ||
		first->block_size != second.block_size ||
		first->scaled_size != second.scaled_size ||
		first->h_ratio != second.h_ratio ||
		first->v_ratio != second.v_ratio ||
		first->qos0 != second.qos0 || first->qos1 != second.qos1 ||
		first->deadlock != second.deadlock ||
		first->smart_pixels != second.smart_pixels ||
		first->dynamic_gating != second.dynamic_gating;
	volatile_changed = (!!((first->enable ^ second.enable) &
				      ~VPP8890_ENABLE_CONFIG_MASK)) +
		(!!((first->irq ^ second.irq) & ~VPP8890_IRQ_CONFIG_MASK)) +
		(first->pingpong != second.pingpong) +
		(!!((first->base_con ^ second.base_con) &
		    ~VPP8890_BASE_CON_CONFIG_MASK)) +
		(first->base_y != second.base_y) +
		(first->base_cb != second.base_cb) +
		(first->shadow_base_y != second.shadow_base_y) +
		(first->shadow_base_cb != second.shadow_base_cb);

	dev_info(observer->dev,
		 "resample configuration %s, volatile-fields-changed=%u base %#08x->%#08x shadow %#08x->%#08x; observer performed no writes\n",
		 config_changed ? "changed" : "preserved", volatile_changed,
		 first->base_y, second.base_y, first->shadow_base_y,
		 second.shadow_base_y);
}

static int vpp8890_observer_probe(struct platform_device *pdev)
{
	struct vpp8890_observer *observer;

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;

	observer->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(observer->regs))
		return PTR_ERR(observer->regs);

	observer->dev = &pdev->dev;
	platform_set_drvdata(pdev, observer);
	vpp8890_take_snapshot(observer, &observer->first);
	vpp8890_report_initial(observer);

	INIT_DELAYED_WORK(&observer->resample_work, vpp8890_resample);
	schedule_delayed_work(&observer->resample_work, msecs_to_jiffies(100));
	return 0;
}

static void vpp8890_observer_remove(struct platform_device *pdev)
{
	struct vpp8890_observer *observer = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&observer->resample_work);
}

static const struct of_device_id vpp8890_observer_of_match[] = {
	{ .compatible = "samsung,exynos8890-vpp-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, vpp8890_observer_of_match);

static struct platform_driver vpp8890_observer_driver = {
	.probe = vpp8890_observer_probe,
	.remove = vpp8890_observer_remove,
	.driver = {
		.name = "exynos8890-vpp-observer",
		.of_match_table = vpp8890_observer_of_match,
	},
};
module_platform_driver(vpp8890_observer_driver);

MODULE_DESCRIPTION("Read-only Exynos8890 IDMA_G1/VPP observer");
MODULE_LICENSE("GPL");
