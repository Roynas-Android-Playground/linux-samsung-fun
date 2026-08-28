/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_CALIBRATION_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_CALIBRATION_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

/**
 * enum exynos8890_calib_domain_id - characterized Exynos8890 DVFS domains
 *
 * Rates exposed by this interface are always in Hz.  Voltages are always in
 * microvolts.  The names intentionally describe the hardware domains rather
 * than the Linux consumer drivers.
 */
enum exynos8890_calib_domain_id {
	EXYNOS8890_CALIB_MONGOOSE,
	EXYNOS8890_CALIB_APOLLO,
	EXYNOS8890_CALIB_G3D,
	EXYNOS8890_CALIB_MIF,
	EXYNOS8890_CALIB_INT,
	EXYNOS8890_CALIB_CAM,
	EXYNOS8890_CALIB_DISP,
	EXYNOS8890_CALIB_DOMAIN_COUNT,
};

/**
 * enum exynos8890_calib_member_type - meaning of a DVFS member table column
 * @EXYNOS8890_CALIB_MEMBER_RAW: unknown ECT encoding, preserved as a raw u32
 * @EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ: PLL output rate, normalized to Hz
 * @EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR: zero-based mux parent selector
 * @EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE: divider ratio minus one
 * @EXYNOS8890_CALIB_MEMBER_GATE_STATE: zero for disabled, one for enabled
 */
enum exynos8890_calib_member_type {
	EXYNOS8890_CALIB_MEMBER_RAW,
	EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ,
	EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR,
	EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE,
	EXYNOS8890_CALIB_MEMBER_GATE_STATE,
};

struct exynos8890_calib_opp {
	unsigned long rate_hz;
	u32 voltage_uv;
	bool enabled;
};

struct exynos8890_calib_member {
	const char *name;
	enum exynos8890_calib_member_type type;
};

/**
 * struct exynos8890_calib_domain - immutable characterized domain data
 * @member_values: row-major matrix indexed by OPP row, then member column
 *
 * The OPP and member rows retain their ECT order.  A consumer may copy and
 * translate the table to native CCF/OPP objects during probe, but must not
 * retain an assumption that a member name is also a globally registered CCF
 * clock name.  The generic MIF OPP voltages exclude the DRAM-keyed margin;
 * MIF consumers must use exynos8890_calib_get_mif_voltages() for final values.
 */
struct exynos8890_calib_domain {
	enum exynos8890_calib_domain_id id;
	const char *name;
	const struct exynos8890_calib_opp *opps;
	unsigned int num_opps;
	const struct exynos8890_calib_member *members;
	unsigned int num_members;
	const u64 *member_values;
	unsigned long min_rate_hz;
	unsigned long max_rate_hz;
	int boot_level;
	int resume_level;
	u8 asv_group;
	u8 asv_table_version;
};

static inline u64
exynos8890_calib_member_value(const struct exynos8890_calib_domain *domain,
			      unsigned int level, unsigned int member)
{
	if (!domain || level >= domain->num_opps || member >= domain->num_members)
		return 0;

	return domain->member_values[level * domain->num_members + member];
}

struct exynos8890_calib_switch_entry {
	unsigned long threshold_rate_hz;
	u32 mux_value;
	u32 divider_ratio_minus_one;
};

struct exynos8890_calib_ema_entry {
	u32 min_voltage_uv;
	u32 value;
	u32 assist_value;
};

struct exynos8890_calib_smpl_metadata {
	u32 register_offset;
	u32 init_mask;
	u32 init_value;
	u32 deinit_mask;
	u32 deinit_value;
	u32 trigger_mask;
	u32 trigger_value;
	u32 status_mask;
};

#define EXYNOS8890_CALIB_NO_REGISTER	U32_MAX

/**
 * struct exynos8890_calib_cpu_metadata - data required by a CPU clock owner
 *
 * Clock names are identifiers from the vendor/ECT description.  They are
 * supplied for explicit mapping to native CCF clock IDs, never for global
 * clkdev lookup.  Register offsets are relative to the corresponding CPU CMU
 * or SYSREG resource owned by the native CPU clock driver.
 */
struct exynos8890_calib_cpu_metadata {
	const char *switch_mux_name;
	const char *switch_source_mux_name;
	const char *switch_source_div_name;
	const char *switch_source_gate_name;
	const char *switch_source_usermux_name;
	u32 switch_use;
	u32 switch_notuse;
	const struct exynos8890_calib_switch_entry *switches;
	unsigned int num_switches;
	u32 ema_register_offset;
	u32 ema_assist_register_offset;
	const struct exynos8890_calib_ema_entry *ema;
	unsigned int num_ema;
	struct exynos8890_calib_smpl_metadata smpl;
};

