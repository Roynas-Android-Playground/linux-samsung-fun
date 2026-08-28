// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only Exynos8890 characterization data provider.
 *
 * Samsung's vendor PWRCAL code combines three unrelated jobs: parsing the
 * bootloader ECT blob, selecting per-die ASV data, and programming clocks and
 * power registers.  This file keeps only the first two jobs.  It copies the
 * selected characterization data into an immutable public representation and
 * deliberately contains no MMIO write path.
 */

#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/soc/samsung/exynos8890-calibration.h>
#include <linux/soc/samsung/exynos8890-ect.h>

#define EXYNOS8890_ASV_INFO_BASE		0x101e9000UL
#define EXYNOS8890_ASV_INFO_WORDS	5
#define EXYNOS8890_MAX_ASV_GROUPS	16
#define EXYNOS8890_MAX_LEVELS		256
#define EXYNOS8890_MAX_MEMBERS		256
#define EXYNOS8890_MAX_TIMING_TABLES	64
#define EXYNOS8890_MAX_TIMING_PARAMS	256
#define EXYNOS8890_MAX_MIF_VOLTAGE_UV	1000000U

/* Legacy Exynos register-read SMC ABI used by the shipping boot firmware. */
#define EXYNOS8890_SMC_CMD_REG		0xffffff9bU
#define EXYNOS8890_SMC_REG_CLASS_SFR_R	(0x3U << 30)
#define EXYNOS8890_SMC_REG_ID_SFR_R(addr) \
	(EXYNOS8890_SMC_REG_CLASS_SFR_R | ((u32)(addr) >> 2))

#define EXYNOS8890_KHZ_TO_HZ		1000UL
#define EXYNOS8890_MHZ_TO_HZ		1000000UL

struct exynos8890_calib_domain_desc {
	const char *ect_name;
	const char *ssa_name;
	u8 fuse_word;
	u8 fuse_shift;
};

static const struct exynos8890_calib_domain_desc domain_descs[] = {
	[EXYNOS8890_CALIB_MONGOOSE] = {
		.ect_name = "dvfs_big",
		.ssa_name = "SSA_BIG",
		.fuse_word = 0,
		.fuse_shift = 0,
	},
	[EXYNOS8890_CALIB_APOLLO] = {
		.ect_name = "dvfs_little",
		.ssa_name = "SSA_LITTLE",
		.fuse_word = 0,
		.fuse_shift = 16,
	},
	[EXYNOS8890_CALIB_G3D] = {
		.ect_name = "dvfs_g3d",
		.ssa_name = "SSA_G3D",
		.fuse_word = 1,
		.fuse_shift = 0,
	},
	[EXYNOS8890_CALIB_MIF] = {
		.ect_name = "dvfs_mif",
		.ssa_name = "SSA_MIF",
		.fuse_word = 1,
		.fuse_shift = 16,
	},
	[EXYNOS8890_CALIB_INT] = {
		.ect_name = "dvfs_int",
		.ssa_name = "SSA_INT",
		.fuse_word = 2,
		.fuse_shift = 0,
	},
	/* CAM deliberately shares the DISP ASV fuse fields on Exynos8890. */
	[EXYNOS8890_CALIB_CAM] = {
		.ect_name = "dvfs_cam",
		.ssa_name = "SSA_CAM",
		.fuse_word = 2,
		.fuse_shift = 16,
	},
	[EXYNOS8890_CALIB_DISP] = {
		.ect_name = "dvfs_disp",
		.ssa_name = "SSA_DISP",
		.fuse_word = 2,
		.fuse_shift = 16,
	},
};

struct exynos8890_calib_ssa {
	u32 subgroup_level;
	u32 floor_uv;
	u32 floor_step_uv;
	u32 offsets_uv[8];
};

struct exynos8890_calib_domain_store {
	struct exynos8890_calib_domain data;
	struct exynos8890_calib_opp *opps;
	struct exynos8890_calib_member *members;
	u64 *member_values;
	/* MIF-only intermediate data, indexed by ASV and DVFS row respectively. */
	u32 *asv_base_voltage_uv;
	unsigned int *opp_to_asv_level;
	unsigned int num_asv_levels;
	struct exynos8890_calib_ssa ssa;
};

struct exynos8890_calib_timing_store {
	struct exynos8890_calib_mif_timing data;
	u32 *values;
};

struct exynos8890_calib_mif_voltage_store {
	struct exynos8890_calib_mif_voltages data;
	u32 *opp_voltage_uv;
};

static DEFINE_MUTEX(calib_init_lock);
static struct exynos8890_calib_domain_store
	domain_stores[EXYNOS8890_CALIB_DOMAIN_COUNT];
static struct exynos8890_calib_pscdc_table pscdc_table;
static struct exynos8890_calib_pscdc_entry *pscdc_entries;
static struct exynos8890_calib_timing_store *timing_stores;
static unsigned int num_timing_stores;
static struct exynos8890_calib_mif_voltage_store *mif_voltage_stores;
static unsigned int num_mif_voltage_stores;
static u32 asv_fuses[EXYNOS8890_ASV_INFO_WORDS];
static bool calib_ready;

