// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos8890 MIPI CSIS/DSIM D-PHY driver, ported from vendor.
 *
 * dsim_drv.c does devm_phy_get(dev, "dsim_dphy") unconditionally at
 * probe - without a real phy provider bound for the "samsung,mipi-phy-*"
 * compatibles this returns -EPROBE_DEFER forever, so this is required
 * for dsim8890 to ever probe, not just an optional isolation-timing
 * nicety.
 *
 * Each phy is just an isolation-bypass + reset-release pair through the
 * PMU syscon, per lane group (M4S4 shared between DSIM0-2, M/S-x unique
 * per CSI instance) - no serdes/clock generation is modeled here, that
 * lives entirely in the DSIM/CSIS host IP itself.
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#define EXYNOS_MIPI_PHY_ISO_BYPASS	BIT(0)
#define EXYNOS_MIPI_PHYS_NUM		4

#define MIPI_PHY_MxSx_UNIQUE		(0 << 1)
#define MIPI_PHY_MxSx_SHARED		(1 << 1)
#define MIPI_PHY_MxSx_INIT_DONE	(2 << 1)

/* reference count for the shared phy-m4s4 (DSIM) group */
static int phy_m4s4_count;

enum exynos_mipi_phy_type {
	EXYNOS_MIPI_PHY_FOR_DSIM,
	EXYNOS_MIPI_PHY_FOR_CSIS,
};

struct mipi_phy_data {
	enum exynos_mipi_phy_type type;
	u8 flags;
};

struct mipi_phy_desc {
	struct phy *phy;
	unsigned int index;
	enum exynos_mipi_phy_type type;
	unsigned int iso_offset;
	unsigned int rst_bit;
	u8 flags;
};

struct exynos_mipi_phy {
	struct device *dev;
	spinlock_t slock;
	void __iomem *regs;
	struct regmap *reg_pmu;
	struct mipi_phy_desc phys[EXYNOS_MIPI_PHYS_NUM];
};

/* 1: Isolation bypass, 0: Isolation enable */
static int __set_phy_isolation(struct regmap *reg_pmu,
		unsigned int offset, unsigned int on)
{
	unsigned int val = on ? EXYNOS_MIPI_PHY_ISO_BYPASS : 0;

	return regmap_update_bits(reg_pmu, offset,
			EXYNOS_MIPI_PHY_ISO_BYPASS, val);
}

/* 1: Enable reset -> release reset, 0: Enable reset */
static void __set_phy_reset(struct exynos_mipi_phy *state,
		unsigned int bit, unsigned int on)
{
	void __iomem *addr = state->regs;
	unsigned int cfg;

	cfg = readl(addr);
	cfg &= ~(1 << bit);
	writel(cfg, addr);

	/* release a reset before using a PHY */
	if (on) {
		cfg |= (1 << bit);
		writel(cfg, addr);
	}
}

static int __set_phy_init(struct exynos_mipi_phy *state,
		struct mipi_phy_desc *phy_desc)
{
	unsigned int cfg;
	int ret;

	ret = regmap_read(state->reg_pmu, phy_desc->iso_offset, &cfg);
	if (ret) {
		dev_err(state->dev, "can't read 0x%x\n", phy_desc->iso_offset);
		return -EINVAL;
	}

	/* isolation already bypassed (e.g. LCD left on by bootloader) */
	if (cfg & EXYNOS_MIPI_PHY_ISO_BYPASS)
		phy_desc->flags |= MIPI_PHY_MxSx_INIT_DONE;

	return 0;
}

static int __set_phy_alone(struct exynos_mipi_phy *state,
		struct mipi_phy_desc *phy_desc, unsigned int on)
{
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&state->slock, flags);
	if (on) {
		ret = __set_phy_isolation(state->reg_pmu,
				phy_desc->iso_offset, on);
		__set_phy_reset(state, phy_desc->rst_bit, on);
	} else {
		__set_phy_reset(state, phy_desc->rst_bit, on);
		ret = __set_phy_isolation(state->reg_pmu,
				phy_desc->iso_offset, on);
	}
	spin_unlock_irqrestore(&state->slock, flags);

	return ret;
}

static DEFINE_SPINLOCK(lock_share);
static int __set_phy_share(struct exynos_mipi_phy *state,
		struct mipi_phy_desc *phy_desc, unsigned int on)
{
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&lock_share, flags);

	on ? ++phy_m4s4_count : --phy_m4s4_count;

	/* already brought up (INIT_DONE from bootloader-left-on state) */
	if (phy_desc->flags & MIPI_PHY_MxSx_INIT_DONE) {
		phy_desc->flags &= ~MIPI_PHY_MxSx_INIT_DONE;
		spin_unlock_irqrestore(&lock_share, flags);
		return ret;
	}

	if (on) {
		if (phy_m4s4_count == 1)
			ret = __set_phy_isolation(state->reg_pmu,
					phy_desc->iso_offset, on);
		__set_phy_reset(state, phy_desc->rst_bit, on);
	} else {
		__set_phy_reset(state, phy_desc->rst_bit, on);
		if (phy_m4s4_count == 0)
			ret = __set_phy_isolation(state->reg_pmu,
					phy_desc->iso_offset, on);
	}

	spin_unlock_irqrestore(&lock_share, flags);

	return ret;
}

static int __set_phy_state(struct exynos_mipi_phy *state,
		struct mipi_phy_desc *phy_desc, unsigned int on)
{
	if (phy_desc->flags & MIPI_PHY_MxSx_SHARED)
		return __set_phy_share(state, phy_desc, on);
	return __set_phy_alone(state, phy_desc, on);
}

static const struct mipi_phy_data mipi_phy_m4sx = {
	.type = EXYNOS_MIPI_PHY_FOR_DSIM,
	.flags = MIPI_PHY_MxSx_SHARED,
};

