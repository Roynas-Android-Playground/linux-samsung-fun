// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Samsung Exynos8890
 *
 * The low-level PHY and ELBI programming is derived from Samsung's
 * Exynos8890 downstream kernel. Host bridge setup, iATU programming,
 * MSI domains and PCI enumeration are handled by the DesignWare core.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "pcie-designware.h"

/* ELBI registers */
#define PCIE_IRQ_PULSE 0x000
#define IRQ_INTA_ASSERT BIT(0)
#define IRQ_INTB_ASSERT BIT(2)
#define IRQ_INTC_ASSERT BIT(4)
#define IRQ_INTD_ASSERT BIT(6)
#define PCIE_IRQ_LEVEL 0x004
#define IRQ_MSI_CTRL BIT(1)
#define PCIE_IRQ_SPECIAL 0x008
#define PCIE_IRQ_EN_PULSE 0x00c
#define PCIE_IRQ_EN_LEVEL 0x010
#define PCIE_IRQ_EN_SPECIAL 0x014
#define PCIE_SW_WAKE 0x018
#define PCIE_BUS_EN BIT(1)
#define PCIE_APP_LTSSM_ENABLE 0x02c
#define PCIE_ELBI_LTSSM_DISABLE 0
#define PCIE_ELBI_LTSSM_ENABLE 1
#define PCIE_L1_BUG_FIX_ENABLE 0x038
#define PCIE_APP_REQ_EXIT_L1_MODE 0x0f4
#define APP_REQ_EXIT_L1_MODE BIT(0)
#define L1_REQ_NAK_CONTROL_MASTER BIT(4)
#define PCIE_ELBI_RDLH_LINKUP 0x074
#define PCIE_LINKDOWN_RST_CTRL_SEL 0x1b8
#define PCIE_LINKDOWN_RST_MANUAL BIT(1)
#define PCIE_SOFT_CORE_RESET 0x1d0
#define PCIE_QCH_SEL 0x2c8
#define CLOCK_GATING_MASK GENMASK(1, 0)
#define CLOCK_NOT_GATING GENMASK(1, 0)

/* ELBI reset controls used by the Exynos8890 PCIe PHY sequence */
#define PCIE_PCS_G_RST 0x288
#define PCIE_PHY_PCS_PMA_RST 0x28c
#define PCIE_MAC_CMN_RST 0x290

/* PCS registers */
#define PCIE_PCS_TIMEOUT_L1SS 0x00c
#define PCIE_PCS_RX_ELECIDLE 0x0ec
#define PCIE_PCS_TX_LATENCY 0x0f8
#define PCIE_PCS_RX_ELECIDLE_IGNORE BIT(3)

/* PMU */
#define EXYNOS8890_PMU_PCIE0_PHY_CONTROL 0x071c
#define EXYNOS8890_PMU_PCIE_PHY_ENABLE BIT(0)

#define EXYNOS8890_NUM_CORE_CLKS 7
#define EXYNOS8890_NUM_PHY_CLKS 3

struct exynos8890_pcie {
	struct dw_pcie pci;

	void __iomem *phy_base;
	void __iomem *pcs_base;
	void __iomem *sysreg_base;
	struct regmap *pmu;

	struct clk_bulk_data core_clks[EXYNOS8890_NUM_CORE_CLKS];
	struct clk_bulk_data phy_clks[EXYNOS8890_NUM_PHY_CLKS];
	struct gpio_desc *perst;
	struct regulator *vpcie;
	int irq;

	bool vpcie_enabled;
	bool core_clks_enabled;
	bool phy_clks_enabled;
	bool phy_powered;
	bool irq_accessible;
};

static const char *const exynos8890_core_clk_names[] = {
	"pcie", "dbi", "slv", "mstr", "ahb", "pcs", "phy-pclk",
};

static const char *const exynos8890_phy_clk_names[] = {
	"phy-ref",
	"phy-tx",
	"phy-rx",
};

static inline struct exynos8890_pcie *to_exynos8890_pcie(struct dw_pcie *pci)
{
	return dev_get_drvdata(pci->dev);
}

static inline u32 exynos8890_elbi_readl(struct exynos8890_pcie *ep, u32 reg)
{
	return readl(ep->pci.elbi_base + reg);
}

static inline void exynos8890_elbi_writel(struct exynos8890_pcie *ep, u32 val,
					  u32 reg)
{
	writel(val, ep->pci.elbi_base + reg);
}