static const struct exynos8890_calib_switch_entry mongoose_switches[] = {
	{ 1056000000UL, 0, 0 },
	{  528000000UL, 0, 1 },
	{  352000000UL, 0, 2 },
	{  264000000UL, 0, 3 },
	{  176000000UL, 0, 5 },
	{   96000000UL, 0, 10 },
};

static const struct exynos8890_calib_switch_entry apollo_switches[] = {
	{ 1056000000UL, 0, 0 },
	{  528000000UL, 0, 1 },
	{  352000000UL, 0, 2 },
	{  264000000UL, 0, 3 },
	{  176000000UL, 0, 5 },
	{   96000000UL, 0, 10 },
};

static const struct exynos8890_calib_ema_entry mongoose_ema[] = {
	{ 1106000, 0x000e91b9, 0 },
	{  900000, 0x001091b9, 0 },
	{       0, 0x001095b9, 0 },
};

static const struct exynos8890_calib_ema_entry apollo_ema[] = {
	{ 0, 0x00000492, 0 },
};

static const struct exynos8890_calib_cpu_metadata cpu_metadata[] = {
	[EXYNOS8890_CALIB_MONGOOSE] = {
		.switch_mux_name = "MNGS_MUX_MNGS",
		.switch_source_mux_name = "TOP_MUX_SCLK_BUS_PLL_MNGS",
		.switch_source_div_name = "TOP_DIV_SCLK_BUS_PLL_MNGS",
		.switch_source_gate_name = "TOP_GATE_SCLK_BUS_PLL_MNGS",
		.switch_source_usermux_name = "MNGS_MUX_BUS_PLL_MNGS_USER",
		.switch_use = 1,
		.switch_notuse = 0,
		.switches = mongoose_switches,
		.num_switches = ARRAY_SIZE(mongoose_switches),
		.ema_register_offset = 0x0314,
		.ema_assist_register_offset = 0x1040,
		.ema = mongoose_ema,
		.num_ema = ARRAY_SIZE(mongoose_ema),
		.smpl = {
			.register_offset = 0x102c,
			.init_mask = (0x3f << 4) | 0x3,
			.init_value = (0x7 << 4) | 0x3,
			.deinit_mask = 0x3,
			.deinit_value = 0,
			.trigger_mask = 0x3 << 2,
			.trigger_value = 0x3 << 2,
			.status_mask = BIT(12),
		},
	},
	[EXYNOS8890_CALIB_APOLLO] = {
		.switch_mux_name = "APOLLO_MUX_APOLLO",
		.switch_source_mux_name = "TOP_MUX_SCLK_BUS_PLL_APOLLO",
		.switch_source_div_name = "TOP_DIV_SCLK_BUS_PLL_APOLLO",
		.switch_source_gate_name = "TOP_GATE_SCLK_BUS_PLL_APOLLO",
		.switch_source_usermux_name = "APOLLO_MUX_BUS_PLL_APOLLO_USER",
		.switch_use = 1,
		.switch_notuse = 0,
		.switches = apollo_switches,
		.num_switches = ARRAY_SIZE(apollo_switches),
		.ema_register_offset = 0x0320,
		.ema_assist_register_offset = EXYNOS8890_CALIB_NO_REGISTER,
		.ema = apollo_ema,
		.num_ema = ARRAY_SIZE(apollo_ema),
		.smpl = {
			.register_offset = 0x102c,
			.init_mask = (0x3f << 4) | 0x3,
			.init_value = (0x7 << 4) | 0x3,
			.deinit_mask = 0x3,
			.deinit_value = 0,
			.trigger_mask = 0x3 << 2,
			.trigger_value = 0x3 << 2,
			.status_mask = BIT(12),
		},
	},
};

static int exynos8890_calib_secure_read(u32 address, u32 *value)
{
	struct arm_smccc_res res;
	u32 command = EXYNOS8890_SMC_CMD_REG;
	u32 register_id = EXYNOS8890_SMC_REG_ID_SFR_R(address);

	asm volatile("dsb sy" ::: "memory");
	arm_smccc_smc(command, register_id,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return (int)res.a0;

	*value = (u32)res.a2;
	return 0;
}

static int exynos8890_calib_read_fuses(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(asv_fuses); i++) {
		ret = exynos8890_calib_secure_read(EXYNOS8890_ASV_INFO_BASE + i * 4,
						   &asv_fuses[i]);
		if (ret) {
			pr_err_once("Exynos8890 calibration: ASV fuse read at %#lx failed: %d\n",
				    EXYNOS8890_ASV_INFO_BASE + i * 4, ret);
			return ret;
		}
	}

	return 0;
}

static unsigned int exynos8890_calib_fused_table_version(void)
{
	/* Unfused samples use Samsung's documented fallback table version 3. */
	if (!(asv_fuses[3] & BIT(7)))
		return 3;

	return asv_fuses[3] & GENMASK(6, 0);
}

static int exynos8890_calib_group(enum exynos8890_calib_domain_id id)
{
	const struct exynos8890_calib_domain_desc *desc = &domain_descs[id];
	u32 value = asv_fuses[desc->fuse_word] >> desc->fuse_shift;
	int modified = sign_extend32((value >> 4) & 0xf, 3);

	return (value & 0xf) + modified;
}

