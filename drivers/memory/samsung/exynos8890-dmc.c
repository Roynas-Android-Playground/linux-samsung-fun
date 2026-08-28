// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 memory-interface clock and LPDDR4 DVFS owner
 *
 * Exynos8890 cannot change the MIF PLL as a collection of independent CCF
 * operations.  Four DREX channels, their PHY timing sets, the shared MIF PLL,
 * CCORE clocks and the PSCDC FIFO form one hardware transaction.  This driver
 * is the only writer for that transaction.  The intermediate state stays
 * private; consumers can request only a complete rate-and-voltage operation.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/samsung.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/s2mps16.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/suspend.h>

#include <linux/soc/samsung/exynos8890-apm.h>
#include <linux/soc/samsung/exynos8890-calibration.h>
#include <linux/soc/samsung/exynos8890-dmc.h>

#include "exynos8890-dmc-internal.h"

#define EXYNOS8890_DMC_CHANNELS		4
#define EXYNOS8890_MIF_SWITCH_LOW_HZ	528000000UL
#define EXYNOS8890_MIF_SWITCH_HIGH_HZ	936000000UL
#define EXYNOS8890_BUS0_PLL_HZ		1056000000UL
#define EXYNOS8890_BUS3_PLL_HZ		1872000000UL
#define EXYNOS8890_MIF_BUS3_THRESHOLD_HZ	1600000000UL
#define EXYNOS8890_MIF_VTH_THRESHOLD_HZ	1539000000UL
#define EXYNOS8890_MIF_DIFF_THRESHOLD_HZ	962000000UL
#define EXYNOS8890_MIF_CMOS_THRESHOLD_HZ	468000000UL
#define EXYNOS8890_MIF_CMOS_OFFSET_UV	56250U
#define EXYNOS8890_MIF_MAX_UV		1250000U

#define MIF_PLL_LOCK			0x0000
#define MIF_PLL_CON0			0x0100
#define MIF_PLL_ENABLE			BIT(31)
#define MIF_PLL_LOCKED			BIT(29)
#define MIF_PLL_MDIV			GENMASK(25, 16)
#define MIF_PLL_PDIV			GENMASK(13, 8)
#define MIF_PLL_SDIV			GENMASK(2, 0)

#define TOP_ROOTCLKEN0			0x0f10
#define TOP_ROOTCLKEN3			0x0f1c
#define TOP_SWITCH_MUX_CTRL		0x0388
#define TOP_SWITCH_MUX_STATUS		0x0688
#define TOP_SWITCH_MUX_SELECTOR		GENMASK(14, 12)
#define TOP_SWITCH_MUX_STATUS_BITS	GENMASK(19, 12)
#define TOP_SWITCH_MUX_GATE		BIT(21)
#define TOP_SWITCH_GATE_CTRL		0x0998
#define TOP_SWITCH_GATE_ENABLE		BIT(0)
#define TOP_MIF_PLL_MUX_CTRL		0x1088
#define TOP_MIF_BUS_PLL_MUX_CTRL	0x108c
#define TOP_MIF_ACLK_MUX_CTRL		0x1090
#define TOP_MIF_MUX_SELECTOR		BIT(12)
#define TOP_BUS0_PLL_CON0		0x0100
#define TOP_BUS3_PLL_CON0		0x0160
#define TOP_BUS0_PLL_MUX_CTRL		0x0200
#define TOP_BUS3_PLL_MUX_CTRL		0x020c
#define TOP_BUS0_PLL_MUX_STATUS		0x0500
#define TOP_BUS3_PLL_MUX_STATUS		0x050c
#define TOP_BUS_PLL_MUX_SELECTOR	BIT(12)
#define TOP_BUS_PLL_MUX_STATUS_BITS	GENMASK(13, 12)
#define TOP_BUS_PLL_OUTPUT_ENABLE	BIT(21)
#define TOP_CCORE_800_MUX_CTRL		0x0240
#define TOP_CCORE_800_MUX_STATUS	0x0540
#define TOP_CCORE_800_DIV_CTRL		0x03a0
#define TOP_CCORE_800_MUX_SELECTOR	GENMASK(14, 12)
#define TOP_CCORE_800_MUX_STATUS_BITS	GENMASK(19, 12)
#define TOP_CCORE_800_DIV_RATIO		GENMASK(3, 0)
#define TOP_CCORE_800_DIV_BUSY		BIT(25)
#define TOP_PSCDC_CTRL0			0x1000
#define TOP_PSCDC_CTRL1			0x1004
#define TOP_PSCDC_SCI_FIFO0		0x1010
#define TOP_PSCDC_SMC_FIFO0		0x1020
#define TOP_MIF_CLK_CTRL0		0x1080
#define TOP_PSCDC_BUSY			GENMASK(31, 30)
#define TOP_CCORE_OUTPUT_ENABLE		BIT(21)

#define MIF_PSCDC_CTRL			0x1000
#define MIF_QCH_CTRL			0x2000
#define MIF_QCH_PREPARE			0x003f0000
#define MIF_QCH_FINISH			0x003f1001
#define MIF_DDRPHY_GATE_VALUE		0x0a08
#define MIF_DDRPHY_GATE_MANUAL		0x1a08
#define MIF_BUS_PLL_USER_CTRL		0x0204
#define MIF_BUS_PLL_USER_ENABLE		BIT(21)
#define MIF_BUS_PLL_USER_SELECTOR	BIT(12)
#define MIF_PLL_MUX_CTRL		0x0200
#define MIF_ACLK_MUX_CTRL		0x0208
#define MIF_PLL_MUX_STATUS		0x0600
#define MIF_BUS_PLL_USER_STATUS		0x0604
#define MIF_ACLK_MUX_STATUS		0x0608
#define MIF_SOURCE_MUX_SELECTOR		BIT(12)
#define MIF_SOURCE_MUX_STATUS		GENMASK(13, 12)
#define DMC_MISC_TIMING_SET		0x003c

#define CCORE_800_USER_CTRL		0x0200
#define CCORE_800_USER_STATUS		0x0500
#define CCORE_800_USER_ENABLE		BIT(21)

#define PMU_DREX_CALIBRATION1		0x09a4
#define PMU_DREX_CALIBRATION2		0x09a8
#define PMU_DREX_CALIBRATION3		0x09ac

#define DMC_IO_TIMEOUT_US		20000
#define DMC_CLK_TIMEOUT_US		1000

enum exynos8890_dmc_region_id {
	DMC_REGION_MISC_CCORE,
	DMC_REGION_TOP,
	DMC_REGION_CCORE,
	DMC_REGION_MIF0,
	DMC_REGION_MIF1,
	DMC_REGION_MIF2,
	DMC_REGION_MIF3,
	DMC_REGION_SMC0,
	DMC_REGION_SMC1,
	DMC_REGION_SMC2,
	DMC_REGION_SMC3,
	DMC_REGION_PHY0,
	DMC_REGION_PHY1,
	DMC_REGION_PHY2,
	DMC_REGION_PHY3,
	DMC_REGION_MISC0,
	DMC_REGION_MISC1,
	DMC_REGION_MISC2,
	DMC_REGION_MISC3,
	DMC_REGION_COUNT,
};

struct exynos8890_dmc_region {
	const char *name;
	phys_addr_t physical;
	size_t size;
	void __iomem *base;
};

#define DMC_REGION(_name, _physical, _size) \
	{ .name = (_name), .physical = (_physical), .size = (_size) }

static const struct exynos8890_dmc_region exynos8890_dmc_region_template[] = {
	[DMC_REGION_MISC_CCORE] = DMC_REGION("dmc-misc-ccore", DMC_MISC_CCORE_BASE, SZ_64K),
	[DMC_REGION_TOP] = DMC_REGION("cmu-top", CMU_TOP_BASE, SZ_32K),
	[DMC_REGION_CCORE] = DMC_REGION("cmu-ccore", CMU_CCORE_BASE, SZ_32K),
	[DMC_REGION_MIF0] = DMC_REGION("cmu-mif0", CMU_MIF0_BASE, SZ_32K),
	[DMC_REGION_MIF1] = DMC_REGION("cmu-mif1", CMU_MIF1_BASE, SZ_32K),
	[DMC_REGION_MIF2] = DMC_REGION("cmu-mif2", CMU_MIF2_BASE, SZ_32K),
	[DMC_REGION_MIF3] = DMC_REGION("cmu-mif3", CMU_MIF3_BASE, SZ_32K),
	[DMC_REGION_SMC0] = DMC_REGION("smc0", SMC0_BASE, SZ_64K),
	[DMC_REGION_SMC1] = DMC_REGION("smc1", SMC1_BASE, SZ_64K),
	[DMC_REGION_SMC2] = DMC_REGION("smc2", SMC2_BASE, SZ_64K),
	[DMC_REGION_SMC3] = DMC_REGION("smc3", SMC3_BASE, SZ_64K),
	[DMC_REGION_PHY0] = DMC_REGION("phy0", LPDDR4_PHY0_BASE, SZ_64K),
	[DMC_REGION_PHY1] = DMC_REGION("phy1", LPDDR4_PHY1_BASE, SZ_64K),
	[DMC_REGION_PHY2] = DMC_REGION("phy2", LPDDR4_PHY2_BASE, SZ_64K),
	[DMC_REGION_PHY3] = DMC_REGION("phy3", LPDDR4_PHY3_BASE, SZ_64K),
	[DMC_REGION_MISC0] = DMC_REGION("dmc-misc0", DMC_MISC0_BASE, SZ_4K),
	[DMC_REGION_MISC1] = DMC_REGION("dmc-misc1", DMC_MISC1_BASE, SZ_4K),
	[DMC_REGION_MISC2] = DMC_REGION("dmc-misc2", DMC_MISC2_BASE, SZ_4K),
	[DMC_REGION_MISC3] = DMC_REGION("dmc-misc3", DMC_MISC3_BASE, SZ_4K),
};

enum exynos8890_dmc_field_kind {
	DMC_FIELD_PLL,
	DMC_FIELD_BUS3_PLL,
	DMC_FIELD_MUX,
	DMC_FIELD_DIV,
};

struct exynos8890_dmc_field {
	const char *name;
	enum exynos8890_dmc_field_kind kind;
	enum exynos8890_calib_member_type type;
	u8 region;
	u16 offset;
	u16 status_offset;
	u8 shift;
	u8 width;
	u8 status_shift;
	u8 status_width;
};

#define DMC_PLL_FIELD(_name) \
	{ .name = (_name), .kind = DMC_FIELD_PLL, \
	  .type = EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ }
#define DMC_BUS3_PLL_FIELD(_name) \
	{ .name = (_name), .kind = DMC_FIELD_BUS3_PLL, \
	  .type = EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ }
#define DMC_MUX_FIELD(_name, _region, _offset, _status, _width, _stat_width) \
	{ .name = (_name), .kind = DMC_FIELD_MUX, \
	  .type = EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR, .region = (_region), \
	  .offset = (_offset), .status_offset = (_status), .shift = 12, \
	  .width = (_width), .status_shift = 12, .status_width = (_stat_width) }
#define DMC_DIV_FIELD(_name, _region, _offset, _width) \
	{ .name = (_name), .kind = DMC_FIELD_DIV, \
	  .type = EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE, \
	  .region = (_region), .offset = (_offset), .status_offset = (_offset), \
	  .width = (_width), .status_shift = 25, .status_width = 1 }