static const struct mipi_phy_data mipi_phy_mxs4 = {
	.type = EXYNOS_MIPI_PHY_FOR_CSIS,
	.flags = MIPI_PHY_MxSx_SHARED,
};

static const struct mipi_phy_data mipi_phy_mxs0 = {
	.type = EXYNOS_MIPI_PHY_FOR_DSIM,
	.flags = MIPI_PHY_MxSx_UNIQUE,
};

static const struct mipi_phy_data mipi_phy_m0sx = {
	.type = EXYNOS_MIPI_PHY_FOR_CSIS,
	.flags = MIPI_PHY_MxSx_UNIQUE,
};

static const struct of_device_id exynos_mipi_phy_of_table[] = {
	{ .compatible = "samsung,mipi-phy-dsim", .data = &mipi_phy_m4sx },
	{ .compatible = "samsung,mipi-phy-m4",   .data = &mipi_phy_mxs0 },
	{ .compatible = "samsung,mipi-phy-m2",   .data = &mipi_phy_mxs0 },
	{ .compatible = "samsung,mipi-phy-m1",   .data = &mipi_phy_mxs0 },
	{ .compatible = "samsung,mipi-phy-csis", .data = &mipi_phy_mxs4 },
	{ .compatible = "samsung,mipi-phy-s4",   .data = &mipi_phy_m0sx },
	{ .compatible = "samsung,mipi-phy-s2",   .data = &mipi_phy_m0sx },
	{ .compatible = "samsung,mipi-phy-s1",   .data = &mipi_phy_m0sx },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_mipi_phy_of_table);

#define to_mipi_phy(desc) \
	container_of((desc), struct exynos_mipi_phy, phys[(desc)->index])

static int exynos_mipi_phy_init(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_mipi_phy(phy_desc);

	return __set_phy_init(state, phy_desc);
}

static int exynos_mipi_phy_power_on(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_mipi_phy(phy_desc);

	return __set_phy_state(state, phy_desc, 1);
}

static int exynos_mipi_phy_power_off(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_mipi_phy(phy_desc);

	return __set_phy_state(state, phy_desc, 0);
}

static struct phy *exynos_mipi_phy_of_xlate(struct device *dev,
		const struct of_phandle_args *args)
{
	struct exynos_mipi_phy *state = dev_get_drvdata(dev);

	if (args->args[0] >= EXYNOS_MIPI_PHYS_NUM)
		return ERR_PTR(-ENODEV);

	return state->phys[args->args[0]].phy;
}

static const struct phy_ops exynos_mipi_phy_ops = {
	.init		= exynos_mipi_phy_init,
	.power_on	= exynos_mipi_phy_power_on,
	.power_off	= exynos_mipi_phy_power_off,
};

static int exynos_mipi_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct exynos_mipi_phy *state;
	struct phy_provider *phy_provider;
	const struct mipi_phy_data *phy_data;
	unsigned int iso[EXYNOS_MIPI_PHYS_NUM];
	unsigned int rst[EXYNOS_MIPI_PHYS_NUM];
	unsigned int i, elements;
	int ret;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->dev = dev;
	state->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(state->regs))
		return PTR_ERR(state->regs);

	phy_data = of_device_get_match_data(dev);
	if (!phy_data)
		return -EINVAL;

	phy_m4s4_count = 0;

	dev_set_drvdata(dev, state);
	spin_lock_init(&state->slock);

	state->reg_pmu = syscon_regmap_lookup_by_phandle(node,
			"samsung,pmu-syscon");
	if (IS_ERR(state->reg_pmu)) {
		dev_err(dev, "failed to look up PMU regmap\n");
		return PTR_ERR(state->reg_pmu);
	}

	ret = of_property_count_u32_elems(node, "isolation");
	if (ret <= 0 || ret > EXYNOS_MIPI_PHYS_NUM)
		return -EINVAL;
	elements = ret;

	ret = of_property_read_u32_array(node, "isolation", iso, elements);
	if (ret) {
		dev_err(dev, "cannot get mipi-phy isolation\n");
		return ret;
	}

	ret = of_property_read_u32_array(node, "reset", rst, elements);
	if (ret) {
		dev_err(dev, "cannot get mipi-phy reset\n");
		return ret;
	}

	for (i = 0; i < elements; i++) {
		struct phy *generic_phy;

		state->phys[i].iso_offset = iso[i];
		state->phys[i].rst_bit = rst[i];

		generic_phy = devm_phy_create(dev, NULL, &exynos_mipi_phy_ops);
		if (IS_ERR(generic_phy)) {
			dev_err(dev, "failed to create PHY\n");
			return PTR_ERR(generic_phy);
		}

		state->phys[i].index = i;
		state->phys[i].phy = generic_phy;
		state->phys[i].type = phy_data->type;
		/* only index 0 can be the SHARED (M4S4) group */
		state->phys[i].flags = (i == 0) ? phy_data->flags
						 : MIPI_PHY_MxSx_UNIQUE;
		phy_set_drvdata(generic_phy, &state->phys[i]);
	}

	phy_provider = devm_of_phy_provider_register(dev,
			exynos_mipi_phy_of_xlate);
	if (IS_ERR(phy_provider)) {
		dev_err(dev, "failed to register phy provider\n");
		return PTR_ERR(phy_provider);
	}

	return 0;
}

static struct platform_driver exynos_mipi_phy_driver = {
	.probe	= exynos_mipi_phy_probe,
	.driver	= {
		.name		= "exynos8890-mipi-phy",
		.of_match_table	= exynos_mipi_phy_of_table,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos_mipi_phy_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 MIPI CSI/DSI D-PHY driver");
MODULE_LICENSE("GPL v2");