static void exynos8890_calib_ssa_fuse(enum exynos8890_calib_domain_id id,
				      u32 *ssa10, u32 *ssa11, u32 *ssa0)
{
	const struct exynos8890_calib_domain_desc *desc = &domain_descs[id];
	u32 value = asv_fuses[desc->fuse_word] >> desc->fuse_shift;

	*ssa10 = (value >> 8) & 0x3;
	*ssa11 = (value >> 10) & 0x3;
	*ssa0 = (value >> 12) & 0xf;
}

static int exynos8890_calib_prepare_ect(void)
{
	struct device_node *ect_node, *mem_node;
	struct reserved_mem *rmem;
	int ret;

	if (exynos8890_ect_get_block(BLOCK_DVFS) &&
	    exynos8890_ect_get_block(BLOCK_ASV))
		return 0;

	ect_node = of_find_compatible_node(NULL, NULL, "samsung,exynos8890-ect");
	if (!ect_node)
		return -ENODEV;

	mem_node = of_parse_phandle(ect_node, "memory-region", 0);
	of_node_put(ect_node);
	if (!mem_node)
		return -EINVAL;

	rmem = of_reserved_mem_lookup(mem_node);
	of_node_put(mem_node);
	if (!rmem)
		return -EPROBE_DEFER;

	exynos8890_ect_init(rmem->base, rmem->size);
	ret = exynos8890_ect_parse_binary_header();
	if (ret)
		return ret;

	if (!exynos8890_ect_get_block(BLOCK_DVFS) ||
	    !exynos8890_ect_get_block(BLOCK_ASV))
		return -ENODATA;

	return 0;
}

static const struct ect_voltage_table *
exynos8890_calib_find_voltage_table(const struct ect_voltage_domain *domain,
				    unsigned int version)
{
	unsigned int i;

	for (i = 0; i < domain->num_of_table; i++)
		if (domain->table_list[i].table_version == version)
			return &domain->table_list[i];

	return NULL;
}

static bool
exynos8890_calib_all_have_version(struct ect_voltage_domain **domains,
				  unsigned int version)
{
	unsigned int i;

	for (i = 0; i < EXYNOS8890_CALIB_DOMAIN_COUNT; i++)
		if (!exynos8890_calib_find_voltage_table(domains[i], version))
			return false;

	return true;
}

static int
exynos8890_calib_select_table_version(struct ect_voltage_domain **domains)
{
	const struct ect_voltage_domain *first = domains[0];
	unsigned int requested = exynos8890_calib_fused_table_version();
	int selected = -1;
	unsigned int i, version;

	if (exynos8890_calib_all_have_version(domains, requested))
		return requested;

	/* Select the newest table version present in every characterized domain. */
	for (i = 0; i < first->num_of_table; i++) {
		version = first->table_list[i].table_version;
		if (version <= U8_MAX && version > selected &&
		    exynos8890_calib_all_have_version(domains, version))
			selected = version;
	}

	if (selected >= 0)
		pr_warn("Exynos8890 ASV table %u incomplete, using common table %d\n",
			requested, selected);

	return selected >= 0 ? selected : -ENODATA;
}

static enum exynos8890_calib_member_type
exynos8890_calib_member_type(const char *name)
{
	if (strstr(name, "_MUX_") || !strncmp(name, "MUX_", 4))
		return EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR;
	if (strstr(name, "_DIV_") || !strncmp(name, "DIV_", 4))
		return EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE;
	if (strstr(name, "_GATE_") || !strncmp(name, "GATE_", 5))
		return EXYNOS8890_CALIB_MEMBER_GATE_STATE;
	if (strstr(name, "_PLL"))
		return EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ;

	return EXYNOS8890_CALIB_MEMBER_RAW;
}

static int exynos8890_calib_read_ssa(enum exynos8890_calib_domain_id id,
				     unsigned int table_version,
				     struct exynos8890_calib_ssa *ssa)
{
	struct ect_gen_param_table *table;
	char *ssa_name;
	void *block;
	unsigned int row, i;

	ssa->subgroup_level = 256;
	block = exynos8890_ect_get_block(BLOCK_GEN_PARAM);
	if (!block)
		return 0;

	ssa_name = (char *)domain_descs[id].ssa_name;
	table = exynos8890_ect_gen_param_get_table(block, ssa_name);
	if (!table)
		return 0;
	if (table->num_of_row <= 0 || table->num_of_col < 12)
		return -EINVAL;

	row = min_t(unsigned int, table_version, table->num_of_row - 1);
	ssa->subgroup_level = table->parameter[row * table->num_of_col + 1];
	ssa->floor_uv = table->parameter[row * table->num_of_col + 2];
	ssa->floor_step_uv = table->parameter[row * table->num_of_col + 3];
	for (i = 0; i < ARRAY_SIZE(ssa->offsets_uv); i++)
		ssa->offsets_uv[i] =
			table->parameter[row * table->num_of_col + 4 + i];

	return 0;
}