/* Every clock column in the vendor dvfs_mif matrix has one native owner. */
static const struct exynos8890_dmc_field exynos8890_dmc_fields[] = {
	DMC_PLL_FIELD("MIF_PLL"),
	DMC_MUX_FIELD("MIF0_MUX_PCLK_MIF", DMC_REGION_MIF0, 0x0210, 0x0610, 2, 4),
	DMC_MUX_FIELD("MIF1_MUX_PCLK_MIF", DMC_REGION_MIF1, 0x0210, 0x0610, 2, 4),
	DMC_MUX_FIELD("MIF2_MUX_PCLK_MIF", DMC_REGION_MIF2, 0x0210, 0x0610, 2, 4),
	DMC_MUX_FIELD("MIF3_MUX_PCLK_MIF", DMC_REGION_MIF3, 0x0210, 0x0610, 2, 4),
	DMC_MUX_FIELD("MIF0_MUX_SCLK_HPM_MIF", DMC_REGION_MIF0, 0x0214, 0x0614, 2, 4),
	DMC_MUX_FIELD("MIF1_MUX_SCLK_HPM_MIF", DMC_REGION_MIF1, 0x0214, 0x0614, 2, 4),
	DMC_MUX_FIELD("MIF2_MUX_SCLK_HPM_MIF", DMC_REGION_MIF2, 0x0214, 0x0614, 2, 4),
	DMC_MUX_FIELD("MIF3_MUX_SCLK_HPM_MIF", DMC_REGION_MIF3, 0x0214, 0x0614, 2, 4),
	DMC_MUX_FIELD("TOP_MUX_ACLK_CCORE_528", DMC_REGION_TOP, 0x024c, 0x054c, 3, 8),
	DMC_MUX_FIELD("TOP_MUX_ACLK_CCORE_264", DMC_REGION_TOP, 0x0244, 0x0544, 2, 4),
	DMC_MUX_FIELD("TOP_MUX_ACLK_CCORE_132", DMC_REGION_TOP, 0x0250, 0x0550, 2, 4),
	DMC_MUX_FIELD("TOP_MUX_PCLK_CCORE_66", DMC_REGION_TOP, 0x0254, 0x0554, 2, 4),
	DMC_MUX_FIELD("TOP_MUX_ACLK_CCORE_G3D_800", DMC_REGION_TOP, 0x0248, 0x0548, 3, 8),
	DMC_MUX_FIELD("MIF0_MUX_PCLK_SMC", DMC_REGION_MIF0, 0x0218, 0x0618, 2, 4),
	DMC_MUX_FIELD("MIF1_MUX_PCLK_SMC", DMC_REGION_MIF1, 0x0218, 0x0618, 2, 4),
	DMC_MUX_FIELD("MIF2_MUX_PCLK_SMC", DMC_REGION_MIF2, 0x0218, 0x0618, 2, 4),
	DMC_MUX_FIELD("MIF3_MUX_PCLK_SMC", DMC_REGION_MIF3, 0x0218, 0x0618, 2, 4),
	DMC_DIV_FIELD("MIF0_DIV_PCLK_MIF", DMC_REGION_MIF0, 0x0400, 3),
	DMC_DIV_FIELD("MIF1_DIV_PCLK_MIF", DMC_REGION_MIF1, 0x0400, 3),
	DMC_DIV_FIELD("MIF2_DIV_PCLK_MIF", DMC_REGION_MIF2, 0x0400, 3),
	DMC_DIV_FIELD("MIF3_DIV_PCLK_MIF", DMC_REGION_MIF3, 0x0400, 3),
	DMC_DIV_FIELD("MIF0_DIV_SCLK_HPM_MIF", DMC_REGION_MIF0, 0x0408, 2),
	DMC_DIV_FIELD("MIF1_DIV_SCLK_HPM_MIF", DMC_REGION_MIF1, 0x0408, 2),
	DMC_DIV_FIELD("MIF2_DIV_SCLK_HPM_MIF", DMC_REGION_MIF2, 0x0408, 2),
	DMC_DIV_FIELD("MIF3_DIV_SCLK_HPM_MIF", DMC_REGION_MIF3, 0x0408, 2),
	DMC_DIV_FIELD("TOP_DIV_ACLK_CCORE_528", DMC_REGION_TOP, 0x03ac, 4),
	DMC_DIV_FIELD("TOP_DIV_ACLK_CCORE_264", DMC_REGION_TOP, 0x03a4, 4),
	DMC_DIV_FIELD("TOP_DIV_ACLK_CCORE_132", DMC_REGION_TOP, 0x03b0, 4),
	DMC_DIV_FIELD("TOP_DIV_PCLK_CCORE_66", DMC_REGION_TOP, 0x03b4, 4),
	DMC_DIV_FIELD("TOP_DIV_ACLK_CCORE_G3D_800", DMC_REGION_TOP, 0x03a8, 4),
	DMC_DIV_FIELD("MIF0_DIV_PCLK_SMC", DMC_REGION_MIF0, 0x0404, 3),
	DMC_DIV_FIELD("MIF1_DIV_PCLK_SMC", DMC_REGION_MIF1, 0x0404, 3),
	DMC_DIV_FIELD("MIF2_DIV_PCLK_SMC", DMC_REGION_MIF2, 0x0404, 3),
	DMC_DIV_FIELD("MIF3_DIV_PCLK_SMC", DMC_REGION_MIF3, 0x0404, 3),
	DMC_BUS3_PLL_FIELD("BUS3_PLL"),
};

static const u16 exynos8890_dmc_top_ccore_mux_offsets[] = {
	0x0240, 0x0244, 0x0248, 0x024c, 0x0250, 0x0254,
};

struct exynos8890_mif_pll_rate {
	unsigned long rate_hz;
	u16 mdiv;
	u8 pdiv;
	u8 sdiv;
};

static const struct exynos8890_mif_pll_rate exynos8890_mif_pll_rates[] = {
	{ 3588000000UL, 207, 3, 0 }, { 3432000000UL, 198, 3, 0 },
	{ 3078400000UL, 296, 5, 0 }, { 2704000000UL, 156, 3, 0 },
	{ 2288000000UL, 132, 3, 0 }, { 2028000000UL, 117, 3, 0 },
	{ 1690000000UL, 195, 3, 1 }, { 1352000000UL, 156, 3, 1 },
	{ 1092000000UL, 126, 3, 1 }, { 841750000UL, 259, 4, 2 },
	{ 572000000UL, 132, 3, 2 }, { 416000000UL, 192, 3, 3 },
};

static const unsigned long exynos8890_bus3_pll_rates[] = {
	1872000000UL, 1352000000UL, 1092000000UL,
	841750000UL, 572000000UL, 416000000UL,
};

static bool exynos8890_dmc_bus3_rate_supported(unsigned long rate_hz)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(exynos8890_bus3_pll_rates); i++)
		if (exynos8890_bus3_pll_rates[i] == rate_hz)
			return true;
	return false;
}

struct exynos8890_dmc {
	struct device *dev;
	struct exynos8890_dmc_region regions[DMC_REGION_COUNT];
	const struct exynos8890_calib_domain *domain;
	const struct exynos8890_calib_pscdc_table *pscdc;
	u64 calibration_key;
	phys_addr_t training_pa;
	u32 *opp_voltage_uv;
	const struct exynos8890_dmc_field **member_fields;
	unsigned int pll_member;
	unsigned int bus3_member;

	struct clk *bus0_pll;
	struct clk *bus3_pll;
	struct clk *bus3_gate;
	struct clk *switch_gate;
	struct clk *pll_monitor;
	struct regulator *vdd_mif;
	struct regmap *pmu;
	struct device *apm_dev;
	struct clk_hw pll_hw;

	struct mutex lock;
	unsigned long current_rate_hz;
	int current_level;
	unsigned int timing_set;
	bool switching;
	bool indeterminate;
	bool suspended;
	bool vth_high;
	bool vth_known;
	bool bus0_retained;
	bool bus3_retained;
	bool switch_gate_enabled;
	unsigned long suspend_rate_hz;
	unsigned int suspend_timing_set;
	u32 voltage_cleanup_uv;
};

static DEFINE_MUTEX(exynos8890_dmc_owner_lock);
static struct exynos8890_dmc *exynos8890_dmc_owner;

static int exynos8890_dmc_set_ddrphy_auto(bool enable);
static int exynos8890_dmc_read_timing_set(struct exynos8890_dmc *dmc,
					  unsigned int *timing_set);
static bool exynos8890_dmc_bus_source_valid(struct exynos8890_dmc *dmc,
					     bool bus3,
					     unsigned long expected_rate);
static int exynos8890_dmc_refresh_ccf_rates(struct exynos8890_dmc *dmc);

static void __iomem *exynos8890_dmc_reg(struct exynos8890_dmc *dmc,
					u8 region, u16 offset)
{
	return dmc->regions[region].base + offset;
}

u32 exynos8890_dmc_read_addr(uintptr_t sfr)
{
	struct exynos8890_dmc *dmc = READ_ONCE(exynos8890_dmc_owner);
	unsigned int i;

	if (!dmc)
		return 0;

	for (i = 0; i < DMC_REGION_COUNT; i++) {
		const struct exynos8890_dmc_region *region = &dmc->regions[i];

		if (sfr >= region->physical &&
		    sfr - region->physical < region->size)
			return readl_relaxed(region->base + sfr - region->physical);
	}

	dev_warn_once(dmc->dev, "unowned DMC SFR read: %#lx\n",
		      (unsigned long)sfr);
	return 0;
}

void exynos8890_dmc_write_addr(uintptr_t sfr, u32 value)
{
	struct exynos8890_dmc *dmc = READ_ONCE(exynos8890_dmc_owner);
	unsigned int i;

	if (!dmc)
		return;

	for (i = 0; i < DMC_REGION_COUNT; i++) {
		const struct exynos8890_dmc_region *region = &dmc->regions[i];

		if (sfr >= region->physical &&
		    sfr - region->physical < region->size) {
			writel_relaxed(value, region->base + sfr - region->physical);
			return;
		}
	}

	dev_warn_once(dmc->dev, "unowned DMC SFR write: %#lx\n",
		      (unsigned long)sfr);
}

static const struct exynos8890_dmc_field *
exynos8890_dmc_find_field(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(exynos8890_dmc_fields); i++)
		if (!strcmp(name, exynos8890_dmc_fields[i].name))
			return &exynos8890_dmc_fields[i];

	return NULL;
}

static u64 exynos8890_dmc_level_value(struct exynos8890_dmc *dmc,
				      int level, unsigned int member)
{
	return exynos8890_calib_member_value(dmc->domain, level, member);
}

static unsigned long exynos8890_dmc_pll_rate(struct exynos8890_dmc *dmc)
{
	u32 val = readl_relaxed(exynos8890_dmc_reg(dmc, DMC_REGION_TOP,
						    TOP_MIF_CLK_CTRL0));
	u32 mdiv = FIELD_GET(MIF_PLL_MDIV, val);
	u32 pdiv = FIELD_GET(MIF_PLL_PDIV, val);
	u32 sdiv = FIELD_GET(MIF_PLL_SDIV, val);
	u64 rate = 26000000ULL;

	if (!(val & MIF_PLL_ENABLE) || !pdiv)
		return 0;

	rate *= 2 * mdiv;
	do_div(rate, pdiv << sdiv);
	return rate;
}

static int exynos8890_dmc_pll_wait_lock(struct exynos8890_dmc *dmc)
{
	u32 value;
	int channel, ret;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++) {
		ret = readl_poll_timeout_atomic(
			exynos8890_dmc_reg(dmc, DMC_REGION_MIF0 + channel,
						 MIF_PLL_CON0), value,
			value & MIF_PLL_LOCKED, 1, DMC_IO_TIMEOUT_US);
		if (ret) {
			dev_err(dmc->dev, "MIF%d PLL lock timeout: %#x\n",
				channel, value);
			return ret;
		}
	}

	return 0;
}

static int exynos8890_dmc_pll_enable(struct exynos8890_dmc *dmc)
{
	void __iomem *reg = exynos8890_dmc_reg(dmc, DMC_REGION_TOP,
						TOP_MIF_CLK_CTRL0);
	u32 value = readl_relaxed(reg);

	if (!(value & MIF_PLL_ENABLE)) {
		value |= MIF_PLL_ENABLE;
		writel_relaxed(value, reg);
	}

	return exynos8890_dmc_pll_wait_lock(dmc);
}

static void exynos8890_dmc_pll_disable(struct exynos8890_dmc *dmc)
{
	void __iomem *reg = exynos8890_dmc_reg(dmc, DMC_REGION_TOP,
						TOP_MIF_CLK_CTRL0);
	u32 value = readl_relaxed(reg);

	writel_relaxed(value & ~MIF_PLL_ENABLE, reg);
}

static int exynos8890_dmc_pll_set_rate(struct exynos8890_dmc *dmc,
				       unsigned long rate_hz)
{
	const struct exynos8890_mif_pll_rate *rate = NULL;
	void __iomem *reg;
	bool enabled;
	u32 value;
	int channel;
	unsigned int i;

	if (!rate_hz) {
		exynos8890_dmc_pll_disable(dmc);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(exynos8890_mif_pll_rates); i++)
		if (exynos8890_mif_pll_rates[i].rate_hz == rate_hz) {
			rate = &exynos8890_mif_pll_rates[i];
			break;
		}
	if (!rate)
		return -EINVAL;

	reg = exynos8890_dmc_reg(dmc, DMC_REGION_TOP, TOP_MIF_CLK_CTRL0);
	value = readl_relaxed(reg);
	enabled = value & MIF_PLL_ENABLE;
	if (enabled)
		writel_relaxed(value & ~MIF_PLL_ENABLE, reg);

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++)
		writel_relaxed(rate->pdiv * 150,
			exynos8890_dmc_reg(dmc, DMC_REGION_MIF0 + channel,
						 MIF_PLL_LOCK));

	value &= ~(MIF_PLL_MDIV | MIF_PLL_PDIV | MIF_PLL_SDIV);
	value |= FIELD_PREP(MIF_PLL_MDIV, rate->mdiv) |
		 FIELD_PREP(MIF_PLL_PDIV, rate->pdiv) |
		 FIELD_PREP(MIF_PLL_SDIV, rate->sdiv);
	writel_relaxed(value, reg);

	if (!enabled)
		return 0;
	writel_relaxed(value | MIF_PLL_ENABLE, reg);
	return exynos8890_dmc_pll_wait_lock(dmc);
}

