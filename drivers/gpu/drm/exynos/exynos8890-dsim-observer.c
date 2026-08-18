// SPDX-License-Identifier: GPL-2.0-only
/* Non-destructive Exynos8890 DSIM state observer. */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "regs-dsim8890.h"

struct dsim8890_snapshot {
	u32 link_status;
	u32 dphy_status;
	u32 swrst;
	u32 clkctrl;
	u32 timeout0;
	u32 timeout1;
	u32 escmode;
	u32 resol;
	u32 vporch;
	u32 hporch;
	u32 sync;
	u32 config;
	u32 intsrc;
	u32 intmsk;
	u32 sfr_ctrl;
	u32 fifoctrl;
	u32 cprs_ctrl;
	u32 slice01;
	u32 slice23;
	u32 cmd_config;
	u32 te_ctrl0;
	u32 te_ctrl1;
	u32 pllctrl;
	u32 pll_ctrl1;
	u32 pll_ctrl2;
	u32 plltmr;
	u32 phyctrl;
	u32 phytiming;
	u32 phytiming1;
	u32 phytiming2;
};

struct dsim8890_observer {
	struct device *dev;
	void __iomem *regs;
	struct dsim8890_snapshot first;
	struct delayed_work resample_work;
};

#define DSIM8890_SAMPLE(member, reg) \
	(snapshot)->member = readl_relaxed(observer->regs + (reg))

static void dsim8890_take_snapshot(struct dsim8890_observer *observer,
				   struct dsim8890_snapshot *snapshot)
{
	DSIM8890_SAMPLE(link_status, DSIM8890_LINK_STATUS);
	DSIM8890_SAMPLE(dphy_status, DSIM8890_DPHY_STATUS);
	DSIM8890_SAMPLE(swrst, DSIM8890_SWRST);
	DSIM8890_SAMPLE(clkctrl, DSIM8890_CLKCTRL);
	DSIM8890_SAMPLE(timeout0, DSIM8890_TIMEOUT0);
	DSIM8890_SAMPLE(timeout1, DSIM8890_TIMEOUT1);
	DSIM8890_SAMPLE(escmode, DSIM8890_ESCMODE);
	DSIM8890_SAMPLE(resol, DSIM8890_RESOL);
	DSIM8890_SAMPLE(vporch, DSIM8890_VPORCH);
	DSIM8890_SAMPLE(hporch, DSIM8890_HPORCH);
	DSIM8890_SAMPLE(sync, DSIM8890_SYNC);
	DSIM8890_SAMPLE(config, DSIM8890_CONFIG);
	DSIM8890_SAMPLE(intsrc, DSIM8890_INTSRC);
	DSIM8890_SAMPLE(intmsk, DSIM8890_INTMSK);
	DSIM8890_SAMPLE(sfr_ctrl, DSIM8890_SFR_CTRL);
	DSIM8890_SAMPLE(fifoctrl, DSIM8890_FIFOCTRL);
	DSIM8890_SAMPLE(cprs_ctrl, DSIM8890_CPRS_CTRL);
	DSIM8890_SAMPLE(slice01, DSIM8890_SLICE01);
	DSIM8890_SAMPLE(slice23, DSIM8890_SLICE23);
	DSIM8890_SAMPLE(cmd_config, DSIM8890_CMD_CONFIG);
	DSIM8890_SAMPLE(te_ctrl0, DSIM8890_CMD_TE_CTRL0);
	DSIM8890_SAMPLE(te_ctrl1, DSIM8890_CMD_TE_CTRL1);
	DSIM8890_SAMPLE(pllctrl, DSIM8890_PLLCTRL);
	DSIM8890_SAMPLE(pll_ctrl1, DSIM8890_PLL_CTRL1);
	DSIM8890_SAMPLE(pll_ctrl2, DSIM8890_PLL_CTRL2);
	DSIM8890_SAMPLE(plltmr, DSIM8890_PLLTMR);
	DSIM8890_SAMPLE(phyctrl, DSIM8890_PHYCTRL);
	DSIM8890_SAMPLE(phytiming, DSIM8890_PHYTIMING);
	DSIM8890_SAMPLE(phytiming1, DSIM8890_PHYTIMING1);
	DSIM8890_SAMPLE(phytiming2, DSIM8890_PHYTIMING2);
}