static void exynos8890_toggle_reset(struct exynos8890_pcie *ep, u32 reg)
{
	exynos8890_elbi_writel(ep, 1, reg);
	udelay(10);
	exynos8890_elbi_writel(ep, 0, reg);
	udelay(10);
	exynos8890_elbi_writel(ep, 1, reg);
	udelay(10);
}

static void exynos8890_pcie_phy_config(struct exynos8890_pcie *ep)
{
	static const u32 common_cfg[] = {
		0x01, 0x0f, 0xa6, 0x31, 0x90, 0x62, 0x20, 0x00, 0x00,
		0xa7, 0x0a, 0x37, 0x20, 0x08, 0xef, 0xfc, 0x96, 0x14,
		0x00, 0x10, 0x60, 0x01, 0x00, 0x00, 0x04, 0x10,
	};
	static const u32 transceiver_cfg[] = {
		0x31, 0xf4, 0xf4, 0x80, 0x25, 0x40, 0xd8, 0x03, 0x35,
		0x55, 0x4c, 0xc3, 0x10, 0x54, 0x70, 0xc5, 0x00, 0x2f,
		0x38, 0xa4, 0x00, 0x3b, 0x30, 0x9a, 0x64, 0x00, 0x1f,
		0x83, 0x1b, 0x01, 0xe0, 0x00, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x1d, 0x00,
	};
	u32 val;
	int i;

	val = readl(ep->sysreg_base);
	val &= ~BIT(1);
	writel(val, ep->sysreg_base);

	val = readl(ep->sysreg_base + 0x0c);
	val &= ~(GENMASK(7, 2) | BIT(1));
	val |= GENMASK(3, 2);
	writel(val, ep->sysreg_base + 0x0c);

	exynos8890_toggle_reset(ep, PCIE_PCS_G_RST);

	for (i = 0; i < ARRAY_SIZE(common_cfg); i++)
		writel(common_cfg[i], ep->phy_base + i * 4);

	for (i = 0; i < ARRAY_SIZE(transceiver_cfg); i++)
		writel(transceiver_cfg[i], ep->phy_base + (0x30 + i) * 4);

	/* TX amplitude and latency tuning from the downstream PHY sequence. */
	writel(0x14, ep->phy_base + 0x5c * 4);
	writel(0x70, ep->pcs_base + PCIE_PCS_TX_LATENCY);

	val = readl(ep->pcs_base + PCIE_PCS_TIMEOUT_L1SS);
	val |= BIT(4);
	writel(val, ep->pcs_base + PCIE_PCS_TIMEOUT_L1SS);

	exynos8890_toggle_reset(ep, PCIE_MAC_CMN_RST);
	exynos8890_toggle_reset(ep, PCIE_PHY_PCS_PMA_RST);
}

static void exynos8890_phy_powerdown(struct exynos8890_pcie *ep, bool down)
{
	u32 val;

	val = readl(ep->phy_base + 0x15 * 4);
	if (down)
		val |= 0xf << 3;
	else
		val &= ~(0xf << 3);
	writel(val, ep->phy_base + 0x15 * 4);

	if (down) {
		writel(0xff, ep->phy_base + 0x4e * 4);
		writel(0x3f, ep->phy_base + 0x4f * 4);
	} else {
		writel(0, ep->phy_base + 0x4e * 4);
		writel(0, ep->phy_base + 0x4f * 4);
	}
}

static void exynos8890_disable_irqs(struct exynos8890_pcie *ep)
{
	exynos8890_elbi_writel(ep, 0, PCIE_IRQ_EN_PULSE);
	exynos8890_elbi_writel(ep, 0, PCIE_IRQ_EN_LEVEL);
	exynos8890_elbi_writel(ep, 0, PCIE_IRQ_EN_SPECIAL);
}

static void exynos8890_clear_irqs(struct exynos8890_pcie *ep)
{
	u32 val;

	val = exynos8890_elbi_readl(ep, PCIE_IRQ_PULSE);
	exynos8890_elbi_writel(ep, val, PCIE_IRQ_PULSE);
	val = exynos8890_elbi_readl(ep, PCIE_IRQ_LEVEL);
	exynos8890_elbi_writel(ep, val, PCIE_IRQ_LEVEL);
	val = exynos8890_elbi_readl(ep, PCIE_IRQ_SPECIAL);
	exynos8890_elbi_writel(ep, val, PCIE_IRQ_SPECIAL);
}