static int exynos8890_dmc_bus3_set_rate(struct exynos8890_dmc *dmc,
					unsigned long rate_hz)
{
	int reacquire_ret;
	int ret;

	if (!exynos8890_dmc_bus3_rate_supported(rate_hz))
		return -EINVAL;

	if (dmc->bus3_retained) {
		clk_rate_exclusive_put(dmc->bus3_pll);
		dmc->bus3_retained = false;
	}
	ret = clk_set_rate_exclusive(dmc->bus3_pll, rate_hz);
	if (ret) {
		reacquire_ret = clk_rate_exclusive_get(dmc->bus3_pll);
		if (!reacquire_ret)
			dmc->bus3_retained = true;
		return ret;
	}
	dmc->bus3_retained = true;

	return clk_get_rate(dmc->bus3_pll) == rate_hz ? 0 : -EIO;
}

static u32 exynos8890_dmc_read_field(struct exynos8890_dmc *dmc,
				     const struct exynos8890_dmc_field *field)
{
	u32 value;

	if (field->kind == DMC_FIELD_PLL)
		return exynos8890_dmc_pll_rate(dmc);
	if (field->kind == DMC_FIELD_BUS3_PLL)
		return clk_get_rate(dmc->bus3_pll);

	value = readl_relaxed(exynos8890_dmc_reg(dmc, field->region,
						  field->offset));
	return (value >> field->shift) & GENMASK(field->width - 1, 0);
}

static bool exynos8890_dmc_field_matches(
		struct exynos8890_dmc *dmc,
		const struct exynos8890_dmc_field *field, u64 target)
{
	u32 mask, value;
	int channel;

	if (field->kind == DMC_FIELD_PLL) {
		if (exynos8890_dmc_pll_rate(dmc) != target)
			return false;
		if (!target)
			return true;
		for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS;
		     channel++) {
			value = readl_relaxed(exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_PLL_CON0));
			if (!(value & MIF_PLL_LOCKED))
				return false;
		}
		return true;
	}
	if (field->kind == DMC_FIELD_BUS3_PLL)
		return clk_get_rate(dmc->bus3_pll) == target;

	value = readl_relaxed(exynos8890_dmc_reg(dmc, field->region,
						  field->offset));
	mask = GENMASK(field->width - 1, 0) << field->shift;
	if ((value & mask) != target << field->shift)
		return false;

	value = readl_relaxed(exynos8890_dmc_reg(dmc, field->region,
						  field->status_offset));
	mask = GENMASK(field->status_shift + field->status_width - 1,
		       field->status_shift);
	if (field->kind == DMC_FIELD_MUX)
		return (value & mask) == BIT(field->status_shift + target);
	return !(value & mask);
}

/*
 * PSCDC owns the three distributed MIF source muxes.  The channel BUS user
 * mux is prepared once and stays on source 1; MIF_CLK_CTRL3 is the separate
 * aggregate switch usermux which vendor code returns to source 0 only after
 * a PLL-backed final transaction.
 */
static bool exynos8890_dmc_root_matches(struct exynos8890_dmc *dmc,
		u32 top_bus_selector, u32 mif_pll_selector,
		u32 top_bus_user_selector, u32 mif_aclk_selector,
		bool require_gout)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	u32 value;
	int channel;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(exynos8890_dmc_top_ccore_mux_offsets); i++)
		if (!(readl_relaxed(top +
		      exynos8890_dmc_top_ccore_mux_offsets[i]) &
		      TOP_CCORE_OUTPUT_ENABLE))
			return false;

	value = readl_relaxed(top + TOP_SWITCH_MUX_CTRL);
	if (FIELD_GET(TOP_SWITCH_MUX_SELECTOR, value) != top_bus_selector ||
	    !(value & TOP_SWITCH_MUX_GATE))
		return false;
	value = readl_relaxed(top + TOP_SWITCH_MUX_STATUS);
	if ((value & TOP_SWITCH_MUX_STATUS_BITS) != BIT(12 + top_bus_selector))
		return false;

	value = readl_relaxed(top + TOP_MIF_PLL_MUX_CTRL);
	if (FIELD_GET(TOP_MIF_MUX_SELECTOR, value) != mif_pll_selector)
		return false;
	value = readl_relaxed(top + TOP_MIF_BUS_PLL_MUX_CTRL);
	if (FIELD_GET(TOP_MIF_MUX_SELECTOR, value) != top_bus_user_selector)
		return false;
	value = readl_relaxed(top + TOP_MIF_ACLK_MUX_CTRL);
	if (FIELD_GET(TOP_MIF_MUX_SELECTOR, value) != mif_aclk_selector)
		return false;

	if (require_gout &&
	    !(readl_relaxed(top + TOP_SWITCH_GATE_CTRL) &
	      TOP_SWITCH_GATE_ENABLE))
		return false;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++) {
		void __iomem *mif = dmc->regions[DMC_REGION_MIF0 + channel].base;

		value = readl_relaxed(mif + MIF_PLL_MUX_STATUS);
		if ((value & MIF_SOURCE_MUX_STATUS) !=
		    BIT(12 + mif_pll_selector))
			return false;
		value = readl_relaxed(mif + MIF_BUS_PLL_USER_STATUS);
		if ((value & MIF_SOURCE_MUX_STATUS) !=
		    BIT(12 + top_bus_user_selector))
			return false;
		value = readl_relaxed(mif + MIF_ACLK_MUX_STATUS);
		if ((value & MIF_SOURCE_MUX_STATUS) !=
		    BIT(12 + mif_aclk_selector))
			return false;
		value = readl_relaxed(mif + MIF_BUS_PLL_USER_CTRL);
		if ((value & (MIF_BUS_PLL_USER_SELECTOR |
			      MIF_BUS_PLL_USER_ENABLE)) !=
		    (MIF_BUS_PLL_USER_SELECTOR | MIF_BUS_PLL_USER_ENABLE))
			return false;
	}

	return true;
}

static bool exynos8890_dmc_live_path_matches(struct exynos8890_dmc *dmc,
					      u32 mif_pll_selector,
					      u32 mif_aclk_selector)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	u32 value;
	int channel;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(exynos8890_dmc_top_ccore_mux_offsets); i++)
		if (!(readl_relaxed(top +
		      exynos8890_dmc_top_ccore_mux_offsets[i]) &
		      TOP_CCORE_OUTPUT_ENABLE))
			return false;

	/*
	 * The BUS0/BUS3 switch branch is dormant while DRAM runs from the MIF
	 * PLL. Vendor code selects it only as a transition begins, so its retained
	 * selector is not part of boot-path validity.
	 */
	if (FIELD_GET(TOP_MIF_MUX_SELECTOR,
		      readl_relaxed(top + TOP_MIF_PLL_MUX_CTRL)) !=
	    mif_pll_selector ||
	    FIELD_GET(TOP_MIF_MUX_SELECTOR,
		      readl_relaxed(top + TOP_MIF_ACLK_MUX_CTRL)) !=
	    mif_aclk_selector)
		return false;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++) {
		void __iomem *mif = dmc->regions[DMC_REGION_MIF0 + channel].base;

		value = readl_relaxed(mif + MIF_PLL_MUX_STATUS);
		if ((value & MIF_SOURCE_MUX_STATUS) !=
		    BIT(12 + mif_pll_selector))
			return false;
		value = readl_relaxed(mif + MIF_ACLK_MUX_STATUS);
		if ((value & MIF_SOURCE_MUX_STATUS) !=
		    BIT(12 + mif_aclk_selector))
			return false;
		value = readl_relaxed(mif + MIF_BUS_PLL_USER_CTRL);
		if ((value & (MIF_BUS_PLL_USER_SELECTOR |
			      MIF_BUS_PLL_USER_ENABLE)) !=
		    (MIF_BUS_PLL_USER_SELECTOR | MIF_BUS_PLL_USER_ENABLE))
			return false;
	}
	return true;
}

static bool exynos8890_dmc_sci_matches(struct exynos8890_dmc *dmc,
				       u32 mux, u32 div, u32 user_mux)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	void __iomem *ccore = dmc->regions[DMC_REGION_CCORE].base;
	u32 value;

	value = readl_relaxed(top + TOP_CCORE_800_MUX_CTRL);
	if (FIELD_GET(TOP_CCORE_800_MUX_SELECTOR, value) != mux ||
	    !(value & TOP_CCORE_OUTPUT_ENABLE))
		return false;
	value = readl_relaxed(top + TOP_CCORE_800_MUX_STATUS);
	if ((value & TOP_CCORE_800_MUX_STATUS_BITS) != BIT(12 + mux))
		return false;
	value = readl_relaxed(top + TOP_CCORE_800_DIV_CTRL);
	if (FIELD_GET(TOP_CCORE_800_DIV_RATIO, value) != div ||
	    (value & TOP_CCORE_800_DIV_BUSY))
		return false;

	value = readl_relaxed(ccore + CCORE_800_USER_CTRL);
	if (FIELD_GET(MIF_SOURCE_MUX_SELECTOR, value) != user_mux ||
	    !(value & CCORE_800_USER_ENABLE))
		return false;
	value = readl_relaxed(ccore + CCORE_800_USER_STATUS);
	return (value & MIF_SOURCE_MUX_STATUS) == BIT(12 + user_mux);
}

static bool exynos8890_dmc_stable_root_matches(struct exynos8890_dmc *dmc,
						int level)
{
	const struct exynos8890_calib_pscdc_entry *entry =
		&dmc->pscdc->entries[level];
	bool pll_backed = exynos8890_dmc_level_value(dmc, level,
						       dmc->pll_member);
	unsigned long bus3_rate = exynos8890_dmc_level_value(dmc, level,
							    dmc->bus3_member);

	if (!exynos8890_dmc_bus_source_valid(dmc, true, bus3_rate))
		return false;
	return exynos8890_dmc_sci_matches(dmc, entry->mux_value,
		entry->divider_ratio_minus_one, 1) &&
		exynos8890_dmc_root_matches(dmc, pll_backed ? 0 : 3,
		pll_backed ? 1 : 0, pll_backed ? 0 : 1,
		pll_backed ? 0 : 1, !pll_backed);
}

static int exynos8890_dmc_write_field(struct exynos8890_dmc *dmc,
				      const struct exynos8890_dmc_field *field,
				      u32 target)
{
	void __iomem *reg, *status;
	u32 expected, mask, value;
	int ret;

	if (field->kind == DMC_FIELD_PLL)
		return exynos8890_dmc_pll_set_rate(dmc, target);
	if (field->kind == DMC_FIELD_BUS3_PLL)
		return exynos8890_dmc_bus3_set_rate(dmc, target);
	if (target > GENMASK(field->width - 1, 0))
		return -EINVAL;

	reg = exynos8890_dmc_reg(dmc, field->region, field->offset);
	value = readl_relaxed(reg);
	value &= ~(GENMASK(field->width - 1, 0) << field->shift);
	value |= target << field->shift;
	writel_relaxed(value, reg);

	status = exynos8890_dmc_reg(dmc, field->region,
				     field->status_offset);
	if (field->kind == DMC_FIELD_MUX) {
		mask = GENMASK(field->status_shift + field->status_width - 1,
			       field->status_shift);
		expected = BIT(field->status_shift + target);
		ret = readl_poll_timeout_atomic(status, value,
						(value & mask) == expected, 1,
						DMC_CLK_TIMEOUT_US);
	} else {
		mask = GENMASK(field->status_shift + field->status_width - 1,
			       field->status_shift);
		ret = readl_poll_timeout_atomic(status, value, !(value & mask),
						1, DMC_CLK_TIMEOUT_US);
	}
	if (ret)
		dev_err(dmc->dev, "%s transition timeout: %#x\n",
			field->name, value);
	return ret;
}

enum exynos8890_dmc_transition_part {
	DMC_TRANS_HIGH,
	DMC_TRANS_LOW,
	DMC_TRANS_DIFF,
	DMC_TRANS_FORCE,
};

static bool exynos8890_dmc_should_write(u64 from, u64 to,
					enum exynos8890_dmc_transition_part part)
{
	switch (part) {
	case DMC_TRANS_HIGH:
		return from < to;
	case DMC_TRANS_LOW:
		return from > to;
	case DMC_TRANS_DIFF:
		return from != to;
	case DMC_TRANS_FORCE:
		return true;
	}
	return false;
}

static int exynos8890_dmc_transition_fields(
		struct exynos8890_dmc *dmc, int from_level, int to_level,
		enum exynos8890_calib_member_type type,
		enum exynos8890_dmc_transition_part part)
{
	unsigned int i;