/**
 * struct exynos8890_calib_pscdc_entry - immutable PSCDC transition row
 * @sci_rate_hz: SCI clock rate, normalized from the ECT MHz value to Hz
 * @mux_value: raw transition mux selector
 * @divider_ratio_minus_one: raw divider ratio minus one
 * @sci_ratio: raw PSCDC SCI field value
 * @smc_ratio: raw PSCDC SMC field value
 *
 * Rows are positional: entry N belongs to MIF DVFS OPP row N.  The SCI rate
 * is not a lookup key for a MIF frequency.
 */
struct exynos8890_calib_pscdc_entry {
	unsigned long sci_rate_hz;
	u32 mux_value;
	u32 divider_ratio_minus_one;
	u32 sci_ratio;
	u32 smc_ratio;
};

struct exynos8890_calib_pscdc_table {
	const struct exynos8890_calib_pscdc_entry *entries;
	unsigned int num_entries;
};

/**
 * struct exynos8890_calib_mif_timing - raw ECT DRAM timing matrix
 * @key: exact ECT timing parameter key
 * @values: row-major matrix indexed by level, then parameter
 *
 * Timing words remain raw because their bit layout belongs to the DMC/DDR PHY
 * driver.  Unlike clock rates, transforming them in this data provider would
 * destroy information.
 */
struct exynos8890_calib_mif_timing {
	u64 key;
	const u32 *values;
	unsigned int num_levels;
	unsigned int num_parameters;
};

/**
 * struct exynos8890_calib_mif_voltages - finalized keyed MIF voltages
 * @key: normalized selection key, or zero for the no-keyed-margin fallback
 * @opp_voltage_uv: one final voltage per MIF domain OPP, in domain row order
 * @num_opps: number of entries in @opp_voltage_uv
 *
 * These values include generic ASV margin, the DRAM-keyed manufacturer
 * margin, the vendor 1,000,000-uV cap and monotonic clipping, followed by SSA
 * offset/floor adjustment.  A MIF consumer must use this table instead of the
 * unkeyed voltage stored in the generic MIF domain OPP rows.  If no matching
 * keyed margin exists, the accessor returns the vendor-compatible baseline
 * (generic margin followed by SSA, without keyed cap or clipping) with key 0.
 */
struct exynos8890_calib_mif_voltages {
	u64 key;
	const u32 *opp_voltage_uv;
	unsigned int num_opps;
};

#if IS_ENABLED(CONFIG_EXYNOS8890_CALIBRATION)
int exynos8890_calib_init(void);
bool exynos8890_calib_is_ready(void);

const struct exynos8890_calib_domain *
exynos8890_calib_get_domain(enum exynos8890_calib_domain_id id);

const struct exynos8890_calib_cpu_metadata *
exynos8890_calib_get_cpu_metadata(enum exynos8890_calib_domain_id id);

const struct exynos8890_calib_pscdc_table *exynos8890_calib_get_pscdc(void);

const struct exynos8890_calib_mif_timing *
exynos8890_calib_get_mif_timing(u64 calibration_key);

const struct exynos8890_calib_mif_voltages *
exynos8890_calib_get_mif_voltages(u64 calibration_key);
#else
static inline int exynos8890_calib_init(void)
{
	return -ENODEV;
}

static inline bool exynos8890_calib_is_ready(void)
{
	return false;
}

static inline const struct exynos8890_calib_domain *
exynos8890_calib_get_domain(enum exynos8890_calib_domain_id id)
{
	return ERR_PTR(-ENODEV);
}

static inline const struct exynos8890_calib_cpu_metadata *
exynos8890_calib_get_cpu_metadata(enum exynos8890_calib_domain_id id)
{
	return ERR_PTR(-ENODEV);
}

static inline const struct exynos8890_calib_pscdc_table *
exynos8890_calib_get_pscdc(void)
{
	return ERR_PTR(-ENODEV);
}

static inline const struct exynos8890_calib_mif_timing *
exynos8890_calib_get_mif_timing(u64 calibration_key)
{
	return ERR_PTR(-ENODEV);
}

static inline const struct exynos8890_calib_mif_voltages *
exynos8890_calib_get_mif_voltages(u64 calibration_key)
{
	return ERR_PTR(-ENODEV);
}
#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_CALIBRATION_H */