static int
exynos8890_calib_base_voltage_uv(const struct ect_voltage_domain *domain,
				 const struct ect_voltage_table *table,
				 const struct ect_margin_domain *margin,
				 unsigned int level, unsigned int group,
				 u32 *voltage_uv)
{
	u64 voltage;
	size_t index;

	index = level * domain->num_of_group + group;
	if (table->voltages)
		voltage = table->voltages[index];
	else if (table->voltages_step)
		voltage = table->voltages_step[index] * table->volt_step;
	else
		return -ENODATA;

	if (margin) {
		if (level >= margin->num_of_level || group >= margin->num_of_group)
			return -EINVAL;
		index = level * margin->num_of_group + group;
		if (margin->offset)
			voltage += margin->offset[index];
		else if (margin->offset_compact)
			voltage += margin->offset_compact[index] * margin->volt_step;
	}

	if (voltage > U32_MAX)
		return -ERANGE;

	*voltage_uv = voltage;
	return 0;
}

static int exynos8890_calib_apply_ssa(enum exynos8890_calib_domain_id id,
				      const struct exynos8890_calib_ssa *ssa,
				      unsigned int level, u32 base_voltage_uv,
				      u32 *voltage_uv)
{
	u32 ssa10, ssa11, ssa0, offset_index;
	u64 voltage = base_voltage_uv;

	exynos8890_calib_ssa_fuse(id, &ssa10, &ssa11, &ssa0);
	offset_index = level < ssa->subgroup_level ? ssa10 : ssa11 + 4;
	voltage += ssa->offsets_uv[offset_index];
	if (ssa->floor_uv)
		voltage = max_t(u64, voltage,
				ssa->floor_uv + (u64)ssa0 * ssa->floor_step_uv);
	if (voltage > U32_MAX)
		return -ERANGE;

	*voltage_uv = voltage;
	return 0;
}

static void
exynos8890_calib_free_domain(struct exynos8890_calib_domain_store *store)
{
	unsigned int i;

	for (i = 0; i < store->data.num_members; i++)
		kfree(store->members[i].name);
	kfree(store->opp_to_asv_level);
	kfree(store->asv_base_voltage_uv);
	kfree(store->member_values);
	kfree(store->members);
	kfree(store->opps);
	memset(store, 0, sizeof(*store));
}