	for (i = 0; i < dmc->domain->num_members; i++) {
		const struct exynos8890_dmc_field *field = dmc->member_fields[i];
		u64 from, to;
		int ret;

		if (field->type != type)
			continue;
		from = from_level >= 0 ? exynos8890_dmc_level_value(dmc,
								     from_level, i) :
			exynos8890_dmc_read_field(dmc, field);
		to = exynos8890_dmc_level_value(dmc, to_level, i);
		if (!exynos8890_dmc_should_write(from, to, part))
			continue;

		ret = exynos8890_dmc_write_field(dmc, field, to);
		if (ret) {
			dev_err(dmc->dev, "failed %s=%llu at level %d: %d\n",
				field->name, (unsigned long long)to,
				to_level, ret);
			return ret;
		}
	}

	return 0;
}

static int exynos8890_dmc_level_for_rate(struct exynos8890_dmc *dmc,
					 unsigned long rate_hz)
{
	unsigned int i;

	if (!rate_hz)
		return -EINVAL;
	for (i = 0; i < dmc->domain->num_opps; i++)
		if (rate_hz >= dmc->domain->opps[i].rate_hz)
			return i;
	return dmc->domain->num_opps;
}

static int exynos8890_dmc_exact_level(struct exynos8890_dmc *dmc,
				      unsigned long rate_hz)
{
	unsigned int i;

	for (i = 0; i < dmc->domain->num_opps; i++)
		if (dmc->domain->opps[i].rate_hz == rate_hz)
			return i;
	return -EINVAL;
}

static int exynos8890_dmc_match_leaf_level(struct exynos8890_dmc *dmc)
{
	unsigned int level, member;

	for (level = 0; level < dmc->domain->num_opps; level++) {
		for (member = 0; member < dmc->domain->num_members; member++)
			if (!exynos8890_dmc_field_matches(dmc,
				    dmc->member_fields[member],
				    exynos8890_dmc_level_value(dmc, level, member)))
				break;
		if (member == dmc->domain->num_members)
			return level;
	}
	return -EINVAL;
}

static unsigned long exynos8890_dmc_sync_rate(struct exynos8890_dmc *dmc)
{
	int level = exynos8890_dmc_match_leaf_level(dmc);

	if (level >= 0 && exynos8890_dmc_stable_root_matches(dmc, level)) {
		dmc->current_level = level;
		return dmc->domain->opps[level].rate_hz;
	}

	dmc->current_level = -1;
	return 0;
}

static u32 exynos8890_dmc_level_voltage(struct exynos8890_dmc *dmc,
					int level)
{
	if (level < 0 || level >= dmc->domain->num_opps)
		return 0;
	return dmc->opp_voltage_uv[level];
}

static int exynos8890_dmc_pscdc(struct exynos8890_dmc *dmc,
				unsigned int sci_ratio, unsigned int smc_ratio,
				u32 top_bus_mux, u32 mif_pll_mux,
				u32 mif_bus_mux, u32 mif_aclk_mux,
				u32 ccore_mux, u32 ccore_div,
				u32 ccore_user_mux, bool pause)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	u32 value;
	int channel, ret;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++)
		writel_relaxed(1, exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_PSCDC_CTRL));
	writel_relaxed(1, exynos8890_dmc_reg(dmc, DMC_REGION_CCORE,
					     MIF_PSCDC_CTRL));
	writel_relaxed(sci_ratio << 16 | smc_ratio, top + TOP_PSCDC_CTRL1);
	writel_relaxed(0x80000000 | top_bus_mux << 12,
		       top + TOP_PSCDC_SMC_FIFO0 + 0x0);
	writel_relaxed(0x80020000 | mif_pll_mux << 12,
		       top + TOP_PSCDC_SMC_FIFO0 + 0x4);
	writel_relaxed(0x80030000 | mif_bus_mux << 12,
		       top + TOP_PSCDC_SMC_FIFO0 + 0x8);
	writel_relaxed(0x80040000 | mif_aclk_mux << 12,
		       top + TOP_PSCDC_SMC_FIFO0 + 0xc);
	writel_relaxed(0, top + TOP_PSCDC_SMC_FIFO0 + 0x10);

	value = readl_relaxed(top + 0x03a0) & 0xf;
	if (value > ccore_div) {
		writel_relaxed(0x80000000 | ccore_mux << 12,
			       top + TOP_PSCDC_SCI_FIFO0 + 0x0);
		writel_relaxed(0x80010000 | ccore_div,
			       top + TOP_PSCDC_SCI_FIFO0 + 0x4);
	} else {
		writel_relaxed(0x80010000 | ccore_div,
			       top + TOP_PSCDC_SCI_FIFO0 + 0x0);
		writel_relaxed(0x80000000 | ccore_mux << 12,
			       top + TOP_PSCDC_SCI_FIFO0 + 0x4);
	}
	writel_relaxed(0x80020000 | ccore_user_mux << 12,
		       top + TOP_PSCDC_SCI_FIFO0 + 0x8);
	writel_relaxed(0, top + TOP_PSCDC_SCI_FIFO0 + 0xc);

	writel_relaxed(0x40000000 | (pause ? BIT(31) : 0),
		       top + TOP_PSCDC_CTRL0);
	ret = readl_poll_timeout_atomic(top + TOP_PSCDC_CTRL0, value,
					!(value & TOP_PSCDC_BUSY), 1,
					DMC_IO_TIMEOUT_US);
	if (ret) {
		dev_err(dmc->dev, "PSCDC timeout: %#x\n", value);
		return ret;
	}
	if (!exynos8890_dmc_root_matches(dmc, top_bus_mux, mif_pll_mux,
			mif_bus_mux, mif_aclk_mux, mif_aclk_mux)) {
		dev_err(dmc->dev,
			"PSCDC root readback mismatch (%u/%u/%u/%u)\n",
			top_bus_mux, mif_pll_mux, mif_bus_mux, mif_aclk_mux);
		return -EIO;
	}
	if (!exynos8890_dmc_sci_matches(dmc, ccore_mux, ccore_div,
			ccore_user_mux)) {
		dev_err(dmc->dev,
			"PSCDC SCI readback mismatch (%u/%u/%u)\n",
			ccore_mux, ccore_div, ccore_user_mux);
		return -EIO;
	}
	return 0;
}

static void exynos8890_dmc_channels_prepare(struct exynos8890_dmc *dmc)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	u32 value;
	int channel;

	value = readl_relaxed(top + TOP_ROOTCLKEN0);
	writel_relaxed(value & ~BIT(0), top + TOP_ROOTCLKEN0);
	value = readl_relaxed(top + TOP_ROOTCLKEN3);
	writel_relaxed(value & ~BIT(2), top + TOP_ROOTCLKEN3);
	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++)
		writel_relaxed(MIF_QCH_PREPARE,
			exynos8890_dmc_reg(dmc, DMC_REGION_MIF0 + channel,
						 MIF_QCH_CTRL));
}

static void exynos8890_dmc_channels_finish(struct exynos8890_dmc *dmc)
{
	int channel;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++)
		writel_relaxed(MIF_QCH_FINISH,
			exynos8890_dmc_reg(dmc, DMC_REGION_MIF0 + channel,
						 MIF_QCH_CTRL));
}

static int exynos8890_dmc_enable_top_ccore_outputs(
		struct exynos8890_dmc *dmc)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	void __iomem *ccore = dmc->regions[DMC_REGION_CCORE].base;
	unsigned int i;
	u32 value;

	/* These words also contain DMC-owned selectors; CCF never writes them. */
	for (i = 0; i < ARRAY_SIZE(exynos8890_dmc_top_ccore_mux_offsets); i++) {
		value = readl_relaxed(top +
				exynos8890_dmc_top_ccore_mux_offsets[i]);
		writel_relaxed(value | TOP_CCORE_OUTPUT_ENABLE, top +
			       exynos8890_dmc_top_ccore_mux_offsets[i]);
		if (!(readl_relaxed(top +
		      exynos8890_dmc_top_ccore_mux_offsets[i]) &
		      TOP_CCORE_OUTPUT_ENABLE))
			return -EIO;
	}

	value = readl_relaxed(top + TOP_SWITCH_MUX_CTRL);
	writel_relaxed(value | TOP_SWITCH_MUX_GATE,
		       top + TOP_SWITCH_MUX_CTRL);
	if (!(readl_relaxed(top + TOP_SWITCH_MUX_CTRL) &
	      TOP_SWITCH_MUX_GATE))
		return -EIO;

	value = readl_relaxed(ccore + CCORE_800_USER_CTRL);
	writel_relaxed(value | CCORE_800_USER_ENABLE,
		       ccore + CCORE_800_USER_CTRL);
	value = readl_relaxed(ccore + CCORE_800_USER_CTRL);
	if (!(value & CCORE_800_USER_ENABLE) ||
	    FIELD_GET(MIF_SOURCE_MUX_SELECTOR, value) != 1)
		return -EIO;
	value = readl_relaxed(ccore + CCORE_800_USER_STATUS);
	if ((value & MIF_SOURCE_MUX_STATUS) != BIT(13))
		return -EIO;

	return 0;
}

static int exynos8890_dmc_select_switch(struct exynos8890_dmc *dmc,
					bool enable_gate)
{
	int ret;

	/* The 0/3 BUS selector is changed only by the atomic PSCDC FIFO. */
	ret = exynos8890_dmc_enable_top_ccore_outputs(dmc);
	if (ret || !enable_gate)
		return ret;
	if (!dmc->switch_gate_enabled) {
		ret = clk_prepare_enable(dmc->switch_gate);
		if (ret)
			return ret;
		dmc->switch_gate_enabled = true;
	}

	return 0;
}

static int exynos8890_dmc_set_top_bus_usermux(
		struct exynos8890_dmc *dmc, u32 selector)
{
	void __iomem *reg = exynos8890_dmc_reg(dmc, DMC_REGION_TOP,
					       TOP_MIF_BUS_PLL_MUX_CTRL);
	u32 value;
	int channel, ret;

	value = readl_relaxed(reg);
	value &= ~TOP_MIF_MUX_SELECTOR;
	value |= FIELD_PREP(TOP_MIF_MUX_SELECTOR, selector);
	writel_relaxed(value, reg);
	ret = readl_poll_timeout_atomic(reg, value,
		FIELD_GET(TOP_MIF_MUX_SELECTOR, value) == selector,
		1, DMC_CLK_TIMEOUT_US);
	if (ret)
		dev_err(dmc->dev, "TOP MIF BUS usermux timeout: %#x\n", value);
	if (ret)
		return ret;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++) {
		void __iomem *status = exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel,
				MIF_BUS_PLL_USER_STATUS);

		ret = readl_poll_timeout_atomic(status, value,
			(value & MIF_SOURCE_MUX_STATUS) == BIT(12 + selector),
			1, DMC_CLK_TIMEOUT_US);
		if (ret) {
			dev_err(dmc->dev,
				"MIF%d BUS usermux timeout: %#x\n",
				channel, value);
			return ret;
		}
	}
	return 0;
}

static int exynos8890_dmc_retain_bus0(struct exynos8890_dmc *dmc)
{
	int ret;

	if (dmc->bus0_retained)
		return 0;
	ret = clk_rate_exclusive_get(dmc->bus0_pll);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dmc->bus0_pll);
	if (ret)
		goto err_exclusive;
	dmc->bus0_retained = true;
	if (!exynos8890_dmc_bus_source_valid(dmc, false,
					      EXYNOS8890_BUS0_PLL_HZ) ||
	    clk_get_rate(dmc->bus0_pll) != EXYNOS8890_BUS0_PLL_HZ) {
		ret = -ERANGE;
		/* A prepared MIF source is never gated on an uncertain unwind. */
		return ret;
	}
	return 0;

err_exclusive:
	clk_rate_exclusive_put(dmc->bus0_pll);
	return ret;
}

static int exynos8890_dmc_retain_bus3(struct exynos8890_dmc *dmc)
{
	unsigned long rate_hz;
	int ret;

	if (dmc->bus3_retained)
		return 0;
	ret = clk_rate_exclusive_get(dmc->bus3_pll);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dmc->bus3_gate);
	if (ret)
		goto err_exclusive;
	dmc->bus3_retained = true;
	rate_hz = clk_get_rate(dmc->bus3_pll);
	if (!exynos8890_dmc_bus3_rate_supported(rate_hz) ||
	    !exynos8890_dmc_bus_source_valid(dmc, true, rate_hz)) {
		ret = -ERANGE;
		/* BUS3 may already be the live DRAM source: retain it fail-safe. */
		return ret;
	}
	return 0;

err_exclusive:
	clk_rate_exclusive_put(dmc->bus3_pll);
	return ret;
}

