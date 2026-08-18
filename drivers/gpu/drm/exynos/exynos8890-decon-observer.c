// SPDX-License-Identifier: GPL-2.0-only
/*
 * Non-destructive Exynos8890 DECON state observer.
 *
 * This driver intentionally performs MMIO reads only. It does not register a
 * DRM device or take ownership of clocks, power domains, interrupts, IOMMUs,
 * VPPs, DSIM, PHY or panel state.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "regs-decon8890.h"

#define DECON8890_WINDOWS 8

struct decon8890_snapshot {
	u32 global_config;
	u32 interrupt_enable;
	u32 trigger_control;
	u32 display_size;
	u32 win_control[DECON8890_WINDOWS];
	u32 win_start[DECON8890_WINDOWS];
	u32 win_end[DECON8890_WINDOWS];
	u32 win_pixels[DECON8890_WINDOWS];
	u32 data_path;
	u32 frame_id;
};

struct decon8890_observer {
	struct device *dev;
	void __iomem *regs;
	struct decon8890_snapshot first;
	struct delayed_work resample_work;
};

static void decon8890_take_snapshot(struct decon8890_observer *observer,
				   struct decon8890_snapshot *snapshot)
{
	u32 global;
	unsigned int i;

	global = readl_relaxed(observer->regs + DECON8890_GLOBAL_CONTROL);
	snapshot->global_config = global & ~(DECON8890_GLOBAL_IDLE |
		DECON8890_GLOBAL_RUN | DECON8890_GLOBAL_URGENT);
	snapshot->interrupt_enable = readl_relaxed(observer->regs +
						   DECON8890_INTERRUPT_ENABLE);
	snapshot->trigger_control = readl_relaxed(observer->regs +
						  DECON8890_TRIGGER_CONTROL);
	snapshot->display_size = readl_relaxed(observer->regs +
					       DECON8890_DISPIF_SIZE);

	for (i = 0; i < DECON8890_WINDOWS; i++) {
		snapshot->win_control[i] = readl_relaxed(observer->regs +
							 DECON8890_WIN_CONTROL(i));
		snapshot->win_start[i] = readl_relaxed(observer->regs +
						       DECON8890_WIN_START(i));
		snapshot->win_end[i] = readl_relaxed(observer->regs +
						     DECON8890_WIN_END(i));
		snapshot->win_pixels[i] = readl_relaxed(observer->regs +
							DECON8890_WIN_PIXEL_COUNT(i));
	}

	snapshot->data_path = readl_relaxed(observer->regs +
					    DECON8890_DATA_PATH_CONTROL);
	snapshot->frame_id = readl_relaxed(observer->regs + DECON8890_FRAME_ID);
}

static void decon8890_report_initial(struct decon8890_observer *observer)
{
	u32 global, int_pending, shadow;
	unsigned int i;

	global = readl_relaxed(observer->regs + DECON8890_GLOBAL_CONTROL);
	int_pending = readl_relaxed(observer->regs + DECON8890_INTERRUPT_PENDING);
	shadow = readl_relaxed(observer->regs + DECON8890_SHADOW_UPDATE);

	dev_info(observer->dev,
		 "global=%#08x enabled=%u/%u run=%u idle=%u size=%ux%u\n",
		 global, !!(global & DECON8890_GLOBAL_ENABLE),
		 !!(global & DECON8890_GLOBAL_ENABLE_F),
		 !!(global & DECON8890_GLOBAL_RUN),
		 !!(global & DECON8890_GLOBAL_IDLE),
		 observer->first.display_size & 0x3fff,
		 (observer->first.display_size >> 16) & 0x3fff);
	dev_info(observer->dev,
		 "irq-enable=%#08x irq-pending=%#08x shadow=%#08x trigger=%#08x path=%#08x frame=%#08x\n",
		 observer->first.interrupt_enable, int_pending, shadow,
		 observer->first.trigger_control, observer->first.data_path,
		 observer->first.frame_id);

	for (i = 0; i < DECON8890_WINDOWS; i++) {
		u32 control = observer->first.win_control[i];
		dev_info(observer->dev,
			 "win%u control=%#08x enabled=%u channel=%u start=%#08x end=%#08x pixels=%u\n",
			 i, control, !!(control & DECON8890_WIN_ENABLE),
			 DECON8890_WIN_CHANNEL(control), observer->first.win_start[i],
			 observer->first.win_end[i], observer->first.win_pixels[i]);
	}
}

static void decon8890_resample(struct work_struct *work)
{
	struct decon8890_observer *observer = container_of(to_delayed_work(work),
					struct decon8890_observer, resample_work);
	struct decon8890_snapshot second;
	bool changed = false;
	unsigned int i;

	decon8890_take_snapshot(observer, &second);
	changed |= second.global_config != observer->first.global_config;
	changed |= second.interrupt_enable != observer->first.interrupt_enable;
	changed |= second.trigger_control != observer->first.trigger_control;
	changed |= second.display_size != observer->first.display_size;
	for (i = 0; i < DECON8890_WINDOWS; i++) {
		changed |= second.win_control[i] != observer->first.win_control[i];
		changed |= second.win_start[i] != observer->first.win_start[i];
		changed |= second.win_end[i] != observer->first.win_end[i];
		changed |= second.win_pixels[i] != observer->first.win_pixels[i];
	}
	changed |= second.data_path != observer->first.data_path;

	dev_info(observer->dev,
		 "frame-id %#08x -> %#08x (%s), configuration %s\n",
		 observer->first.frame_id, second.frame_id,
		 observer->first.frame_id == second.frame_id ? "unchanged" : "advanced",
		 changed ? "changed" : "preserved");
}

static int decon8890_observer_probe(struct platform_device *pdev)
{
	struct decon8890_observer *observer;

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;

	observer->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(observer->regs))
		return PTR_ERR(observer->regs);

	observer->dev = &pdev->dev;
	platform_set_drvdata(pdev, observer);
	decon8890_take_snapshot(observer, &observer->first);
	decon8890_report_initial(observer);

	INIT_DELAYED_WORK(&observer->resample_work, decon8890_resample);
	schedule_delayed_work(&observer->resample_work, msecs_to_jiffies(100));

	return 0;
}

static void decon8890_observer_remove(struct platform_device *pdev)
{
	struct decon8890_observer *observer = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&observer->resample_work);
}

static const struct of_device_id decon8890_observer_of_match[] = {
	{ .compatible = "samsung,exynos8890-decon" },
	{ }
};
MODULE_DEVICE_TABLE(of, decon8890_observer_of_match);

static struct platform_driver decon8890_observer_driver = {
	.probe = decon8890_observer_probe,
	.remove = decon8890_observer_remove,
	.driver = {
		.name = "exynos8890-decon-observer",
		.of_match_table = decon8890_observer_of_match,
	},
};
module_platform_driver(decon8890_observer_driver);

MODULE_DESCRIPTION("Read-only Exynos8890 DECON state observer");
MODULE_LICENSE("GPL");
