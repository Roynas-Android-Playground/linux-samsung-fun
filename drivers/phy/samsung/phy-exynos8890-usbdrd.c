// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 USB DRD PHY
 *
 * Exynos8890 uses the older Exynos USB DRD register layout, but its clock
 * layout, 26 MHz differential reference-clock programming and HS tuning are
 * sufficiently different from the generic Exynos7 data to warrant a small
 * dedicated driver. Herolte only exposes the UTMI/USB2 path at the connector;
 * the PIPE3/SuperSpeed PHY is deliberately not registered here.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define EXYNOS8890_USBDEV_PHY_CONTROL		0x704
#define EXYNOS8890_USBDRD_ENABLE		BIT(0)

#define EXYNOS8890_DRD_LINKSYSTEM		0x04
#define LINKSYSTEM_XHCI_VERSION_CONTROL		BIT(27)
#define LINKSYSTEM_FLADJ			GENMASK(6, 1)

#define EXYNOS8890_DRD_PHYUTMI			0x08
#define PHYUTMI_VBUSVLDEXTSEL			BIT(10)
#define PHYUTMI_VBUSVLDEXT			BIT(9)
#define PHYUTMI_OTGDISABLE			BIT(6)
#define PHYUTMI_IDPULLUP			BIT(5)
#define PHYUTMI_DRVVBUS				BIT(4)
#define PHYUTMI_DPPULLDOWN			BIT(3)
#define PHYUTMI_DMPULLDOWN			BIT(2)
#define PHYUTMI_FORCESUSPEND			BIT(1)
#define PHYUTMI_FORCESLEEP			BIT(0)

#define EXYNOS8890_DRD_PHYPIPE			0x0c
#define PHYPIPE_UTMI_CLKSEL			BIT(4)

#define EXYNOS8890_DRD_PHYCLKRST		0x10
#define PHYCLKRST_EN_UTMISUSPEND		BIT(31)
#define PHYCLKRST_SSC_EN			BIT(20)
#define PHYCLKRST_REF_SSP_EN			BIT(19)
#define PHYCLKRST_REF_CLKDIV2			BIT(18)
#define PHYCLKRST_MPLL_MULTIPLIER		GENMASK(17, 11)
/* Exynos8890 CAL uses a six-bit FSEL field rather than the old 3-bit field. */
#define PHYCLKRST_FSEL				GENMASK(10, 5)
#define PHYCLKRST_RETENABLEN			BIT(4)
#define PHYCLKRST_REFCLKSEL			GENMASK(3, 2)
#define PHYCLKRST_PORTRESET			BIT(1)
#define PHYCLKRST_COMMONONN			BIT(0)

#define EXYNOS8890_DRD_PHYREG0			0x14
#define PHYREG0_SSC_REFCLKSEL			GENMASK(31, 23)
#define PHYREG0_SSC_RANGE			GENMASK(22, 20)

#define EXYNOS8890_DRD_PHYPARAM0		0x1c
#define PHYPARAM0_REF_USE_PAD			BIT(31)
#define PHYPARAM0_TXVREFTUNE			GENMASK(25, 22)
#define PHYPARAM0_TXRISETUNE			GENMASK(21, 20)
#define PHYPARAM0_TXRESTUNE			GENMASK(19, 18)
#define PHYPARAM0_TXPREEMPPULSETUNE		BIT(17)
#define PHYPARAM0_TXPREEMPAMPTUNE		GENMASK(16, 15)
#define PHYPARAM0_TXHSXVTUNE			GENMASK(14, 13)
#define PHYPARAM0_TXFSLSTUNE			GENMASK(12, 9)
#define PHYPARAM0_SQRXTUNE			GENMASK(8, 6)
#define PHYPARAM0_OTGTUNE			GENMASK(5, 3)
#define PHYPARAM0_COMPDISTUNE			GENMASK(2, 0)

#define EXYNOS8890_DRD_PHYTEST			0x28
#define PHYTEST_POWERDOWN_SSP			BIT(3)
#define PHYTEST_POWERDOWN_HSP			BIT(2)

#define EXYNOS8890_DRD_PHYRESUME		0x34
#define PHYRESUME_DIS_LINKGATE_QACT		BIT(13)
#define PHYRESUME_DIS_ID0_QACT			BIT(12)
#define PHYRESUME_DIS_VBUSVALID_QACT		BIT(11)
#define PHYRESUME_DIS_BVALID_QACT		BIT(10)
#define PHYRESUME_FORCE_QACT			BIT(9)

#define EXYNOS8890_DRD_LINKPORT			0x44
#define LINKPORT_HOST_PORT_OVCR_U3_SEL		BIT(3)
#define LINKPORT_HOST_PORT_OVCR_U2_SEL		BIT(2)