static bool exynos8890_dmc_bus_source_valid(struct exynos8890_dmc *dmc,
					     bool bus3,
					     unsigned long expected_rate)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;
	u16 pll_offset = bus3 ? TOP_BUS3_PLL_CON0 : TOP_BUS0_PLL_CON0;
	u16 mux_offset = bus3 ? TOP_BUS3_PLL_MUX_CTRL :
		TOP_BUS0_PLL_MUX_CTRL;
	u16 status_offset = bus3 ? TOP_BUS3_PLL_MUX_STATUS :
		TOP_BUS0_PLL_MUX_STATUS;
	u32 mdiv, pdiv, sdiv, value;
	u64 rate;

	value = readl_relaxed(top + pll_offset);
	if ((value & (MIF_PLL_ENABLE | MIF_PLL_LOCKED)) !=
	    (MIF_PLL_ENABLE | MIF_PLL_LOCKED))
		return false;
	mdiv = FIELD_GET(MIF_PLL_MDIV, value);
	pdiv = FIELD_GET(MIF_PLL_PDIV, value);
	sdiv = FIELD_GET(MIF_PLL_SDIV, value);
	if (!pdiv)
		return false;
	rate = 26000000ULL * mdiv;
	do_div(rate, pdiv << sdiv);
	if (rate != expected_rate)
		return false;
	value = readl_relaxed(top + mux_offset);
	if (!(value & TOP_BUS_PLL_MUX_SELECTOR) ||
	    !(value & TOP_BUS_PLL_OUTPUT_ENABLE))
		return false;
	value = readl_relaxed(top + status_offset);
	if ((value & TOP_BUS_PLL_MUX_STATUS_BITS) != BIT(13))
		return false;

	return true;
}

static bool exynos8890_dmc_switch_sources_valid(
		struct exynos8890_dmc *dmc, unsigned long expected_bus3_rate)
{
	void __iomem *top = dmc->regions[DMC_REGION_TOP].base;

	return dmc->bus0_retained && dmc->bus3_retained &&
		dmc->switch_gate_enabled &&
		(readl_relaxed(top + TOP_SWITCH_GATE_CTRL) &
		 TOP_SWITCH_GATE_ENABLE) &&
		exynos8890_dmc_bus_source_valid(dmc, false,
						 EXYNOS8890_BUS0_PLL_HZ) &&
		exynos8890_dmc_bus_source_valid(dmc, true,
						 expected_bus3_rate);
}

static int exynos8890_dmc_verify_timing_set(struct exynos8890_dmc *dmc)
{
	unsigned int timing_set;
	int ret;

	ret = exynos8890_dmc_read_timing_set(dmc, &timing_set);
	if (!ret && timing_set == dmc->timing_set)
		return 0;

	dmc->indeterminate = true;
	dmc->current_rate_hz = 0;
	dev_crit(dmc->dev,
		 "DREX timing-bank selector mismatch; MIF DVFS disabled\n");
	return ret ?: -EIO;
}

static void exynos8890_dmc_release_switch_resources(
		struct exynos8890_dmc *dmc, unsigned long target_hz)
{
	int target_level = exynos8890_dmc_exact_level(dmc, target_hz);
	bool uses_bus_pll;

	/* An unknown target is fail-safe: retain every possible live source. */
	uses_bus_pll = target_level < 0 ||
		!exynos8890_dmc_level_value(dmc, target_level, dmc->pll_member);
	if (target_level < 0)
		return;
	if (dmc->switch_gate_enabled && !uses_bus_pll) {
		clk_disable_unprepare(dmc->switch_gate);
		dmc->switch_gate_enabled = false;
	}
	/*
	 * BUS0/BUS3 remain rate-exclusive and prepared for this owner's life.
	 * PSCDC changes read-only CCF muxes out of band, so permanent source
	 * residency also makes stale CCF parent-enable references harmless.
	 */
}

static int exynos8890_dmc_retain_stable_source(struct exynos8890_dmc *dmc)
{
	bool uses_bus_pll;
	int ret;

	uses_bus_pll = !exynos8890_dmc_level_value(dmc, dmc->current_level,
						  dmc->pll_member);
	/* Lease a potentially live BUS3 path before any merely preparatory source. */
	if (uses_bus_pll) {
		ret = exynos8890_dmc_retain_bus3(dmc);
		if (ret)
			return ret;
	}
	ret = exynos8890_dmc_retain_bus0(dmc);
	if (ret)
		return ret;
	if (!uses_bus_pll) {
		ret = exynos8890_dmc_retain_bus3(dmc);
		if (ret)
			return ret;
	}
	if (!uses_bus_pll || dmc->switch_gate_enabled)
		return 0;
	ret = clk_prepare_enable(dmc->switch_gate);
	if (!ret)
		dmc->switch_gate_enabled = true;
	return ret;
}

static int exynos8890_dmc_enter_switch(struct exynos8890_dmc *dmc,
				       unsigned long switch_hz)
{
	const struct exynos8890_calib_pscdc_entry *entry;
	unsigned long bus3_rate;
	unsigned long pll_rate;
	unsigned int old_timing_set;
	int from, sw, ret, restore_ret;
	u32 switch_mux;

	if (dmc->switching)
		return -EBUSY;
	if (dmc->timing_set != 0)
		return -EIO;
	from = exynos8890_dmc_level_for_rate(dmc, dmc->current_rate_hz);
	sw = exynos8890_dmc_level_for_rate(dmc, switch_hz) - 1;
	if (from < 0 || from >= dmc->domain->num_opps || sw < 0 ||
	    sw >= dmc->domain->num_opps || sw >= dmc->pscdc->num_entries)
		return -EINVAL;
	entry = &dmc->pscdc->entries[sw];
	pll_rate = exynos8890_dmc_level_value(dmc, from, dmc->pll_member);
	bus3_rate = exynos8890_dmc_level_value(dmc, from, dmc->bus3_member);

	exynos8890_dmc_channels_prepare(dmc);
	ret = exynos8890_dmc_retain_bus3(dmc);
	if (ret)
		goto fail;
	ret = exynos8890_dmc_retain_bus0(dmc);
	if (ret)
		goto fail;
	ret = exynos8890_dmc_select_switch(dmc, pll_rate != 0);
	if (ret)
		goto fail;
	if (pll_rate) {
		ret = exynos8890_dmc_set_top_bus_usermux(dmc, 1);
		if (ret)
			goto fail;
	}

	/* Vendor trans_pre: force direct DMC control for both timing sets. */
	exynos8890_dmc_write(DMC_MISC_CCORE_BASE + 0x14,
			     BIT(24) | (2 << 20));
	ret = exynos8890_dmc_transition_fields(dmc, from, sw,
		EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
		DMC_TRANS_HIGH);
	if (ret)
		goto fail;
	ret = exynos8890_dmc_transition_fields(dmc, from, sw,
		EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR, DMC_TRANS_DIFF);
	if (ret)
		goto fail;

	if (!exynos8890_dmc_switch_sources_valid(dmc, bus3_rate)) {
		ret = -ERANGE;
		goto fail;
	}
	old_timing_set = dmc->timing_set;
	dmc->timing_set ^= 1;
	ret = exynos8890_dmc_program_timing(switch_hz, dmc->timing_set);
	if (ret) {
		dmc->timing_set = old_timing_set;
		goto fail;
	}
	ret = exynos8890_dmc_verify_timing_set(dmc);
	if (ret)
		return ret;
	switch_mux = switch_hz >= EXYNOS8890_MIF_SWITCH_HIGH_HZ ? 3 : 0;
	ret = exynos8890_dmc_pscdc(dmc,
		div_u64(entry->sci_rate_hz, 1000000), switch_hz / 2000000,
		switch_mux, pll_rate ? 1 : 0, 1, 1,
		entry->mux_value, entry->divider_ratio_minus_one, 1, true);
	if (ret) {
		/* A timeout after command issue cannot prove which path is live. */
		dmc->indeterminate = true;
		dmc->current_rate_hz = 0;
		dev_crit(dmc->dev,
			 "PSCDC switch result is indeterminate; MIF DVFS disabled\n");
		return ret;
	}

	/* PSCDC has committed the live path; rollback must use leave_switch(). */
	dmc->switching = true;
	dmc->current_rate_hz = switch_hz;
	dmc->current_level = sw;
	ret = exynos8890_dmc_transition_fields(dmc, from, sw,
		EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
		DMC_TRANS_LOW);
	if (ret)
		return ret;

	exynos8890_dmc_pll_disable(dmc);
	return 0;

fail:
	/* PSCDC did not run: restore only the preparatory CCF fields. */
	restore_ret = exynos8890_dmc_transition_fields(dmc, -1, from,
		EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
		DMC_TRANS_HIGH);
	if (!restore_ret)
		restore_ret = exynos8890_dmc_transition_fields(dmc, -1, from,
			EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR, DMC_TRANS_DIFF);
	if (!restore_ret)
		restore_ret = exynos8890_dmc_transition_fields(dmc, -1, from,
			EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
			DMC_TRANS_LOW);
	if (!restore_ret && pll_rate)
		restore_ret = exynos8890_dmc_set_top_bus_usermux(dmc, 0);
	if (restore_ret) {
		dmc->indeterminate = true;
		dmc->current_rate_hz = 0;
		dev_crit(dmc->dev,
			 "pre-PSCDC restore failed; MIF state is indeterminate: %d\n",
			 restore_ret);
		return ret ?: restore_ret;
	}
	exynos8890_dmc_channels_finish(dmc);
	exynos8890_dmc_write(DMC_MISC_CCORE_BASE + 0x14, 2 << 20);
	exynos8890_dmc_release_switch_resources(dmc, dmc->current_rate_hz);
	restore_ret = exynos8890_dmc_refresh_ccf_rates(dmc);
	if (restore_ret) {
		dmc->indeterminate = true;
		dmc->current_rate_hz = 0;
		dev_crit(dmc->dev,
			 "cannot synchronize CCF after MIF restore: %d\n",
			 restore_ret);
		return ret ?: restore_ret;
	}
	return ret;
}

static int exynos8890_dmc_leave_switch(struct exynos8890_dmc *dmc,
				       unsigned long target_hz)
{
	const struct exynos8890_calib_pscdc_entry *entry;
	unsigned long bus3_rate;
	unsigned long pll_rate;
	unsigned int old_timing_set;
	bool pscdc_committed = false;
	int sw, to, ret;

	if (!dmc->switching)
		return -EINVAL;
	if (dmc->timing_set != 1)
		return -EIO;
	sw = dmc->current_level;
	to = exynos8890_dmc_exact_level(dmc, target_hz);
	if (sw < 0 || to < 0 || to >= dmc->pscdc->num_entries)
		return -EINVAL;
	entry = &dmc->pscdc->entries[to];
	pll_rate = exynos8890_dmc_level_value(dmc, to, dmc->pll_member);
	bus3_rate = exynos8890_dmc_level_value(dmc, to, dmc->bus3_member);
	if (dmc->current_rate_hz == EXYNOS8890_MIF_SWITCH_HIGH_HZ && !pll_rate)
		return -EINVAL;
	if (target_hz > EXYNOS8890_MIF_BUS3_THRESHOLD_HZ) {
		ret = exynos8890_dmc_retain_bus3(dmc);
		if (ret)
			goto fail;
	}

	ret = exynos8890_dmc_transition_fields(dmc, sw, to,
		EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ, DMC_TRANS_FORCE);
	if (ret)
		goto fail;
	if (pll_rate) {
		ret = exynos8890_dmc_pll_enable(dmc);
		if (ret)
			goto fail;
	}
	ret = exynos8890_dmc_transition_fields(dmc, -1, to,
		EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
		DMC_TRANS_HIGH);
	if (ret)
		goto fail;
	ret = exynos8890_dmc_transition_fields(dmc, -1, to,
		EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR, DMC_TRANS_DIFF);
	if (ret)
		goto fail;

	if (!exynos8890_dmc_switch_sources_valid(dmc, bus3_rate)) {
		ret = -ERANGE;
		goto fail;
	}
	old_timing_set = dmc->timing_set;
	dmc->timing_set ^= 1;
	ret = exynos8890_dmc_program_timing(target_hz, dmc->timing_set);
	if (ret) {
		dmc->timing_set = old_timing_set;
		goto fail;
	}
	ret = exynos8890_dmc_verify_timing_set(dmc);
	if (ret)
		return ret;
	ret = exynos8890_dmc_pscdc(dmc, entry->sci_ratio, entry->smc_ratio,
		pll_rate ? 0 : 3, pll_rate ? 1 : 0, 1,
		pll_rate ? 0 : 1, entry->mux_value,
		entry->divider_ratio_minus_one, 1, true);
	if (ret) {
		/* A timeout after command issue cannot prove which path is live. */
		dmc->indeterminate = true;
		dmc->current_rate_hz = 0;
		dev_crit(dmc->dev,
			 "PSCDC final result is indeterminate; MIF DVFS disabled\n");
		return ret;
	}

	/* From this point the live DRAM path is the requested stable OPP. */
	pscdc_committed = true;
	dmc->switching = false;
	dmc->current_rate_hz = target_hz;
	dmc->current_level = to;
	ret = exynos8890_dmc_transition_fields(dmc, -1, to,
		EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
		DMC_TRANS_LOW);
	if (ret)
		goto fail;
	if (pll_rate) {
		ret = exynos8890_dmc_set_top_bus_usermux(dmc, 0);
		if (ret)
			goto fail;
	}
	if (!exynos8890_dmc_stable_root_matches(dmc, to)) {
		ret = -EIO;
		dev_err(dmc->dev, "settled MIF root readback mismatch\n");
		goto fail;
	}
	if (dmc->timing_set != 0 ||
	    exynos8890_dmc_read_timing_set(dmc, &old_timing_set) ||
	    old_timing_set != 0) {
		ret = -EIO;
		dev_err(dmc->dev, "stable MIF did not return to timing bank 0\n");
		goto fail;
	}

	exynos8890_dmc_write(DMC_MISC_CCORE_BASE + 0x14, 2 << 20);
	exynos8890_dmc_channels_finish(dmc);
	exynos8890_dmc_release_switch_resources(dmc, target_hz);
	ret = exynos8890_dmc_refresh_ccf_rates(dmc);
	if (ret)
		goto fail;
	return 0;

fail:
	dev_err(dmc->dev, "MIF transition failed in switch state: %d\n", ret);
	if (!pscdc_committed)
		return ret;

	/* A post-commit failure cannot be reported as an uncommitted old rate. */
	dmc->indeterminate = true;
	dmc->current_rate_hz = 0;
	dev_crit(dmc->dev,
		 "post-PSCDC cleanup failed; MIF state is indeterminate\n");
	exynos8890_dmc_channels_finish(dmc);
	exynos8890_dmc_write(DMC_MISC_CCORE_BASE + 0x14, 2 << 20);
	return ret;
}