static int
exynos8890_calib_build_domain(enum exynos8890_calib_domain_id id,
			      struct ect_dvfs_domain *dvfs,
			      struct ect_voltage_domain *asv,
			      unsigned int table_version)
{
	struct exynos8890_calib_domain_store *store = &domain_stores[id];
	const struct ect_voltage_table *voltage_table;
	struct ect_margin_domain *margin = NULL;
	struct exynos8890_calib_ssa ssa = { };
	char *ect_name = (char *)domain_descs[id].ect_name;
	void *margin_block;
	unsigned long min_rate = ULONG_MAX, max_rate = 0;
	unsigned int level, member;
	unsigned int asv_level;
	int group, ret;
	size_t num_values;

	if (dvfs->num_of_level <= 0 || dvfs->num_of_level > EXYNOS8890_MAX_LEVELS ||
	    dvfs->num_of_clock < 0 || dvfs->num_of_clock > EXYNOS8890_MAX_MEMBERS ||
	    asv->num_of_level <= 0 || asv->num_of_level > EXYNOS8890_MAX_LEVELS ||
	    asv->num_of_group <= 0 ||
	    asv->num_of_group > EXYNOS8890_MAX_ASV_GROUPS)
		return -EINVAL;
	/* Vendor PWRCAL pairs each DFS row with the same ASV row index. */
	if (dvfs->num_of_level > asv->num_of_level) {
		pr_err_once("Exynos8890 calibration: %s has %d DFS rows but only %d ASV rows\n",
			    ect_name, dvfs->num_of_level, asv->num_of_level);
		return -EINVAL;
	}
	if (check_mul_overflow((size_t)dvfs->num_of_level,
			       (size_t)dvfs->num_of_clock, &num_values))
		return -EOVERFLOW;

	voltage_table = exynos8890_calib_find_voltage_table(asv, table_version);
	if (!voltage_table)
		return -ENODATA;

	group = exynos8890_calib_group(id);
	if (group < 0 || group >= asv->num_of_group)
		return -ERANGE;

	ret = exynos8890_calib_read_ssa(id, table_version, &ssa);
	if (ret)
		return ret;

	margin_block = exynos8890_ect_get_block(BLOCK_MARGIN);
	if (margin_block)
		margin = exynos8890_ect_margin_get_domain(margin_block, ect_name);

	store->opps = kcalloc(dvfs->num_of_level, sizeof(*store->opps),
			      GFP_KERNEL);
	store->members = kcalloc(dvfs->num_of_clock, sizeof(*store->members),
				 GFP_KERNEL);
	store->member_values = kcalloc(num_values,
				       sizeof(*store->member_values), GFP_KERNEL);
	if (!store->opps ||
	    (dvfs->num_of_clock && (!store->members || !store->member_values))) {
		ret = -ENOMEM;
		goto err_free;
	}
	if (id == EXYNOS8890_CALIB_MIF) {
		store->asv_base_voltage_uv =
			kcalloc(asv->num_of_level,
				sizeof(*store->asv_base_voltage_uv), GFP_KERNEL);
		store->opp_to_asv_level =
			kcalloc(dvfs->num_of_level,
				sizeof(*store->opp_to_asv_level), GFP_KERNEL);
		if (!store->asv_base_voltage_uv || !store->opp_to_asv_level) {
			ret = -ENOMEM;
			goto err_free;
		}
		store->num_asv_levels = asv->num_of_level;
		store->ssa = ssa;
		for (level = 0; level < asv->num_of_level; level++) {
			u32 *base_voltage = &store->asv_base_voltage_uv[level];

			if (level && asv->level_list[level] >=
			    asv->level_list[level - 1]) {
				ret = -EINVAL;
				goto err_free;
			}
			ret = exynos8890_calib_base_voltage_uv(asv, voltage_table,
							       margin, level, group,
							       base_voltage);
			if (ret)
				goto err_free;
		}
	}

	store->data.id = id;
	store->data.name = domain_descs[id].ect_name;
	store->data.opps = store->opps;
	store->data.num_opps = dvfs->num_of_level;
	store->data.members = store->members;
	store->data.num_members = dvfs->num_of_clock;
	store->data.member_values = store->member_values;
	store->data.boot_level = dvfs->boot_level_idx;
	store->data.resume_level = dvfs->resume_level_idx;
	store->data.asv_group = group;
	store->data.asv_table_version = table_version;

	for (member = 0; member < dvfs->num_of_clock; member++) {
		store->members[member].name = kstrdup(dvfs->list_clock[member],
						      GFP_KERNEL);
		if (!store->members[member].name) {
			ret = -ENOMEM;
			goto err_free;
		}
		store->members[member].type =
			exynos8890_calib_member_type(dvfs->list_clock[member]);
	}

	for (level = 0; level < dvfs->num_of_level; level++) {
		struct exynos8890_calib_opp *opp = &store->opps[level];
		u64 rate_hz = (u64)dvfs->list_level[level].level *
			      EXYNOS8890_KHZ_TO_HZ;
		u32 base_voltage_uv = 0;
		bool asv_enabled;

		if (rate_hz > ULONG_MAX) {
			ret = -ERANGE;
			goto err_free;
		}
		opp->rate_hz = rate_hz;
		asv_level = level;
		if (asv->level_list[asv_level] !=
		    dvfs->list_level[level].level)
			pr_warn_once("Exynos8890 calibration: %s row %u rates differ (%u/%u kHz); using vendor row mapping\n",
				     ect_name, level,
				     dvfs->list_level[level].level,
				     asv->level_list[asv_level]);

		if (id == EXYNOS8890_CALIB_MIF) {
			store->opp_to_asv_level[level] = asv_level;
			base_voltage_uv = store->asv_base_voltage_uv[asv_level];
		} else {
			ret = exynos8890_calib_base_voltage_uv(asv, voltage_table,
							       margin, asv_level,
							       group, &base_voltage_uv);
			if (ret)
				goto err_free;
		}
		ret = exynos8890_calib_apply_ssa(id, &ssa, asv_level,
						 base_voltage_uv,
						 &opp->voltage_uv);
		if (ret)
			goto err_free;

		asv_enabled = !voltage_table->level_en ||
			      voltage_table->level_en[asv_level];
		opp->enabled = dvfs->list_level[level].level_en && asv_enabled &&
			(!dvfs->max_frequency ||
			 dvfs->list_level[level].level <= dvfs->max_frequency) &&
			(!dvfs->min_frequency ||
			 dvfs->list_level[level].level >= dvfs->min_frequency);
		if (opp->enabled) {
			min_rate = min(min_rate, opp->rate_hz);
			max_rate = max(max_rate, opp->rate_hz);
		}

		for (member = 0; member < dvfs->num_of_clock; member++) {
			u64 value;

			value = dvfs->list_dvfs_value
				[level * dvfs->num_of_clock + member];

			if (store->members[member].type ==
			    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ)
				value *= EXYNOS8890_KHZ_TO_HZ;
			store->member_values[level * dvfs->num_of_clock + member] =
				value;
		}
	}
	if (store->data.boot_level < -1 ||
	    (store->data.boot_level >= 0 &&
	     (unsigned int)store->data.boot_level >= store->data.num_opps) ||
	    store->data.resume_level < -1 ||
	    (store->data.resume_level >= 0 &&
	     (unsigned int)store->data.resume_level >= store->data.num_opps) ||
	    (store->data.boot_level >= 0 &&
	     !store->opps[store->data.boot_level].enabled) ||
	    (store->data.resume_level >= 0 &&
	     !store->opps[store->data.resume_level].enabled)) {
		ret = -EINVAL;
		goto err_free;
	}

	if (!max_rate) {
		ret = -ENODATA;
		goto err_free;
	}
	store->data.min_rate_hz = min_rate;
	store->data.max_rate_hz = max_rate;
	return 0;

err_free:
	exynos8890_calib_free_domain(store);
	return ret;
}