#define EXYNOS8890_REFCLK_RATE			26000000UL
#define EXYNOS8890_REFSEL_DIFF_INTERNAL		0x4
#define EXYNOS8890_FSEL_DIFF_26MHZ		0x02
#define EXYNOS8890_MPLL_26MHZ			0x60
#define EXYNOS8890_SSC_REFCLKSEL_26MHZ		0x108

struct exynos8890_usbdrd_phy {
	struct device *dev;
	void __iomem *regs;
	struct regmap *pmu;
	struct clk_bulk_data clks[3];
	struct phy *phy;
};

static void exynos8890_usbdrd_tune_hs(struct exynos8890_usbdrd_phy *phy,
				      bool host)
{
	u32 reg;

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYPARAM0);
	reg &= ~(PHYPARAM0_TXVREFTUNE |
		 PHYPARAM0_TXRISETUNE |
		 PHYPARAM0_TXRESTUNE |
		 PHYPARAM0_TXPREEMPPULSETUNE |
		 PHYPARAM0_TXPREEMPAMPTUNE |
		 PHYPARAM0_TXHSXVTUNE |
		 PHYPARAM0_TXFSLSTUNE |
		 PHYPARAM0_SQRXTUNE |
		 PHYPARAM0_OTGTUNE |
		 PHYPARAM0_COMPDISTUNE);

	/* Vendor defaults for the Exynos8890 USB3DRD HS block. */
	reg |= FIELD_PREP(PHYPARAM0_TXVREFTUNE, host ? 0x1 : 0xb);
	reg |= FIELD_PREP(PHYPARAM0_TXRISETUNE, 0x3);
	reg |= FIELD_PREP(PHYPARAM0_TXRESTUNE, host ? 0x3 : 0x2);
	reg |= FIELD_PREP(PHYPARAM0_TXPREEMPAMPTUNE, host ? 0x0 : 0x3);
	reg |= FIELD_PREP(PHYPARAM0_TXHSXVTUNE, 0x0);
	reg |= FIELD_PREP(PHYPARAM0_TXFSLSTUNE, 0x3);
	reg |= FIELD_PREP(PHYPARAM0_SQRXTUNE, 0x7);
	reg |= FIELD_PREP(PHYPARAM0_OTGTUNE, 0x4);
	reg |= FIELD_PREP(PHYPARAM0_COMPDISTUNE, host ? 0x7 : 0x0);

	writel(reg, phy->regs + EXYNOS8890_DRD_PHYPARAM0);
}

static void exynos8890_usbdrd_qchannel_enable(struct exynos8890_usbdrd_phy *phy)
{
	u32 reg;

	/* Mirror the Exynos8890 vendor CAL Q-channel workaround. */
	reg = readl(phy->regs + EXYNOS8890_DRD_PHYRESUME);
	reg |= PHYRESUME_DIS_ID0_QACT |
	       PHYRESUME_DIS_VBUSVALID_QACT |
	       PHYRESUME_DIS_BVALID_QACT |
	       PHYRESUME_DIS_LINKGATE_QACT;
	reg &= ~PHYRESUME_FORCE_QACT;
	udelay(500);
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYRESUME);

	udelay(500);
	reg = readl(phy->regs + EXYNOS8890_DRD_PHYRESUME);
	reg |= PHYRESUME_FORCE_QACT;
	udelay(500);
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYRESUME);
}