static int exynos8890_dmc_set_voltage(struct exynos8890_dmc *dmc, u32 uv)
{
	int ret;

	if (!uv)
		return 0;
	ret = exynos8890_dmc_set_ddrphy_auto(false);
	if (ret)
		return ret;
	ret = regulator_set_voltage(dmc->vdd_mif, uv,
				    EXYNOS8890_MIF_MAX_UV);
	if (exynos8890_dmc_set_ddrphy_auto(true) && !ret)
		ret = -EIO;
	if (ret)
		dev_err(dmc->dev, "failed to set vdd_mif to %u uV: %d\n",
			uv, ret);
	return ret;
}

static int exynos8890_dmc_finish_voltage_cleanup(
		struct exynos8890_dmc *dmc, u32 target_uv)
{
	int actual_uv, ret;

	ret = exynos8890_dmc_set_voltage(dmc, target_uv);
	if (!ret) {
		dmc->voltage_cleanup_uv = 0;
		return 0;
	}

	actual_uv = regulator_get_voltage(dmc->vdd_mif);
	if (actual_uv >= 0 && (u32)actual_uv >= target_uv) {
		dmc->voltage_cleanup_uv = target_uv;
		dev_warn(dmc->dev,
			 "MIF rate committed; vdd_mif remains safely high at %d uV\n",
			 actual_uv);
		return 0;
	}

	dmc->indeterminate = true;
	dev_crit(dmc->dev,
		 "cannot prove vdd_mif after cleanup failure (%d uV): %d\n",
		 actual_uv, ret);
	return -EIO;
}

static int exynos8890_dmc_set_vth(struct exynos8890_dmc *dmc, bool high)
{
	int ret;

	if (dmc->vth_known && dmc->vth_high == high)
		return 0;
	ret = s2mps16_regulator_set_vth_offset(dmc->vdd_mif, high);
	if (ret) {
		dmc->vth_known = false;
		dev_err(dmc->dev, "failed to set S2MPS16 MIF VTH %s: %d\n",
			high ? "high" : "low", ret);
		return ret;
	}
	dmc->vth_high = high;
	dmc->vth_known = true;
	return 0;
}

static u32 exynos8890_dmc_switch_voltage(struct exynos8890_dmc *dmc,
					 unsigned long switch_hz)
{
	unsigned int i;

	for (i = 1; i < dmc->domain->num_opps; i++)
		if (dmc->domain->opps[i].rate_hz < switch_hz)
			return exynos8890_dmc_level_voltage(dmc, i - 1);
	return 0;
}

static unsigned long exynos8890_dmc_switch_rate(unsigned long old_hz,
						unsigned long new_hz)
{
	unsigned long switch_hz = EXYNOS8890_MIF_SWITCH_LOW_HZ;

	if (old_hz > EXYNOS8890_MIF_SWITCH_HIGH_HZ ||
	    new_hz > EXYNOS8890_MIF_SWITCH_HIGH_HZ)
		switch_hz = EXYNOS8890_MIF_SWITCH_HIGH_HZ;
	if (old_hz < 845000000UL || new_hz < 845000000UL)
		switch_hz = EXYNOS8890_MIF_SWITCH_LOW_HZ;
	return switch_hz;
}

static int exynos8890_dmc_recover_rate(struct exynos8890_dmc *dmc,
					unsigned long target_hz,
					bool target_vth_high)
{
	unsigned long switch_hz;
	int ret;

	if (dmc->indeterminate)
		return -EIO;
	if (!dmc->switching) {
		if (dmc->current_rate_hz == target_hz)
			return exynos8890_dmc_set_vth(dmc, target_vth_high);
		if (exynos8890_dmc_exact_level(dmc, dmc->current_rate_hz) < 0)
			return -EIO;
		switch_hz = exynos8890_dmc_switch_rate(dmc->current_rate_hz,
						       target_hz);
		ret = exynos8890_dmc_enter_switch(dmc, switch_hz);
		if (ret && !dmc->switching)
			return ret;
	}

	/* Vendor requires high VTH before, and low VTH after, the final hop. */
	if (target_vth_high) {
		ret = exynos8890_dmc_set_vth(dmc, true);
		if (ret)
			return ret;
	}
	ret = exynos8890_dmc_leave_switch(dmc, target_hz);
	if (ret || target_vth_high)
		return ret;
	return exynos8890_dmc_set_vth(dmc, false);
}

/*
 * The regulator and both hardware phases deliberately live under this one
 * lock and API.  No external CCF consumer can observe or request the
 * intermediate state.
 */
int exynos8890_dmc_set_rate(struct exynos8890_dmc *dmc,
			    unsigned long target_rate_hz)
{
	unsigned long old_rate_hz, switch_hz;
	u32 old_uv, target_uv, switch_uv, rollback_uv;
	bool old_vth_high, target_vth_high;
	int old_level, target_level, ret, rollback_ret;

	if (!dmc)
		return -EINVAL;
	mutex_lock(&dmc->lock);
	if (dmc->suspended || dmc->indeterminate) {
		ret = -EBUSY;
		goto out;
	}
	if (dmc->voltage_cleanup_uv) {
		ret = exynos8890_dmc_finish_voltage_cleanup(dmc,
						dmc->voltage_cleanup_uv);
		if (ret)
			goto out;
	}
	old_rate_hz = dmc->current_rate_hz;
	if (old_rate_hz == target_rate_hz) {
		ret = exynos8890_dmc_set_vth(dmc,
			old_rate_hz >= EXYNOS8890_MIF_VTH_THRESHOLD_HZ);
		goto out;
	}
	old_level = exynos8890_dmc_exact_level(dmc, old_rate_hz);
	target_level = exynos8890_dmc_exact_level(dmc, target_rate_hz);
	if (old_level < 0 || target_level < 0 ||
	    !dmc->domain->opps[target_level].enabled) {
		ret = -EINVAL;
		goto out;
	}
	old_vth_high = old_rate_hz >= EXYNOS8890_MIF_VTH_THRESHOLD_HZ;
	target_vth_high = target_rate_hz >= EXYNOS8890_MIF_VTH_THRESHOLD_HZ;
	ret = exynos8890_dmc_set_vth(dmc, old_vth_high);
	if (ret)
		goto out;

	old_uv = exynos8890_dmc_level_voltage(dmc, old_level);
	target_uv = exynos8890_dmc_level_voltage(dmc, target_level);
	switch_hz = exynos8890_dmc_switch_rate(old_rate_hz, target_rate_hz);
	if (old_rate_hz <= EXYNOS8890_MIF_CMOS_THRESHOLD_HZ ||
	    target_rate_hz <= EXYNOS8890_MIF_CMOS_THRESHOLD_HZ) {
		switch_uv = exynos8890_dmc_switch_voltage(dmc,
						 EXYNOS8890_MIF_SWITCH_LOW_HZ);
		if (switch_uv)
			switch_uv += EXYNOS8890_MIF_CMOS_OFFSET_UV;
	} else if (old_rate_hz > EXYNOS8890_MIF_DIFF_THRESHOLD_HZ &&
		   target_rate_hz > EXYNOS8890_MIF_DIFF_THRESHOLD_HZ) {
		switch_uv = 0;
	} else {
		switch_uv = old_rate_hz >= target_rate_hz ? old_uv : target_uv;
	}
	if (!old_uv || !target_uv ||
	    ((old_rate_hz <= EXYNOS8890_MIF_CMOS_THRESHOLD_HZ ||
	      target_rate_hz <= EXYNOS8890_MIF_CMOS_THRESHOLD_HZ) &&
	     !switch_uv)) {
		ret = -EINVAL;
		goto out;
	}

	if (switch_uv > old_uv) {
		ret = exynos8890_dmc_set_voltage(dmc, switch_uv);
		if (ret)
			goto out;
	}
	ret = exynos8890_dmc_enter_switch(dmc, switch_hz);
	if (dmc->indeterminate)
		goto out;
	if (ret && !dmc->switching) {
		if (switch_uv > old_uv &&
		    exynos8890_dmc_finish_voltage_cleanup(dmc, old_uv))
			dev_crit(dmc->dev,
				 "vdd_mif is unverified after pre-switch error\n");
		goto out;
	}
	if (ret)
		goto rollback;
	if (!old_vth_high && target_vth_high) {
		ret = exynos8890_dmc_set_vth(dmc, true);
		if (ret)
			goto rollback;
	}
	if (switch_uv && switch_uv < old_uv) {
		ret = exynos8890_dmc_set_voltage(dmc, switch_uv);
		if (ret)
			goto rollback;
	}
	if (switch_uv < target_uv) {
		ret = exynos8890_dmc_set_voltage(dmc, target_uv);
		if (ret)
			goto rollback;
	}
	ret = exynos8890_dmc_leave_switch(dmc, target_rate_hz);
	if (dmc->indeterminate)
		goto out;
	if (ret)
		goto rollback;
	if (old_vth_high && !target_vth_high) {
		ret = exynos8890_dmc_set_vth(dmc, false);
		if (ret)
			goto rollback;
	}
	ret = exynos8890_dmc_finish_voltage_cleanup(dmc, target_uv);
	goto out;

rollback:
	/* Recover through the same complete PSCDC transaction at a safe voltage. */
	rollback_uv = max(old_uv, max(target_uv, switch_uv));
	rollback_ret = exynos8890_dmc_set_voltage(dmc, rollback_uv);
	if (rollback_ret) {
		dev_crit(dmc->dev,
			 "cannot establish safe rollback voltage %u uV: %d\n",
			 rollback_uv, rollback_ret);
		dmc->indeterminate = true;
		goto out;
	}
	rollback_ret = exynos8890_dmc_recover_rate(dmc, old_rate_hz,
						 old_vth_high);
	if (!rollback_ret) {
		rollback_ret = exynos8890_dmc_finish_voltage_cleanup(dmc, old_uv);
		if (rollback_ret)
			dev_crit(dmc->dev,
				 "MIF rate recovered but vdd_mif is unverified: %d\n",
				 rollback_ret);
	} else {
		dev_crit(dmc->dev, "failed to roll MIF back to %lu Hz: %d\n",
			 old_rate_hz, rollback_ret);
		dmc->indeterminate = true;
	}
out:
	mutex_unlock(&dmc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_set_rate);

static int exynos8890_dmc_set_ddrphy_auto(bool enable)
{
	struct exynos8890_dmc *dmc = READ_ONCE(exynos8890_dmc_owner);
	int channel;

	if (!dmc)
		return -ENODEV;
	for (channel = enable ? EXYNOS8890_DMC_CHANNELS - 1 : 0;
	     enable ? channel >= 0 : channel < EXYNOS8890_DMC_CHANNELS;
	     channel += enable ? -1 : 1) {
		if (enable) {
			writel_relaxed(0, exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_DDRPHY_GATE_MANUAL));
			writel_relaxed(0, exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_DDRPHY_GATE_VALUE));
		} else {
			writel_relaxed(1, exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_DDRPHY_GATE_VALUE));
			writel_relaxed(1, exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_DDRPHY_GATE_MANUAL));
		}
	}
	return 0;
}

struct exynos8890_dmc *exynos8890_dmc_get(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct exynos8890_dmc *dmc;

	if (!consumer || !consumer->of_node)
		return ERR_PTR(-EINVAL);
	node = of_parse_phandle(consumer->of_node, "samsung,dmc", 0);
	if (!node)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	dmc = platform_get_drvdata(pdev);
	if (!dmc) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	if (!device_link_add(consumer, dmc->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&pdev->dev);
		return ERR_PTR(-EINVAL);
	}
	return dmc;
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_get);