static void exynos8890_enable_irqs(struct exynos8890_pcie *ep)
{
	u32 pulse = IRQ_INTA_ASSERT | IRQ_INTB_ASSERT | IRQ_INTC_ASSERT |
		    IRQ_INTD_ASSERT;

	exynos8890_clear_irqs(ep);
	exynos8890_elbi_writel(ep, pulse, PCIE_IRQ_EN_PULSE);
	exynos8890_elbi_writel(ep, IRQ_MSI_CTRL, PCIE_IRQ_EN_LEVEL);
	exynos8890_elbi_writel(ep, 0, PCIE_IRQ_EN_SPECIAL);
}

static irqreturn_t exynos8890_pcie_irq_handler(int irq, void *arg)
{
	struct exynos8890_pcie *ep = arg;
	u32 level, pulse, special;

	if (!READ_ONCE(ep->irq_accessible))
		return IRQ_NONE;

	pulse = exynos8890_elbi_readl(ep, PCIE_IRQ_PULSE);
	level = exynos8890_elbi_readl(ep, PCIE_IRQ_LEVEL);
	special = exynos8890_elbi_readl(ep, PCIE_IRQ_SPECIAL);

	if (!pulse && !level && !special)
		return IRQ_NONE;

	if (pulse)
		exynos8890_elbi_writel(ep, pulse, PCIE_IRQ_PULSE);

	if (level & IRQ_MSI_CTRL)
		dw_handle_msi_irq(&ep->pci.pp);

	if (level)
		exynos8890_elbi_writel(ep, level, PCIE_IRQ_LEVEL);
	if (special)
		exynos8890_elbi_writel(ep, special, PCIE_IRQ_SPECIAL);

	return IRQ_HANDLED;
}

static enum dw_pcie_ltssm exynos8890_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);

	return exynos8890_elbi_readl(ep, PCIE_ELBI_RDLH_LINKUP) & 0x1f;
}

static bool exynos8890_pcie_link_up(struct dw_pcie *pci)
{
	enum dw_pcie_ltssm ltssm = exynos8890_pcie_get_ltssm(pci);

	return ltssm >= DW_PCIE_LTSSM_L0 && ltssm <= DW_PCIE_LTSSM_L1_IDLE;
}

static int exynos8890_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);
	u32 val;

	/*
	 * Downstream keeps RX-elecidle checking disabled while programming DBI
	 * and restores it immediately after dw_pcie_setup_rc().  The generic
	 * DWC host calls .start_link() at exactly that point.
	 */
	val = readl(ep->pcs_base + PCIE_PCS_RX_ELECIDLE);
	val &= ~PCIE_PCS_RX_ELECIDLE_IGNORE;
	writel(val, ep->pcs_base + PCIE_PCS_RX_ELECIDLE);

	exynos8890_elbi_writel(ep, PCIE_ELBI_LTSSM_ENABLE,
			       PCIE_APP_LTSSM_ENABLE);

	return 0;
}

static void exynos8890_pcie_stop_link(struct dw_pcie *pci)
{
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);

	if (!ep->core_clks_enabled)
		return;

	exynos8890_elbi_writel(ep, PCIE_ELBI_LTSSM_DISABLE,
			       PCIE_APP_LTSSM_ENABLE);
	gpiod_set_value_cansleep(ep->perst, 1);
}

static void exynos8890_pcie_quiesce_irqs(struct exynos8890_pcie *ep)
{
	if (!ep->core_clks_enabled || !READ_ONCE(ep->irq_accessible))
		return;

	exynos8890_disable_irqs(ep);
	WRITE_ONCE(ep->irq_accessible, false);
	synchronize_irq(ep->irq);
}

static void exynos8890_pcie_power_off(struct exynos8890_pcie *ep)
{
	exynos8890_pcie_quiesce_irqs(ep);

	if (ep->core_clks_enabled)
		exynos8890_pcie_stop_link(&ep->pci);

	if (ep->phy_powered && ep->core_clks_enabled)
		exynos8890_phy_powerdown(ep, true);

	if (ep->phy_clks_enabled) {
		clk_bulk_disable_unprepare(EXYNOS8890_NUM_PHY_CLKS,
					   ep->phy_clks);
		ep->phy_clks_enabled = false;
	}

	if (ep->phy_powered) {
		regmap_update_bits(ep->pmu, EXYNOS8890_PMU_PCIE0_PHY_CONTROL,
				   EXYNOS8890_PMU_PCIE_PHY_ENABLE, 0);
		ep->phy_powered = false;
	}

	if (ep->core_clks_enabled) {
		clk_bulk_disable_unprepare(EXYNOS8890_NUM_CORE_CLKS,
					   ep->core_clks);
		ep->core_clks_enabled = false;
	}

	if (ep->vpcie_enabled) {
		regulator_disable(ep->vpcie);
		ep->vpcie_enabled = false;
	}
}

