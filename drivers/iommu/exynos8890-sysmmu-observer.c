// SPDX-License-Identifier: GPL-2.0-only
/* Non-destructive observer for the inherited Exynos8890 System MMU. */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define SYSMMU_CTRL			0x000
#define SYSMMU_CFG			0x004
#define SYSMMU_STATUS			0x008
#define SYSMMU_PT_BASE_PPN		0x00c
#define SYSMMU_CAPA			0x030
#define SYSMMU_VERSION			0x034
#define SYSMMU_CAPA_1			0x038
#define SYSMMU_INT_STATUS		0x060
#define SYSMMU_FAULT_AR_ADDR		0x070
#define SYSMMU_FAULT_AR_TRANS_INFO	0x078
#define SYSMMU_FAULT_AW_ADDR		0x080
#define SYSMMU_FAULT_AW_TRANS_INFO	0x088
#define SYSMMU_FAULT_MASK		0x001f001f

struct exynos8890_sysmmu_snapshot {
	u32 ctrl;
	u32 cfg;
	u32 status;
	u32 pt_base_ppn;
	u32 capa;
	u32 version;
	u32 capa_1;
	u32 int_status;
	u32 fault_ar_addr;
	u32 fault_ar_info;
	u32 fault_aw_addr;
	u32 fault_aw_info;
};

struct exynos8890_sysmmu_observer {
	struct device *dev;
	void __iomem *regs;
	struct exynos8890_sysmmu_snapshot first;
	struct delayed_work resample_work;
};

static u32 exynos8890_sysmmu_read(struct exynos8890_sysmmu_observer *observer,
				 u32 offset)
{
	return readl_relaxed(observer->regs + offset);
}

static void exynos8890_sysmmu_snapshot(
	struct exynos8890_sysmmu_observer *observer,
	struct exynos8890_sysmmu_snapshot *snapshot)
{
	snapshot->ctrl = exynos8890_sysmmu_read(observer, SYSMMU_CTRL);
	snapshot->cfg = exynos8890_sysmmu_read(observer, SYSMMU_CFG);
	snapshot->status = exynos8890_sysmmu_read(observer, SYSMMU_STATUS);
	snapshot->pt_base_ppn =
		exynos8890_sysmmu_read(observer, SYSMMU_PT_BASE_PPN);
	snapshot->capa = exynos8890_sysmmu_read(observer, SYSMMU_CAPA);
	snapshot->version = exynos8890_sysmmu_read(observer, SYSMMU_VERSION);
	snapshot->capa_1 = exynos8890_sysmmu_read(observer, SYSMMU_CAPA_1);
	snapshot->int_status =
		exynos8890_sysmmu_read(observer, SYSMMU_INT_STATUS);
	snapshot->fault_ar_addr =
		exynos8890_sysmmu_read(observer, SYSMMU_FAULT_AR_ADDR);
	snapshot->fault_ar_info =
		exynos8890_sysmmu_read(observer, SYSMMU_FAULT_AR_TRANS_INFO);
	snapshot->fault_aw_addr =
		exynos8890_sysmmu_read(observer, SYSMMU_FAULT_AW_ADDR);
	snapshot->fault_aw_info =
		exynos8890_sysmmu_read(observer, SYSMMU_FAULT_AW_TRANS_INFO);
}

static const char *exynos8890_sysmmu_mode(u32 ctrl)
{
	switch (ctrl) {
	case 0x0:
		return "disabled-physical-address";
	case 0x5:
		return "enabled-iova";
	case 0x3:
		return "block-disabled-transition";
	case 0x7:
		return "block-request-transition";
	default:
		return "unknown-fail-closed";
	}
}

static void exynos8890_sysmmu_report(
	struct exynos8890_sysmmu_observer *observer)
{
	const struct exynos8890_sysmmu_snapshot *s = &observer->first;
	u32 faults = s->int_status & SYSMMU_FAULT_MASK;

	dev_info(observer->dev,
		 "ctrl=%#08x mode=%s cfg=%#08x status=%#08x pt-base=%#010llx\n",
		 s->ctrl, exynos8890_sysmmu_mode(s->ctrl), s->cfg, s->status,
		 (unsigned long long)s->pt_base_ppn << 12);
	dev_info(observer->dev, "version=%#08x capa=%#08x/%#08x\n",
		 s->version, s->capa, s->capa_1);
	dev_info(observer->dev,
		 "faults=%#08x raw-int=%#08x read=%#08x info=%#08x write=%#08x info=%#08x\n",
		 faults, s->int_status, s->fault_ar_addr, s->fault_ar_info,
		 s->fault_aw_addr, s->fault_aw_info);
}

static void exynos8890_sysmmu_resample(struct work_struct *work)
{
	struct exynos8890_sysmmu_observer *observer =
		container_of(to_delayed_work(work),
			     struct exynos8890_sysmmu_observer, resample_work);
	struct exynos8890_sysmmu_snapshot second;
	const struct exynos8890_sysmmu_snapshot *first = &observer->first;
	bool context_changed;
	u32 volatile_changed;

	exynos8890_sysmmu_snapshot(observer, &second);
	context_changed = first->ctrl != second.ctrl ||
		first->cfg != second.cfg ||
		first->pt_base_ppn != second.pt_base_ppn ||
		first->capa != second.capa ||
		first->version != second.version ||
		first->capa_1 != second.capa_1;
	volatile_changed = (first->status != second.status) +
		(first->int_status != second.int_status) +
		(first->fault_ar_addr != second.fault_ar_addr) +
		(first->fault_ar_info != second.fault_ar_info) +
		(first->fault_aw_addr != second.fault_aw_addr) +
		(first->fault_aw_info != second.fault_aw_info);

	dev_info(observer->dev,
		 "resample context %s, volatile-fields-changed=%u ctrl %#08x->%#08x pt-base %#010llx->%#010llx; observer performed no writes\n",
		 context_changed ? "changed-fail-closed" : "preserved",
		 volatile_changed, first->ctrl, second.ctrl,
		 (unsigned long long)first->pt_base_ppn << 12,
		 (unsigned long long)second.pt_base_ppn << 12);
}

static int exynos8890_sysmmu_observer_probe(struct platform_device *pdev)
{
	struct exynos8890_sysmmu_observer *observer;

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;

	observer->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(observer->regs))
		return PTR_ERR(observer->regs);

	observer->dev = &pdev->dev;
	platform_set_drvdata(pdev, observer);
	exynos8890_sysmmu_snapshot(observer, &observer->first);
	exynos8890_sysmmu_report(observer);

	INIT_DELAYED_WORK(&observer->resample_work, exynos8890_sysmmu_resample);
	schedule_delayed_work(&observer->resample_work, msecs_to_jiffies(100));
	return 0;
}

static void exynos8890_sysmmu_observer_remove(struct platform_device *pdev)
{
	struct exynos8890_sysmmu_observer *observer = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&observer->resample_work);
}

static const struct of_device_id exynos8890_sysmmu_observer_of_match[] = {
	{ .compatible = "samsung,exynos8890-sysmmu-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_sysmmu_observer_of_match);

static struct platform_driver exynos8890_sysmmu_observer_driver = {
	.probe = exynos8890_sysmmu_observer_probe,
	.remove = exynos8890_sysmmu_observer_remove,
	.driver = {
		.name = "exynos8890-sysmmu-observer",
		.of_match_table = exynos8890_sysmmu_observer_of_match,
	},
};
module_platform_driver(exynos8890_sysmmu_observer_driver);

MODULE_DESCRIPTION("Read-only Exynos8890 System MMU observer");
MODULE_LICENSE("GPL");