void exynos8890_dmc_put(struct exynos8890_dmc *dmc)
{
	if (dmc)
		put_device(dmc->dev);
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_put);

unsigned long exynos8890_dmc_get_rate(struct exynos8890_dmc *dmc)
{
	unsigned long rate = 0;

	if (!dmc)
		return 0;
	mutex_lock(&dmc->lock);
	if (!dmc->suspended && !dmc->indeterminate && !dmc->switching)
		rate = dmc->current_rate_hz;
	mutex_unlock(&dmc->lock);
	return rate;
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_get_rate);

int exynos8890_dmc_get_voltage(struct exynos8890_dmc *dmc,
			       unsigned long rate_hz, u32 *voltage_uv)
{
	int level;

	if (!dmc || !voltage_uv)
		return -EINVAL;
	level = exynos8890_dmc_exact_level(dmc, rate_hz);
	if (level < 0)
		return level;
	*voltage_uv = exynos8890_dmc_level_voltage(dmc, level);
	return *voltage_uv ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_get_voltage);

void exynos8890_dmc_dump(struct exynos8890_dmc *dmc)
{
	if (!dmc)
		return;
	dev_info(dmc->dev,
		 "rate=%lu Hz pll=%lu Hz pscdc=%#x timing-set=%u switching=%u indeterminate=%u\n",
		 dmc->current_rate_hz, exynos8890_dmc_pll_rate(dmc),
		 readl_relaxed(exynos8890_dmc_reg(dmc, DMC_REGION_TOP,
						   TOP_PSCDC_CTRL0)),
		 dmc->timing_set, dmc->switching, dmc->indeterminate);
}
EXPORT_SYMBOL_GPL(exynos8890_dmc_dump);

static int exynos8890_dmc_map_regions(struct platform_device *pdev,
				      struct exynos8890_dmc *dmc)
{
	struct resource *resource;
	unsigned int i;

	memcpy(dmc->regions, exynos8890_dmc_region_template,
	       sizeof(dmc->regions));
	for (i = 0; i < DMC_REGION_COUNT; i++) {
		resource = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						dmc->regions[i].name);
		if (!resource || resource->start != dmc->regions[i].physical ||
		    resource_size(resource) != dmc->regions[i].size)
			return dev_err_probe(&pdev->dev, -EINVAL,
				"invalid %s MMIO resource\n",
				dmc->regions[i].name);
		dmc->regions[i].base = devm_platform_ioremap_resource_byname(
			pdev, dmc->regions[i].name);
		if (IS_ERR(dmc->regions[i].base))
			return dev_err_probe(&pdev->dev,
				PTR_ERR(dmc->regions[i].base),
				"failed to map %s\n", dmc->regions[i].name);
	}
	return 0;
}

static void exynos8890_dmc_put_device(void *data)
{
	put_device(data);
}

static int exynos8890_dmc_link_apm(struct platform_device *pdev,
				   struct exynos8890_dmc *dmc)
{
	struct device_node *node;
	int ret;

	node = of_parse_phandle(pdev->dev.of_node, "samsung,apm", 0);
	if (!node)
		return -EINVAL;
	dmc->apm_dev = bus_find_device_by_of_node(&platform_bus_type, node);
	of_node_put(node);
	if (!dmc->apm_dev || !device_is_bound(dmc->apm_dev)) {
		if (dmc->apm_dev)
			put_device(dmc->apm_dev);
		dmc->apm_dev = NULL;
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(&pdev->dev,
				       exynos8890_dmc_put_device, dmc->apm_dev);
	if (ret) {
		dmc->apm_dev = NULL;
		return ret;
	}
	if (!device_link_add(&pdev->dev, dmc->apm_dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;

	return exynos8890_apm_dvfs_claim(dmc->apm_dev, true);
}

static int exynos8890_dmc_enable_channel_usermuxes(
		struct exynos8890_dmc *dmc)
{
	unsigned int channel;
	u32 value;
	int ret;

	for (channel = 0; channel < EXYNOS8890_DMC_CHANNELS; channel++) {
		void __iomem *reg = exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel, MIF_BUS_PLL_USER_CTRL);
		void __iomem *status = exynos8890_dmc_reg(dmc,
				DMC_REGION_MIF0 + channel,
				MIF_BUS_PLL_USER_STATUS);

		value = readl_relaxed(reg);
		value |= MIF_BUS_PLL_USER_SELECTOR | MIF_BUS_PLL_USER_ENABLE;
		writel_relaxed(value, reg);
		ret = readl_poll_timeout_atomic(status, value,
			(value & MIF_SOURCE_MUX_STATUS) == BIT(13),
			1, DMC_CLK_TIMEOUT_US);
		if (ret || !(readl_relaxed(reg) & MIF_BUS_PLL_USER_ENABLE)) {
			dev_err(dmc->dev,
				"MIF%u BUS root usermux did not enable: %#x\n",
				channel, value);
			return ret ?: -EIO;
		}
	}

	return 0;
}

static bool exynos8890_dmc_pll_rate_supported(u64 rate_hz)
{
	unsigned int i;

	if (!rate_hz)
		return true;
	for (i = 0; i < ARRAY_SIZE(exynos8890_mif_pll_rates); i++)
		if (exynos8890_mif_pll_rates[i].rate_hz == rate_hz)
			return true;
	return false;
}

static int exynos8890_dmc_validate_calibration(struct exynos8890_dmc *dmc)
{
	unsigned int level, member;

	if (dmc->domain->num_members != ARRAY_SIZE(exynos8890_dmc_fields)) {
		dev_err(dmc->dev, "MIF matrix has %u members, expected %zu\n",
			dmc->domain->num_members,
			ARRAY_SIZE(exynos8890_dmc_fields));
		return -EINVAL;
	}

	for (level = 0; level < dmc->domain->num_opps; level++) {
		const struct exynos8890_calib_pscdc_entry *entry =
			&dmc->pscdc->entries[level];

		if (!dmc->domain->opps[level].rate_hz ||
		    dmc->domain->opps[level].rate_hz > U32_MAX ||
		    !dmc->opp_voltage_uv[level] ||
		    dmc->opp_voltage_uv[level] > EXYNOS8890_MIF_MAX_UV) {
			dev_err(dmc->dev, "invalid MIF OPP row %u\n", level);
			return -ERANGE;
		}
		if (level && dmc->domain->opps[level - 1].rate_hz <=
			     dmc->domain->opps[level].rate_hz) {
			dev_err(dmc->dev, "MIF OPP rows are not strictly descending\n");
			return -EINVAL;
		}

		for (member = 0; member < dmc->domain->num_members; member++) {
			const struct exynos8890_dmc_field *field =
				dmc->member_fields[member];
			u64 value = exynos8890_dmc_level_value(dmc, level, member);

			if (field->kind == DMC_FIELD_PLL) {
				if (exynos8890_dmc_pll_rate_supported(value))
					continue;
			} else if (field->kind == DMC_FIELD_BUS3_PLL) {
				if (exynos8890_dmc_bus3_rate_supported(value))
					continue;
			} else if (value <= GENMASK_ULL(field->width - 1, 0)) {
				continue;
			}
			dev_err(dmc->dev,
				"MIF row %u member %s value %llu exceeds hardware encoding\n",
				level, field->name, (unsigned long long)value);
			return -ERANGE;
		}

		if (!entry->sci_rate_hz || entry->sci_rate_hz % 1000000 ||
		    entry->sci_rate_hz / 1000000 > U16_MAX ||
		    entry->mux_value > 0xf ||
		    entry->divider_ratio_minus_one > 0xf ||
		    entry->sci_ratio > U16_MAX || entry->smc_ratio > U16_MAX) {
			dev_err(dmc->dev, "PSCDC row %u exceeds hardware encoding\n",
				level);
			return -ERANGE;
		}
	}

	return 0;
}

static int exynos8890_dmc_load_calibration(struct exynos8890_dmc *dmc)
{
	const struct exynos8890_calib_mif_voltages *voltages;
	u32 key_high, key_low, training_pa;
	u64 key;
	bool bus3_found = false;
	bool pll_found = false;
	unsigned int i, j;
	int ret;

	dmc->domain = exynos8890_calib_get_domain(EXYNOS8890_CALIB_MIF);
	if (IS_ERR(dmc->domain))
		return PTR_ERR(dmc->domain);
	dmc->pscdc = exynos8890_calib_get_pscdc();
	if (IS_ERR(dmc->pscdc))
		return PTR_ERR(dmc->pscdc);
	if (dmc->pscdc->num_entries < dmc->domain->num_opps)
		return -EINVAL;
	ret = regmap_read(dmc->pmu, PMU_DREX_CALIBRATION1, &training_pa);
	if (ret)
		return ret;
	ret = regmap_read(dmc->pmu, PMU_DREX_CALIBRATION2, &key_high);
	if (ret)
		return ret;
	ret = regmap_read(dmc->pmu, PMU_DREX_CALIBRATION3, &key_low);
	if (ret)
		return ret;
	key = (u64)key_high << 32 | key_low;
	if (!key)
		return -ENODATA;
	dmc->calibration_key = key;
	dmc->training_pa = training_pa;
	voltages = exynos8890_calib_get_mif_voltages(key);
	if (IS_ERR(voltages))
		return PTR_ERR(voltages);
	if (voltages->num_opps != dmc->domain->num_opps)
		return -EINVAL;
	dmc->opp_voltage_uv = devm_kmemdup(dmc->dev, voltages->opp_voltage_uv,
					   array_size(voltages->num_opps,
						      sizeof(*dmc->opp_voltage_uv)),
					   GFP_KERNEL);
	if (!dmc->opp_voltage_uv)
		return -ENOMEM;
	if (!voltages->key)
		dev_warn(dmc->dev,
			 "DRAM-keyed MIF margin missing; using calibrated baseline\n");

	dmc->member_fields = devm_kcalloc(dmc->dev, dmc->domain->num_members,
					   sizeof(*dmc->member_fields),
					   GFP_KERNEL);
	if (!dmc->member_fields)
		return -ENOMEM;
	for (i = 0; i < dmc->domain->num_members; i++) {
		const struct exynos8890_calib_member *member =
			&dmc->domain->members[i];
		const struct exynos8890_dmc_field *field =
			exynos8890_dmc_find_field(member->name);

		if (!field || field->type != member->type) {
			dev_err(dmc->dev, "unsupported MIF member %s type %u\n",
				member->name, member->type);
			return -EINVAL;
		}
		dmc->member_fields[i] = field;
		for (j = 0; j < i; j++)
			if (dmc->member_fields[j] == field) {
				dev_err(dmc->dev, "duplicate MIF member %s\n",
					member->name);
				return -EINVAL;
			}
		if (field->kind == DMC_FIELD_PLL) {
			dmc->pll_member = i;
			pll_found = true;
		} else if (field->kind == DMC_FIELD_BUS3_PLL) {
			dmc->bus3_member = i;
			bus3_found = true;
		}
	}
	if (!pll_found || !bus3_found)
		return -EINVAL;
	return exynos8890_dmc_validate_calibration(dmc);
}

static unsigned long exynos8890_dmc_pll_recalc_rate(struct clk_hw *hw,
						     unsigned long parent_rate)
{
	struct exynos8890_dmc *dmc = container_of(hw, struct exynos8890_dmc,
						   pll_hw);

	return exynos8890_dmc_pll_rate(dmc);
}

static const struct clk_ops exynos8890_dmc_pll_monitor_ops = {
	.recalc_rate = exynos8890_dmc_pll_recalc_rate,
};

static int exynos8890_dmc_register_pll_monitor(struct exynos8890_dmc *dmc)
{
	struct clk_init_data init = {
		.name = "fout_mif_pll",
		.ops = &exynos8890_dmc_pll_monitor_ops,
		.flags = CLK_GET_RATE_NOCACHE,
	};

	int ret;

	dmc->pll_hw.init = &init;
	ret = devm_clk_hw_register(dmc->dev, &dmc->pll_hw);
	if (ret)
		return ret;
	dmc->pll_monitor = devm_clk_hw_get_clk(dmc->dev, &dmc->pll_hw,
					       "mif-pll-monitor");
	return PTR_ERR_OR_ZERO(dmc->pll_monitor);
}

static int exynos8890_dmc_refresh_ccf_rates(struct exynos8890_dmc *dmc)
{
	/* Refresh the raw PLL, then reconcile every out-of-band mux parent. */
	clk_get_rate(dmc->pll_monitor);
	return exynos8890_clk_sync_dmc();
}

static int exynos8890_dmc_read_timing_set(struct exynos8890_dmc *dmc,
					  unsigned int *timing_set)
{
	unsigned int channel, value;

	value = readl_relaxed(exynos8890_dmc_reg(dmc, DMC_REGION_MISC0,
						  DMC_MISC_TIMING_SET)) & 1;
	for (channel = 1; channel < EXYNOS8890_DMC_CHANNELS; channel++)
		if ((readl_relaxed(exynos8890_dmc_reg(dmc,
			     DMC_REGION_MISC0 + channel, DMC_MISC_TIMING_SET)) & 1) !=
		    value)
			return -EIO;
	*timing_set = value;
	return 0;
}

static int exynos8890_dmc_ensure_min_voltage(struct exynos8890_dmc *dmc,
					      int level)
{
	u32 minimum_uv = exynos8890_dmc_level_voltage(dmc, level);
	int actual_uv, ret;

	if (!minimum_uv)
		return -EINVAL;
	actual_uv = regulator_get_voltage(dmc->vdd_mif);
	if (actual_uv < 0)
		return actual_uv;
	if (actual_uv >= minimum_uv)
		return 0;

	ret = exynos8890_dmc_set_voltage(dmc, minimum_uv);
	if (ret)
		return ret;
	actual_uv = regulator_get_voltage(dmc->vdd_mif);
	if (actual_uv < 0)
		return actual_uv;
	if (actual_uv < minimum_uv) {
		dev_crit(dmc->dev,
			 "vdd_mif readback %d uV is below OPP minimum %u uV\n",
			 actual_uv, minimum_uv);
		return -ERANGE;
	}
	return 0;
}

static int exynos8890_dmc_suspend_noirq(struct device *dev)
{
	struct exynos8890_dmc *dmc = dev_get_drvdata(dev);
	unsigned long rate;

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		return -EOPNOTSUPP;
	if (!mutex_trylock(&dmc->lock))
		return -EBUSY;
	if (dmc->switching || dmc->indeterminate) {
		mutex_unlock(&dmc->lock);
		return -EBUSY;
	}
	rate = exynos8890_dmc_sync_rate(dmc);
	if (!rate) {
		mutex_unlock(&dmc->lock);
		return -EIO;
	}
	if (dmc->timing_set != 0 ||
	    exynos8890_dmc_read_timing_set(dmc, &dmc->suspend_timing_set) ||
	    dmc->suspend_timing_set != dmc->timing_set) {
		mutex_unlock(&dmc->lock);
		return -EIO;
	}
	dmc->current_rate_hz = rate;
	dmc->suspend_rate_hz = rate;
	dmc->vth_known = false;
	dmc->suspended = true;
	mutex_unlock(&dmc->lock);
	return 0;
}

static int exynos8890_dmc_resume_noirq(struct device *dev)
{
	struct exynos8890_dmc *dmc = dev_get_drvdata(dev);
	unsigned long rate;
	unsigned int timing_set;

	if (!mutex_trylock(&dmc->lock))
		return -EBUSY;
	rate = exynos8890_dmc_sync_rate(dmc);
	if (!rate || rate != dmc->suspend_rate_hz ||
	    exynos8890_dmc_read_timing_set(dmc, &timing_set) ||
	    timing_set != dmc->suspend_timing_set) {
		dev_crit(dev,
			 "s2idle changed or incompletely restored the MIF state\n");
		dmc->current_rate_hz = 0;
		dmc->indeterminate = true;
		mutex_unlock(&dmc->lock);
		return -EIO;
	}
	dmc->timing_set = timing_set;
	dmc->current_rate_hz = rate;
	mutex_unlock(&dmc->lock);
	return 0;
}

static int exynos8890_dmc_resume(struct device *dev)
{
	struct exynos8890_dmc *dmc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&dmc->lock);
	if (!dmc->current_rate_hz || dmc->indeterminate) {
		ret = -EIO;
		goto out;
	}
	ret = exynos8890_apm_dvfs_claim(dmc->apm_dev, true);
	if (ret) {
		dev_crit(dev, "cannot reclaim APM DVFS ownership: %d\n", ret);
		goto out;
	}
	ret = exynos8890_dmc_refresh_ccf_rates(dmc);
	if (ret) {
		dev_crit(dev, "cannot synchronize resumed MIF CCF state: %d\n",
			 ret);
		goto out;
	}
	ret = exynos8890_dmc_ensure_min_voltage(dmc, dmc->current_level);
	if (ret) {
		dev_crit(dev, "cannot prove resumed vdd_mif: %d\n", ret);
		goto out;
	}
	ret = exynos8890_dmc_set_vth(dmc,
		dmc->current_rate_hz >= EXYNOS8890_MIF_VTH_THRESHOLD_HZ);
	if (!ret)
		dmc->suspended = false;
out:
	mutex_unlock(&dmc->lock);
	return ret;
}

static const struct dev_pm_ops exynos8890_dmc_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(NULL, exynos8890_dmc_resume)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos8890_dmc_suspend_noirq,
				  exynos8890_dmc_resume_noirq)
};