static int exynos8890_usbdrd_hw_init(struct exynos8890_usbdrd_phy *phy)
{
	u32 reg, phyclkrst, phyreg0;
	unsigned long ref_rate = clk_get_rate(phy->clks[1].clk);

	if (ref_rate != EXYNOS8890_REFCLK_RATE) {
		dev_err(phy->dev, "unsupported USB PHY reference clock: %lu Hz\n",
			ref_rate);
		return -EINVAL;
	}

	exynos8890_usbdrd_qchannel_enable(phy);

	reg = readl(phy->regs + EXYNOS8890_DRD_LINKSYSTEM);
	reg &= ~LINKSYSTEM_FLADJ;
	reg |= FIELD_PREP(LINKSYSTEM_FLADJ, 0x20) |
	       LINKSYSTEM_XHCI_VERSION_CONTROL;
	writel(reg, phy->regs + EXYNOS8890_DRD_LINKSYSTEM);

	/* Match the Exynos8890 v1.01 CAL clock setup for the internal 26 MHz ref. */
	phyclkrst = readl(phy->regs + EXYNOS8890_DRD_PHYCLKRST);
	phyclkrst |= PHYCLKRST_PORTRESET;
	phyclkrst &= ~PHYCLKRST_REFCLKSEL;
	/*
	 * The vendor v1.01 CAL encodes DIFF_INTERNAL as 0x4 << 2. Bit 4
	 * overlaps the legacy RETENABLEN definition; preserve that exact
	 * programming instead of squeezing it through the old two-bit field.
	 */
	phyclkrst |= EXYNOS8890_REFSEL_DIFF_INTERNAL << 2;
	phyclkrst &= ~PHYCLKRST_FSEL;
	phyclkrst |= FIELD_PREP(PHYCLKRST_FSEL, EXYNOS8890_FSEL_DIFF_26MHZ);
	phyclkrst &= ~(PHYCLKRST_COMMONONN | PHYCLKRST_EN_UTMISUSPEND |
			 PHYCLKRST_MPLL_MULTIPLIER | PHYCLKRST_REF_CLKDIV2);
	phyclkrst |= PHYCLKRST_RETENABLEN | PHYCLKRST_REF_SSP_EN |
		      PHYCLKRST_SSC_EN |
		      FIELD_PREP(PHYCLKRST_MPLL_MULTIPLIER,
				 EXYNOS8890_MPLL_26MHZ);

	phyreg0 = readl(phy->regs + EXYNOS8890_DRD_PHYREG0);
	phyreg0 &= ~(PHYREG0_SSC_REFCLKSEL | PHYREG0_SSC_RANGE);
	phyreg0 |= FIELD_PREP(PHYREG0_SSC_REFCLKSEL,
			      EXYNOS8890_SSC_REFCLKSEL_26MHZ);
	writel(phyreg0, phy->regs + EXYNOS8890_DRD_PHYREG0);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYPARAM0);
	reg &= ~PHYPARAM0_REF_USE_PAD;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYPARAM0);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYPIPE);
	reg |= PHYPIPE_UTMI_CLKSEL;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYPIPE);

	writel(phyclkrst, phy->regs + EXYNOS8890_DRD_PHYCLKRST);
	udelay(10);
	phyclkrst &= ~PHYCLKRST_PORTRESET;
	writel(phyclkrst, phy->regs + EXYNOS8890_DRD_PHYCLKRST);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYTEST);
	reg &= ~(PHYTEST_POWERDOWN_HSP | PHYTEST_POWERDOWN_SSP);
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYTEST);
	fsleep(500);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYUTMI);
	reg &= ~(PHYUTMI_FORCESLEEP | PHYUTMI_FORCESUSPEND |
		 PHYUTMI_DMPULLDOWN | PHYUTMI_DPPULLDOWN | PHYUTMI_DRVVBUS);
	reg |= PHYUTMI_OTGDISABLE;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYUTMI);

	/* Exynos8890 uses the external OVC inputs for both host ports. */
	reg = readl(phy->regs + EXYNOS8890_DRD_LINKPORT);
	reg &= ~(LINKPORT_HOST_PORT_OVCR_U3_SEL |
		 LINKPORT_HOST_PORT_OVCR_U2_SEL);
	writel(reg, phy->regs + EXYNOS8890_DRD_LINKPORT);

	exynos8890_usbdrd_tune_hs(phy, false);

	return 0;
}

static int exynos8890_usbdrd_init(struct phy *generic_phy)
{
	struct exynos8890_usbdrd_phy *phy = phy_get_drvdata(generic_phy);
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(phy->clks), phy->clks);
	if (ret)
		return ret;

	ret = exynos8890_usbdrd_hw_init(phy);
	clk_bulk_disable_unprepare(ARRAY_SIZE(phy->clks), phy->clks);

	return ret;
}

static int exynos8890_usbdrd_exit(struct phy *generic_phy)
{
	struct exynos8890_usbdrd_phy *phy = phy_get_drvdata(generic_phy);
	u32 reg;
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(phy->clks), phy->clks);
	if (ret)
		return ret;

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYUTMI);
	reg &= ~(PHYUTMI_DRVVBUS | PHYUTMI_IDPULLUP |
		 PHYUTMI_VBUSVLDEXT | PHYUTMI_VBUSVLDEXTSEL);
	reg |= PHYUTMI_FORCESUSPEND | PHYUTMI_FORCESLEEP;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYUTMI);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYTEST);
	reg |= PHYTEST_POWERDOWN_HSP | PHYTEST_POWERDOWN_SSP;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYTEST);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYCLKRST);
	reg &= ~(PHYCLKRST_REF_SSP_EN | PHYCLKRST_SSC_EN |
		 PHYCLKRST_COMMONONN);
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYCLKRST);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYRESUME);
	reg &= ~PHYRESUME_FORCE_QACT;
	reg |= PHYRESUME_DIS_ID0_QACT |
	       PHYRESUME_DIS_VBUSVALID_QACT |
	       PHYRESUME_DIS_BVALID_QACT |
	       PHYRESUME_DIS_LINKGATE_QACT;
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYRESUME);

	clk_bulk_disable_unprepare(ARRAY_SIZE(phy->clks), phy->clks);
	return 0;
}