static int exynos8890_calib_build_domains(void)
{
	struct ect_voltage_domain *asv_domains[EXYNOS8890_CALIB_DOMAIN_COUNT];
	struct ect_dvfs_domain *dvfs_domains[EXYNOS8890_CALIB_DOMAIN_COUNT];
	void *dvfs_block = exynos8890_ect_get_block(BLOCK_DVFS);
	void *asv_block = exynos8890_ect_get_block(BLOCK_ASV);
	struct ect_voltage_domain *asv;
	struct ect_dvfs_domain *dvfs;
	char *ect_name;
	unsigned int id;
	int version, ret;

	for (id = 0; id < EXYNOS8890_CALIB_DOMAIN_COUNT; id++) {
		ect_name = (char *)domain_descs[id].ect_name;
		dvfs_domains[id] =
			exynos8890_ect_dvfs_get_domain(dvfs_block, ect_name);
		asv_domains[id] =
			exynos8890_ect_asv_get_domain(asv_block, ect_name);
		if (!dvfs_domains[id] || !asv_domains[id])
			return -ENODATA;
		if (asv_domains[id]->num_of_table <= 0)
			return -ENODATA;
	}

	version = exynos8890_calib_select_table_version(asv_domains);
	if (version < 0)
		return version;

	for (id = 0; id < EXYNOS8890_CALIB_DOMAIN_COUNT; id++) {
		dvfs = dvfs_domains[id];
		asv = asv_domains[id];
		ret = exynos8890_calib_build_domain(id, dvfs, asv, version);
		if (ret) {
			pr_err_once("Exynos8890 calibration: domain %s build failed: %d\n",
				    domain_descs[id].ect_name, ret);
			return ret;
		}
	}

	return 0;
}

static int exynos8890_calib_build_pscdc(void)
{
	struct ect_gen_param_table *table;
	void *block = exynos8890_ect_get_block(BLOCK_GEN_PARAM);
	unsigned int i;
	u64 rate_hz;

	if (!block)
		return -ENODATA;
	table = exynos8890_ect_gen_param_get_table(block, "PSCDC");
	if (!table)
		return -ENODATA;
	if (table->num_of_row <= 0 || table->num_of_row > EXYNOS8890_MAX_LEVELS ||
	    table->num_of_col != 5 ||
	    table->num_of_row !=
		(int)domain_stores[EXYNOS8890_CALIB_MIF].data.num_opps)
		return -EINVAL;

	pscdc_entries = kcalloc(table->num_of_row, sizeof(*pscdc_entries),
				GFP_KERNEL);
	if (!pscdc_entries)
		return -ENOMEM;

	for (i = 0; i < table->num_of_row; i++) {
		rate_hz = (u64)table->parameter[i * table->num_of_col] *
			  EXYNOS8890_MHZ_TO_HZ;
		if (rate_hz > ULONG_MAX)
			return -ERANGE;
		pscdc_entries[i].sci_rate_hz = rate_hz;
		pscdc_entries[i].mux_value =
			table->parameter[i * table->num_of_col + 1];
		pscdc_entries[i].divider_ratio_minus_one =
			table->parameter[i * table->num_of_col + 2];
		pscdc_entries[i].sci_ratio =
			table->parameter[i * table->num_of_col + 3];
		pscdc_entries[i].smc_ratio =
			table->parameter[i * table->num_of_col + 4];
	}

	pscdc_table.entries = pscdc_entries;
	pscdc_table.num_entries = table->num_of_row;
	return 0;
}

static int exynos8890_calib_build_timings(void)
{
	struct ect_timing_param_header *header;
	void *block = exynos8890_ect_get_block(BLOCK_TIMING_PARAM);
	unsigned int i;

	if (!block)
		return -ENODATA;
	header = block;
	if (header->num_of_size <= 0 ||
	    header->num_of_size > EXYNOS8890_MAX_TIMING_TABLES)
		return -EINVAL;

	timing_stores = kcalloc(header->num_of_size, sizeof(*timing_stores),
				GFP_KERNEL);
	if (!timing_stores)
		return -ENOMEM;
	num_timing_stores = header->num_of_size;

	for (i = 0; i < num_timing_stores; i++) {
		struct ect_timing_param_size *source = &header->size_list[i];
		struct exynos8890_calib_timing_store *store = &timing_stores[i];
		size_t count;

		if (source->num_of_level <= 0 ||
		    source->num_of_level > EXYNOS8890_MAX_LEVELS ||
		    source->num_of_timing_param <= 0 ||
		    source->num_of_timing_param > EXYNOS8890_MAX_TIMING_PARAMS ||
		    check_mul_overflow((size_t)source->num_of_level,
				       (size_t)source->num_of_timing_param, &count))
			return -EINVAL;

		store->values = kmemdup_array(source->timing_parameter, count,
					      sizeof(*store->values), GFP_KERNEL);
		if (!store->values)
			return -ENOMEM;
		store->data.key = source->parameter_key;
		store->data.values = store->values;
		store->data.num_levels = source->num_of_level;
		store->data.num_parameters = source->num_of_timing_param;
	}

	return 0;
}