static int exynos8890_dmc_probe(struct platform_device *pdev)
{
	struct exynos8890_dmc *dmc;
	int boot_level, ret;

	if (!exynos8890_apm_dvfs_ready())
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "APM DVFS owner is not ready\n");
	ret = exynos8890_calib_init();
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "calibration data unavailable\n");
	dmc = devm_kzalloc(&pdev->dev, sizeof(*dmc), GFP_KERNEL);
	if (!dmc)
		return -ENOMEM;
	dmc->dev = &pdev->dev;
	dmc->current_level = -1;
	mutex_init(&dmc->lock);
	dmc->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						   "samsung,pmu");
	if (IS_ERR(dmc->pmu))
		return dev_err_probe(&pdev->dev, PTR_ERR(dmc->pmu),
				     "failed to get PMU syscon\n");

	ret = exynos8890_dmc_link_apm(pdev, dmc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot claim APM DVFS ownership\n");
	ret = exynos8890_dmc_map_regions(pdev, dmc);
	if (ret)
		return ret;
	ret = exynos8890_dmc_load_calibration(dmc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid MIF calibration tables\n");
	/*
	 * These four handles can hold or control a live DRAM source. They deliberately
	 * are not devm managed: after the first successful source lease, even
	 * platform-device removal must not clk_put() and drop rate protection.
	 */
	dmc->bus0_pll = clk_get(&pdev->dev, "bus0-pll");
	if (IS_ERR(dmc->bus0_pll))
		return dev_err_probe(&pdev->dev, PTR_ERR(dmc->bus0_pll),
				     "missing BUS0 PLL\n");
	dmc->bus3_pll = clk_get(&pdev->dev, "bus3-pll");
	if (IS_ERR(dmc->bus3_pll)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(dmc->bus3_pll),
				    "missing BUS3 PLL\n");
		goto err_put_bus0;
	}
	dmc->bus3_gate = clk_get(&pdev->dev, "bus3-gate");
	if (IS_ERR(dmc->bus3_gate)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(dmc->bus3_gate),
				    "missing BUS3 output gate\n");
		goto err_put_bus3;
	}
	dmc->switch_gate = clk_get(&pdev->dev, "switch-gate");
	if (IS_ERR(dmc->switch_gate)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(dmc->switch_gate),
				    "missing MIF switch gate\n");
		goto err_put_bus3_gate;
	}
	dmc->vdd_mif = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(dmc->vdd_mif)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(dmc->vdd_mif),
				    "missing vdd_mif regulator\n");
		goto err_put_switch;
	}
	ret = exynos8890_dmc_register_pll_monitor(dmc);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret,
				    "cannot register the MIF PLL monitor\n");
		goto err_put_switch;
	}
	ret = exynos8890_dmc_refresh_ccf_rates(dmc);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret,
				    "MIF clock providers are not ready\n");
		goto err_put_switch;
	}

	mutex_lock(&exynos8890_dmc_owner_lock);
	if (exynos8890_dmc_owner) {
		mutex_unlock(&exynos8890_dmc_owner_lock);
		ret = -EBUSY;
		goto err_put_switch;
	}
	exynos8890_dmc_owner = dmc;
	mutex_unlock(&exynos8890_dmc_owner_lock);
	ret = exynos8890_dmc_enable_top_ccore_outputs(dmc);
	if (ret)
		goto err_owner;
	ret = exynos8890_dmc_enable_channel_usermuxes(dmc);
	if (ret)
		goto err_owner;

	ret = exynos8890_dmc_timing_init(dmc, dmc->calibration_key,
					 dmc->training_pa);
	if (ret)
		goto err_owner;
	ret = exynos8890_dmc_read_timing_set(dmc, &dmc->timing_set);
	if (ret) {
		dev_err(&pdev->dev, "DREX channels disagree on the timing bank\n");
		goto err_timing;
	}
	if (dmc->timing_set != 0) {
		ret = -EIO;
		dev_err(&pdev->dev, "boot MIF is not using stable timing bank 0\n");
		goto err_timing;
	}
	boot_level = exynos8890_dmc_match_leaf_level(dmc);
	if (boot_level < 0) {
		ret = -EIO;
		dev_err(&pdev->dev, "boot MIF leaf state is not calibrated\n");
		goto err_timing;
	}
	if (exynos8890_dmc_level_value(dmc, boot_level, dmc->pll_member)) {
		if (!exynos8890_dmc_live_path_matches(dmc, 1, 0)) {
			ret = -EIO;
			dev_err(&pdev->dev, "boot MIF PLL path is not live\n");
			goto err_timing;
		}
		/* dfsmif_en_list prepared BUS=1; settle the inactive branch. */
		ret = exynos8890_dmc_set_top_bus_usermux(dmc, 0);
		if (ret)
			goto err_timing;
	}
	dmc->current_level = boot_level;
	dmc->current_rate_hz = dmc->domain->opps[boot_level].rate_hz;
	dmc->current_rate_hz = exynos8890_dmc_sync_rate(dmc);
	if (!dmc->current_rate_hz) {
		ret = -EIO;
		dev_err(&pdev->dev, "boot MIF state does not match calibration\n");
		goto err_timing;
	}
	ret = exynos8890_dmc_ensure_min_voltage(dmc, dmc->current_level);
	if (ret) {
		dev_crit(&pdev->dev,
			 "cannot establish boot vdd_mif minimum: %d\n", ret);
		goto err_timing;
	}
	ret = exynos8890_dmc_set_vth(dmc,
		dmc->current_rate_hz >= EXYNOS8890_MIF_VTH_THRESHOLD_HZ);
	if (ret)
		goto err_timing;
	ret = exynos8890_dmc_refresh_ccf_rates(dmc);
	if (ret)
		goto err_timing;
	/*
	 * Leases are the final fallible setup. Once acquired, no error/remove
	 * path may gate a clock that can be the live DRAM source.
	 */
	ret = exynos8890_dmc_retain_stable_source(dmc);
	if (ret) {
		dev_crit(&pdev->dev,
			 "cannot retain the boot MIF source clock: %d\n", ret);
		if (dmc->bus0_retained || dmc->bus3_retained ||
		    dmc->switch_gate_enabled) {
			/*
			 * A successful lease must never be torn down behind live
			 * DRAM. Keep the non-unbindable owner attached but fail-stop
			 * every consumer and transition.
			 */
			dmc->indeterminate = true;
			dmc->current_rate_hz = 0;
			platform_set_drvdata(pdev, dmc);
			return 0;
		}
		goto err_timing;
	}
	platform_set_drvdata(pdev, dmc);
	dev_info(&pdev->dev, "native MIF owner initialized at %lu Hz\n",
		 dmc->current_rate_hz);
	return 0;

err_timing:
	exynos8890_dmc_timing_exit(dmc);
err_owner:
	mutex_lock(&exynos8890_dmc_owner_lock);
	if (exynos8890_dmc_owner == dmc)
		exynos8890_dmc_owner = NULL;
	mutex_unlock(&exynos8890_dmc_owner_lock);
err_put_switch:
	clk_put(dmc->switch_gate);
err_put_bus3_gate:
	clk_put(dmc->bus3_gate);
err_put_bus3:
	clk_put(dmc->bus3_pll);
err_put_bus0:
	clk_put(dmc->bus0_pll);
	return ret;
}

static void exynos8890_dmc_remove(struct platform_device *pdev)
{
	struct exynos8890_dmc *dmc = platform_get_drvdata(pdev);

	mutex_lock(&exynos8890_dmc_owner_lock);
	if (exynos8890_dmc_owner == dmc)
		exynos8890_dmc_owner = NULL;
	mutex_unlock(&exynos8890_dmc_owner_lock);
	exynos8890_dmc_timing_exit(dmc);
	/* Non-devm MIF source handles and leases intentionally outlive removal. */
}

static const struct of_device_id exynos8890_dmc_of_match[] = {
	{ .compatible = "samsung,exynos8890-dmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_dmc_of_match);

static struct platform_driver exynos8890_dmc_driver = {
	.probe = exynos8890_dmc_probe,
	.remove = exynos8890_dmc_remove,
	.driver = {
		.name = "exynos8890-dmc",
		.of_match_table = exynos8890_dmc_of_match,
		.pm = pm_sleep_ptr(&exynos8890_dmc_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos8890_dmc_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 native MIF/DMC DVFS driver");
MODULE_LICENSE("GPL");