static int exynos8890_usbdrd_power_on(struct phy *generic_phy)
{
	struct exynos8890_usbdrd_phy *phy = phy_get_drvdata(generic_phy);
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(phy->clks), phy->clks);
	if (ret)
		return ret;

	ret = regmap_update_bits(phy->pmu, EXYNOS8890_USBDEV_PHY_CONTROL,
				 EXYNOS8890_USBDRD_ENABLE,
				 EXYNOS8890_USBDRD_ENABLE);
	if (ret)
		clk_bulk_disable_unprepare(ARRAY_SIZE(phy->clks), phy->clks);

	return ret;
}

static int exynos8890_usbdrd_power_off(struct phy *generic_phy)
{
	struct exynos8890_usbdrd_phy *phy = phy_get_drvdata(generic_phy);
	int ret;

	ret = regmap_update_bits(phy->pmu, EXYNOS8890_USBDEV_PHY_CONTROL,
				 EXYNOS8890_USBDRD_ENABLE, 0);
	clk_bulk_disable_unprepare(ARRAY_SIZE(phy->clks), phy->clks);

	return ret;
}

static int exynos8890_usbdrd_set_mode(struct phy *generic_phy,
				      enum phy_mode mode, int submode)
{
	struct exynos8890_usbdrd_phy *phy = phy_get_drvdata(generic_phy);
	bool host;
	u32 reg;
	int ret;

	if (mode != PHY_MODE_USB_HOST && mode != PHY_MODE_USB_DEVICE)
		return 0;

	host = mode == PHY_MODE_USB_HOST;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(phy->clks), phy->clks);
	if (ret)
		return ret;

	exynos8890_usbdrd_tune_hs(phy, host);

	reg = readl(phy->regs + EXYNOS8890_DRD_PHYUTMI);
	if (host) {
		reg |= PHYUTMI_DMPULLDOWN | PHYUTMI_DPPULLDOWN;
		reg &= ~(PHYUTMI_VBUSVLDEXTSEL | PHYUTMI_VBUSVLDEXT);
	} else {
		reg &= ~(PHYUTMI_DMPULLDOWN | PHYUTMI_DPPULLDOWN);
	}
	writel(reg, phy->regs + EXYNOS8890_DRD_PHYUTMI);

	clk_bulk_disable_unprepare(ARRAY_SIZE(phy->clks), phy->clks);
	return 0;
}

static const struct phy_ops exynos8890_usbdrd_ops = {
	.init = exynos8890_usbdrd_init,
	.exit = exynos8890_usbdrd_exit,
	.power_on = exynos8890_usbdrd_power_on,
	.power_off = exynos8890_usbdrd_power_off,
	.set_mode = exynos8890_usbdrd_set_mode,
	.owner = THIS_MODULE,
};

static struct phy *exynos8890_usbdrd_xlate(struct device *dev,
					  const struct of_phandle_args *args)
{
	struct exynos8890_usbdrd_phy *phy = dev_get_drvdata(dev);

	if (args->args_count != 1 || args->args[0] != 0)
		return ERR_PTR(-ENODEV);

	return phy->phy;
}

static int exynos8890_usbdrd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos8890_usbdrd_phy *phy;
	struct phy_provider *provider;
	int ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->dev = dev;
	phy->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy->regs))
		return PTR_ERR(phy->regs);

	phy->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "samsung,pmu-syscon");
	if (IS_ERR(phy->pmu))
		return dev_err_probe(dev, PTR_ERR(phy->pmu),
				     "failed to get PMU syscon\n");

	phy->clks[0].id = "phy";
	phy->clks[1].id = "ref";
	phy->clks[2].id = "pipe";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(phy->clks), phy->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get USB PHY clocks\n");

	phy->phy = devm_phy_create(dev, NULL, &exynos8890_usbdrd_ops);
	if (IS_ERR(phy->phy))
		return dev_err_probe(dev, PTR_ERR(phy->phy),
				     "failed to create USB PHY\n");

	phy_set_drvdata(phy->phy, phy);
	platform_set_drvdata(pdev, phy);

	provider = devm_of_phy_provider_register(dev, exynos8890_usbdrd_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "failed to register USB PHY provider\n");

	return 0;
}

static const struct of_device_id exynos8890_usbdrd_of_match[] = {
	{ .compatible = "samsung,exynos8890-usbdrd-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_usbdrd_of_match);

static struct platform_driver exynos8890_usbdrd_driver = {
	.probe = exynos8890_usbdrd_probe,
	.driver = {
		.name = "exynos8890-usbdrd-phy",
		.of_match_table = exynos8890_usbdrd_of_match,
	},
};
module_platform_driver(exynos8890_usbdrd_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 USB DRD UTMI PHY driver");
MODULE_LICENSE("GPL");