static int exynos8890_calib_build_mif_voltage(unsigned int store_index,
					      unsigned int timing_index)
{
	struct exynos8890_calib_domain_store *mif =
		&domain_stores[EXYNOS8890_CALIB_MIF];
	struct exynos8890_calib_mif_voltage_store *voltage_store =
		&mif_voltage_stores[store_index];
	const struct exynos8890_calib_timing_store *margin_store =
		&timing_stores[timing_index];
	u32 *asv_voltage_uv;
	unsigned int level;
	size_t count;
	int ret;

	count = (size_t)margin_store->data.num_levels *
		margin_store->data.num_parameters;
	/* Vendor PWRCAL consumes the first ASV-row values of this flat payload. */
	if (count < mif->num_asv_levels)
		return -EINVAL;

	asv_voltage_uv = kcalloc(mif->num_asv_levels,
				 sizeof(*asv_voltage_uv), GFP_KERNEL);
	voltage_store->opp_voltage_uv =
		kcalloc(mif->data.num_opps,
			sizeof(*voltage_store->opp_voltage_uv), GFP_KERNEL);
	if (!asv_voltage_uv || !voltage_store->opp_voltage_uv) {
		ret = -ENOMEM;
		goto out_free_asv;
	}

	/* Match vendor order: keyed margin and cap precede monotonic clipping. */
	for (level = 0; level < mif->num_asv_levels; level++) {
		u64 voltage = (u64)mif->asv_base_voltage_uv[level] +
			margin_store->values[level];

		voltage = min_t(u64, voltage, EXYNOS8890_MAX_MIF_VOLTAGE_UV);
		asv_voltage_uv[level] = voltage;
	}
	for (level = 1; level < mif->num_asv_levels; level++) {
		u32 previous_voltage_uv = asv_voltage_uv[level - 1];

		asv_voltage_uv[level] = min(previous_voltage_uv,
					    asv_voltage_uv[level]);
	}

	/* SSA is indexed by ASV row, then mapped back to each DVFS OPP row. */
	for (level = 0; level < mif->data.num_opps; level++) {
		unsigned int asv_level = mif->opp_to_asv_level[level];

		if (asv_level >= mif->num_asv_levels) {
			ret = -EINVAL;
			goto out_free_asv;
		}
		ret = exynos8890_calib_apply_ssa(EXYNOS8890_CALIB_MIF,
						 &mif->ssa, asv_level,
						 asv_voltage_uv[asv_level],
						 &voltage_store->opp_voltage_uv[level]);
		if (ret)
			goto out_free_asv;
	}

	voltage_store->data.key = margin_store->data.key;
	voltage_store->data.opp_voltage_uv = voltage_store->opp_voltage_uv;
	voltage_store->data.num_opps = mif->data.num_opps;
	ret = 0;

out_free_asv:
	kfree(asv_voltage_uv);
	if (ret) {
		kfree(voltage_store->opp_voltage_uv);
		voltage_store->opp_voltage_uv = NULL;
	}
	return ret;
}

static int exynos8890_calib_build_mif_voltages(void)
{
	unsigned int i, j, num_stores = 0, store_index = 0;
	struct exynos8890_calib_domain_store *mif =
		&domain_stores[EXYNOS8890_CALIB_MIF];
	struct exynos8890_calib_mif_voltage_store *fallback;
	int ret;

	for (i = 0; i < num_timing_stores; i++) {
		size_t count;

		if ((timing_stores[i].data.key & 0xff) != 0x3)
			continue;
		count = (size_t)timing_stores[i].data.num_levels *
			timing_stores[i].data.num_parameters;
		if (count >= mif->num_asv_levels)
			num_stores++;
	}
	/* One extra row is the vendor-compatible no-keyed-margin fallback. */
	mif_voltage_stores = kcalloc(num_stores + 1,
				     sizeof(*mif_voltage_stores), GFP_KERNEL);
	if (!mif_voltage_stores)
		return -ENOMEM;

	for (i = 0; i < num_timing_stores; i++) {
		size_t count;
		bool duplicate = false;

		if ((timing_stores[i].data.key & 0xff) != 0x3)
			continue;
		count = (size_t)timing_stores[i].data.num_levels *
			timing_stores[i].data.num_parameters;
		if (count < mif->num_asv_levels) {
			pr_warn("Exynos8890 calibration: ignoring short MIF margin key %#llx (%zu values, need %u)\n",
				(unsigned long long)timing_stores[i].data.key,
				count, mif->num_asv_levels);
			continue;
		}
		for (j = 0; j < store_index; j++) {
			if (mif_voltage_stores[j].data.key ==
			    timing_stores[i].data.key) {
				duplicate = true;
				break;
			}
		}
		if (duplicate) {
			pr_warn("Exynos8890 calibration: ignoring duplicate MIF margin key %#llx\n",
				(unsigned long long)timing_stores[i].data.key);
			continue;
		}
		ret = exynos8890_calib_build_mif_voltage(store_index, i);
		if (ret)
			return ret;
		store_index++;
		num_mif_voltage_stores = store_index;
	}

	fallback = &mif_voltage_stores[store_index];
	fallback->opp_voltage_uv =
		kcalloc(mif->data.num_opps, sizeof(*fallback->opp_voltage_uv),
			GFP_KERNEL);
	if (!fallback->opp_voltage_uv)
		return -ENOMEM;
	for (i = 0; i < mif->data.num_opps; i++)
		fallback->opp_voltage_uv[i] = mif->data.opps[i].voltage_uv;
	fallback->data.key = 0;
	fallback->data.opp_voltage_uv = fallback->opp_voltage_uv;
	fallback->data.num_opps = mif->data.num_opps;
	num_mif_voltage_stores = store_index + 1;

	return 0;
}

static void exynos8890_calib_free_cache(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(domain_stores); i++)
		exynos8890_calib_free_domain(&domain_stores[i]);
	for (i = 0; i < num_timing_stores; i++)
		kfree(timing_stores[i].values);
	for (i = 0; i < num_mif_voltage_stores; i++)
		kfree(mif_voltage_stores[i].opp_voltage_uv);
	kfree(mif_voltage_stores);
	mif_voltage_stores = NULL;
	num_mif_voltage_stores = 0;
	kfree(timing_stores);
	timing_stores = NULL;
	num_timing_stores = 0;
	kfree(pscdc_entries);
	pscdc_entries = NULL;
	memset(&pscdc_table, 0, sizeof(pscdc_table));
}