static int exynos8890_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);
	u32 val;
	int ret;

	if (ep->vpcie) {
		ret = regulator_enable(ep->vpcie);
		if (ret)
			return ret;
		ep->vpcie_enabled = true;
	}

	ret = clk_bulk_prepare_enable(EXYNOS8890_NUM_CORE_CLKS, ep->core_clks);
	if (ret)
		goto err_power_off;
	ep->core_clks_enabled = true;

	ret = regmap_update_bits(ep->pmu, EXYNOS8890_PMU_PCIE0_PHY_CONTROL,
				 EXYNOS8890_PMU_PCIE_PHY_ENABLE,
				 EXYNOS8890_PMU_PCIE_PHY_ENABLE);
	if (ret)
		goto err_power_off;
	ep->phy_powered = true;

	exynos8890_phy_powerdown(ep, false);

	/* Ignore RX electrical-idle while the generic DWC core accesses DBI. */
	val = readl(ep->pcs_base + PCIE_PCS_RX_ELECIDLE);
	val |= PCIE_PCS_RX_ELECIDLE_IGNORE;
	writel(val, ep->pcs_base + PCIE_PCS_RX_ELECIDLE);

	exynos8890_elbi_writel(ep, 0, PCIE_SOFT_CORE_RESET);
	udelay(20);
	exynos8890_elbi_writel(ep, 1, PCIE_SOFT_CORE_RESET);

	/* Release endpoint reset before programming the PHY, as downstream does. */
	gpiod_set_value_cansleep(ep->perst, 0);
	usleep_range(18000, 20000);

	val = exynos8890_elbi_readl(ep, PCIE_APP_REQ_EXIT_L1_MODE);
	val |= APP_REQ_EXIT_L1_MODE | L1_REQ_NAK_CONTROL_MASTER;
	exynos8890_elbi_writel(ep, val, PCIE_APP_REQ_EXIT_L1_MODE);
	exynos8890_elbi_writel(ep, PCIE_LINKDOWN_RST_MANUAL,
			       PCIE_LINKDOWN_RST_CTRL_SEL);

	val = exynos8890_elbi_readl(ep, PCIE_QCH_SEL);
	val &= ~CLOCK_GATING_MASK;
	val |= CLOCK_NOT_GATING;
	exynos8890_elbi_writel(ep, val, PCIE_QCH_SEL);

	exynos8890_pcie_phy_config(ep);

	ret = clk_bulk_prepare_enable(EXYNOS8890_NUM_PHY_CLKS, ep->phy_clks);
	if (ret)
		goto err_power_off;
	ep->phy_clks_enabled = true;

	val = exynos8890_elbi_readl(ep, PCIE_SW_WAKE);
	val &= ~PCIE_BUS_EN;
	exynos8890_elbi_writel(ep, val, PCIE_SW_WAKE);

	exynos8890_elbi_writel(ep, 1, PCIE_L1_BUG_FIX_ENABLE);

	exynos8890_disable_irqs(ep);
	exynos8890_clear_irqs(ep);
	WRITE_ONCE(ep->irq_accessible, true);
	return 0;

err_power_off:
	exynos8890_pcie_power_off(ep);
	return ret;
}

static void exynos8890_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);

	exynos8890_pcie_power_off(ep);
}

static void exynos8890_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos8890_pcie *ep = to_exynos8890_pcie(pci);

	exynos8890_enable_irqs(ep);
}

static const struct dw_pcie_host_ops exynos8890_pcie_host_ops = {
	.init = exynos8890_pcie_host_init,
	.deinit = exynos8890_pcie_host_deinit,
	.post_init = exynos8890_pcie_host_post_init,
};

static const struct dw_pcie_ops exynos8890_pcie_ops = {
	.link_up = exynos8890_pcie_link_up,
	.get_ltssm = exynos8890_pcie_get_ltssm,
	.start_link = exynos8890_pcie_start_link,
	.stop_link = exynos8890_pcie_stop_link,
};

static int exynos8890_pcie_get_clocks(struct device *dev,
				      struct exynos8890_pcie *ep)
{
	int i, ret;

	for (i = 0; i < EXYNOS8890_NUM_CORE_CLKS; i++)
		ep->core_clks[i].id = exynos8890_core_clk_names[i];
	for (i = 0; i < EXYNOS8890_NUM_PHY_CLKS; i++)
		ep->phy_clks[i].id = exynos8890_phy_clk_names[i];