static void dsim8890_report_initial(struct dsim8890_observer *observer)
{
	const struct dsim8890_snapshot *s = &observer->first;
	u32 width = s->resol & 0xfff;
	u32 height = (s->resol >> 16) & 0xfff;
	const char *evidence;

	if (s->config & DSIM8890_CONFIG_CPRS_EN)
		evidence = "DSC-enabled";
	else if (width == 720)
		evidence = "720-payload-consistent-with-herolte-MIC-1/2";
	else if (width == 480)
		evidence = "480-payload-without-DSC-enable";
	else
		evidence = "unclassified";

	dev_info(observer->dev,
		 "link=%#08x pll=%u dphy=%#08x hs-ready=%u clk=%#08x tx-hs=%u\n",
		 s->link_status,
		 !!(s->link_status & DSIM8890_LINK_PLL_STABLE),
		 s->dphy_status,
		 !!(s->dphy_status & DSIM8890_DPHY_TX_READY_HS_CLK),
		 s->clkctrl, !!(s->clkctrl & DSIM8890_CLKCTRL_TX_HS));
	dev_info(observer->dev,
		 "resol=%ux%u evidence=%s config=%#08x cmd=%u multipix=%u cprs=%u qch=%u\n",
		 width, height, evidence, s->config,
		 !(s->config & DSIM8890_CONFIG_VIDEO_MODE),
		 !!(s->config & DSIM8890_CONFIG_MULTI_PIX),
		 !!(s->config & DSIM8890_CONFIG_CPRS_EN),
		 !!(s->config & DSIM8890_CONFIG_Q_CHANNEL));
	dev_info(observer->dev,
		 "porch v=%#08x h=%#08x sync=%#08x timeout=%#08x/%#08x esc=%#08x\n",
		 s->vporch, s->hporch, s->sync, s->timeout0, s->timeout1,
		 s->escmode);
	dev_info(observer->dev,
		 "sfr=%#08x fifo=%#08x irq=%#08x/%#08x cprs=%#08x slices=%#08x/%#08x\n",
		 s->sfr_ctrl, s->fifoctrl, s->intsrc, s->intmsk,
		 s->cprs_ctrl, s->slice01, s->slice23);
	dev_info(observer->dev,
		 "cmdcfg=%#08x te=%#08x/%#08x pll=%#08x analog=%#08x/%#08x timer=%#08x\n",
		 s->cmd_config, s->te_ctrl0, s->te_ctrl1, s->pllctrl,
		 s->pll_ctrl1, s->pll_ctrl2, s->plltmr);
	dev_info(observer->dev,
		 "phy=%#08x timing=%#08x/%#08x/%#08x reset=%#08x\n",
		 s->phyctrl, s->phytiming, s->phytiming1, s->phytiming2,
		 s->swrst);
}

static void dsim8890_resample(struct work_struct *work)
{
	struct dsim8890_observer *observer = container_of(to_delayed_work(work),
					struct dsim8890_observer, resample_work);
	struct dsim8890_snapshot second;
	const struct dsim8890_snapshot *first = &observer->first;
	bool config_changed;
	u32 volatile_changed;

	dsim8890_take_snapshot(observer, &second);
	config_changed = first->swrst != second.swrst ||
		first->clkctrl != second.clkctrl ||
		first->timeout0 != second.timeout0 ||
		first->timeout1 != second.timeout1 ||
		first->escmode != second.escmode ||
		first->resol != second.resol ||
		first->vporch != second.vporch ||
		first->hporch != second.hporch ||
		first->sync != second.sync ||
		first->config != second.config ||
		first->intmsk != second.intmsk ||
		first->sfr_ctrl != second.sfr_ctrl ||
		first->cprs_ctrl != second.cprs_ctrl ||
		first->slice01 != second.slice01 ||
		first->slice23 != second.slice23 ||
		first->cmd_config != second.cmd_config ||
		first->te_ctrl0 != second.te_ctrl0 ||
		first->te_ctrl1 != second.te_ctrl1 ||
		first->pllctrl != second.pllctrl ||
		first->pll_ctrl1 != second.pll_ctrl1 ||
		first->pll_ctrl2 != second.pll_ctrl2 ||
		first->plltmr != second.plltmr ||
		first->phyctrl != second.phyctrl ||
		first->phytiming != second.phytiming ||
		first->phytiming1 != second.phytiming1 ||
		first->phytiming2 != second.phytiming2;
	volatile_changed = (first->link_status != second.link_status) +
		(first->dphy_status != second.dphy_status) +
		(first->intsrc != second.intsrc) +
		(first->fifoctrl != second.fifoctrl);

	dev_info(observer->dev,
		 "resample configuration %s, volatile-fields-changed=%u intsrc %#08x->%#08x fifo %#08x->%#08x; observer performed no writes\n",
		 config_changed ? "changed" : "preserved", volatile_changed,
		 first->intsrc, second.intsrc, first->fifoctrl, second.fifoctrl);
}

static int dsim8890_observer_probe(struct platform_device *pdev)
{
	struct dsim8890_observer *observer;

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;

	observer->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(observer->regs))
		return PTR_ERR(observer->regs);

	observer->dev = &pdev->dev;
	platform_set_drvdata(pdev, observer);
	dsim8890_take_snapshot(observer, &observer->first);
	dsim8890_report_initial(observer);

	INIT_DELAYED_WORK(&observer->resample_work, dsim8890_resample);
	schedule_delayed_work(&observer->resample_work, msecs_to_jiffies(100));

	return 0;
}

static void dsim8890_observer_remove(struct platform_device *pdev)
{
	struct dsim8890_observer *observer = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&observer->resample_work);
}

static const struct of_device_id dsim8890_observer_of_match[] = {
	{ .compatible = "samsung,exynos8890-dsim-observer" },
	{ }
};
MODULE_DEVICE_TABLE(of, dsim8890_observer_of_match);

static struct platform_driver dsim8890_observer_driver = {
	.probe = dsim8890_observer_probe,
	.remove = dsim8890_observer_remove,
	.driver = {
		.name = "exynos8890-dsim-observer",
		.of_match_table = dsim8890_observer_of_match,
	},
};
module_platform_driver(dsim8890_observer_driver);

MODULE_DESCRIPTION("Read-only Exynos8890 DSIM state observer");
MODULE_LICENSE("GPL");