int exynos8890_calib_init(void)
{
	int ret;

	/* Pairs with the release after every cache pointer is published. */
	if (smp_load_acquire(&calib_ready))
		return 0;

	mutex_lock(&calib_init_lock);
	if (calib_ready) {
		ret = 0;
		goto out_unlock;
	}

	ret = exynos8890_calib_prepare_ect();
	if (ret) {
		pr_err_once("Exynos8890 calibration: ECT preparation failed: %d\n",
			    ret);
		goto out_unlock;
	}
	ret = exynos8890_calib_read_fuses();
	if (ret)
		goto out_unlock;
	ret = exynos8890_calib_build_domains();
	if (ret)
		goto err_free;
	ret = exynos8890_calib_build_pscdc();
	if (ret) {
		pr_err_once("Exynos8890 calibration: PSCDC build failed: %d\n",
			    ret);
		goto err_free;
	}
	ret = exynos8890_calib_build_timings();
	if (ret) {
		pr_err_once("Exynos8890 calibration: timing build failed: %d\n",
			    ret);
		goto err_free;
	}
	ret = exynos8890_calib_build_mif_voltages();
	if (ret) {
		pr_err_once("Exynos8890 calibration: MIF voltage build failed: %d\n",
			    ret);
		goto err_free;
	}

	/* Publish the fully populated immutable cache as one unit. */
	smp_store_release(&calib_ready, true);
	pr_info("Exynos8890 calibration: ASV table %u, read-only data ready\n",
		domain_stores[0].data.asv_table_version);
	goto out_unlock;

err_free:
	exynos8890_calib_free_cache();
out_unlock:
	mutex_unlock(&calib_init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_calib_init);

bool exynos8890_calib_is_ready(void)
{
	/* Pairs with the release at the successful end of initialization. */
	return smp_load_acquire(&calib_ready);
}
EXPORT_SYMBOL_GPL(exynos8890_calib_is_ready);

const struct exynos8890_calib_domain *
exynos8890_calib_get_domain(enum exynos8890_calib_domain_id id)
{
	if ((unsigned int)id >= EXYNOS8890_CALIB_DOMAIN_COUNT)
		return ERR_PTR(-EINVAL);
	if (!exynos8890_calib_is_ready())
		return ERR_PTR(-EPROBE_DEFER);

	return &domain_stores[id].data;
}
EXPORT_SYMBOL_GPL(exynos8890_calib_get_domain);

const struct exynos8890_calib_cpu_metadata *
exynos8890_calib_get_cpu_metadata(enum exynos8890_calib_domain_id id)
{
	if (id != EXYNOS8890_CALIB_MONGOOSE && id != EXYNOS8890_CALIB_APOLLO)
		return ERR_PTR(-EINVAL);
	if (!exynos8890_calib_is_ready())
		return ERR_PTR(-EPROBE_DEFER);

	return &cpu_metadata[id];
}
EXPORT_SYMBOL_GPL(exynos8890_calib_get_cpu_metadata);

const struct exynos8890_calib_pscdc_table *exynos8890_calib_get_pscdc(void)
{
	if (!exynos8890_calib_is_ready())
		return ERR_PTR(-EPROBE_DEFER);

	return &pscdc_table;
}
EXPORT_SYMBOL_GPL(exynos8890_calib_get_pscdc);

const struct exynos8890_calib_mif_timing *
exynos8890_calib_get_mif_timing(u64 calibration_key)
{
	u64 timing_key;
	unsigned int i;

	if (!exynos8890_calib_is_ready())
		return ERR_PTR(-EPROBE_DEFER);
	if (!calibration_key)
		return ERR_PTR(-ENODATA);
	timing_key = calibration_key | 0x1;

	for (i = 0; i < num_timing_stores; i++)
		if (timing_stores[i].data.key == timing_key)
			return &timing_stores[i].data;

	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(exynos8890_calib_get_mif_timing);

const struct exynos8890_calib_mif_voltages *
exynos8890_calib_get_mif_voltages(u64 calibration_key)
{
	u64 margin_key;
	unsigned int i;

	if (!exynos8890_calib_is_ready())
		return ERR_PTR(-EPROBE_DEFER);
	if (!calibration_key)
		return ERR_PTR(-ENODATA);
	margin_key = (calibration_key & ~0xffULL) | 0x3;

	for (i = 0; i < num_mif_voltage_stores; i++)
		if (mif_voltage_stores[i].data.key == margin_key)
			return &mif_voltage_stores[i].data;

	/* The final store is the vendor no-keyed-margin fallback. */
	if (num_mif_voltage_stores)
		return &mif_voltage_stores[num_mif_voltage_stores - 1].data;

	return ERR_PTR(-ENODATA);
}
EXPORT_SYMBOL_GPL(exynos8890_calib_get_mif_voltages);

static int __init exynos8890_calib_initcall(void)
{
	return exynos8890_calib_init();
}
subsys_initcall_sync(exynos8890_calib_initcall);