	ret = devm_clk_bulk_get(dev, EXYNOS8890_NUM_CORE_CLKS, ep->core_clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get PCIe clocks\n");

	ret = devm_clk_bulk_get(dev, EXYNOS8890_NUM_PHY_CLKS, ep->phy_clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get PCIe PHY clocks\n");

	return 0;
}

static int exynos8890_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos8890_pcie *ep;
	struct dw_pcie_rp *pp;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->pci.dev = dev;
	ep->pci.ops = &exynos8890_pcie_ops;
	pp = &ep->pci.pp;
	pp->ops = &exynos8890_pcie_host_ops;

	/*
	 * Exynos8890 exposes MSI through the same ELBI IRQ as other controller
	 * events.  Keep the DWC MSI domain, but demultiplex that physical IRQ
	 * in this driver instead of installing the generic chained handler.
	 */
	pp->msi_irq[0] = -ENODEV;

	platform_set_drvdata(pdev, ep);

	ep->phy_base = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(ep->phy_base))
		return PTR_ERR(ep->phy_base);

	ep->sysreg_base = devm_platform_ioremap_resource_byname(pdev, "sysreg");
	if (IS_ERR(ep->sysreg_base))
		return PTR_ERR(ep->sysreg_base);

	ep->pcs_base = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(ep->pcs_base))
		return PTR_ERR(ep->pcs_base);

	ep->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "samsung,pmu-syscon");
	if (IS_ERR(ep->pmu))
		return dev_err_probe(dev, PTR_ERR(ep->pmu),
				     "failed to get PMU syscon\n");

	ep->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ep->perst))
		return dev_err_probe(dev, PTR_ERR(ep->perst),
				     "failed to get PERST# GPIO\n");

	ep->vpcie = devm_regulator_get_optional(dev, "vpcie");
	if (IS_ERR(ep->vpcie)) {
		ret = PTR_ERR(ep->vpcie);
		if (ret == -ENODEV)
			ep->vpcie = NULL;
		else
			return dev_err_probe(
				dev, ret,
				"failed to get endpoint power supply\n");
	}

	ret = exynos8890_pcie_get_clocks(dev, ep);
	if (ret)
		return ret;

	ep->irq = platform_get_irq(pdev, 0);
	if (ep->irq < 0)
		return ep->irq;
	pp->irq = ep->irq;
	ret = devm_request_irq(dev, ep->irq, exynos8890_pcie_irq_handler,
			       IRQF_SHARED | IRQF_NO_THREAD, "exynos8890-pcie",
			       ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request ELBI IRQ\n");

	ret = dw_pcie_host_init(pp);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize PCIe host\n");

	return 0;
}

static void exynos8890_pcie_remove(struct platform_device *pdev)
{
	struct exynos8890_pcie *ep = platform_get_drvdata(pdev);

	/* Quiesce MSI dispatch before the generic core removes its IRQ domain. */
	exynos8890_pcie_quiesce_irqs(ep);
	dw_pcie_host_deinit(&ep->pci.pp);
}

static int exynos8890_pcie_suspend_noirq(struct device *dev)
{
	struct exynos8890_pcie *ep = dev_get_drvdata(dev);

	/* A failed resume has already powered the suspended controller off. */
	if (ep->pci.suspended)
		return 0;

	return dw_pcie_suspend_noirq(&ep->pci);
}

static int exynos8890_pcie_resume_noirq(struct device *dev)
{
	struct exynos8890_pcie *ep = dev_get_drvdata(dev);

	return dw_pcie_resume_noirq(&ep->pci);
}

static const struct dev_pm_ops exynos8890_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos8890_pcie_suspend_noirq,
				  exynos8890_pcie_resume_noirq)
};

static const struct of_device_id exynos8890_pcie_of_match[] = {
	{ .compatible = "samsung,exynos8890-pcie" },
	{}
};
MODULE_DEVICE_TABLE(of, exynos8890_pcie_of_match);

static struct platform_driver exynos8890_pcie_driver = {
	.probe = exynos8890_pcie_probe,
	.remove = exynos8890_pcie_remove,
	.driver = {
		.name = "exynos8890-pcie",
		.of_match_table = exynos8890_pcie_of_match,
		.pm = pm_sleep_ptr(&exynos8890_pcie_pm_ops),
	},
};
module_platform_driver(exynos8890_pcie_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 DesignWare PCIe host controller");
MODULE_LICENSE("GPL");
