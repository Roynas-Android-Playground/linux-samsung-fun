// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native Exynos8890 Apollo/Mongoose CPU-domain clocks.
 *
 * A CPU rate is not one independently writable divider.  It is a qualified
 * transaction involving a TOP BUS-PLL switch path, the cluster PLL and all
 * characterized auxiliary dividers.  Expose that transaction as one CCF
 * clock so consumers cannot observe or request a half-completed rate change.
 * ECT/ASV is consumed once as immutable calibration data during probe; no
 * writable PWRCAL object participates in a transition.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/samsung.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soc/samsung/exynos8890-apm.h>
#include <linux/soc/samsung/exynos8890-calibration.h>

#define EXYNOS8890_CPUCLK_DOMAINS	2
#define EXYNOS8890_CPUCLK_MAX_MEMBERS	16
#define EXYNOS8890_CPUCLK_MAX_MUX_PARENTS	8

enum exynos8890_cpuclk_id {
	EXYNOS8890_CPUCLK_APOLLO,
	EXYNOS8890_CPUCLK_MONGOOSE,
};

struct exynos8890_cpuclk_level {
	unsigned long rate_hz;
	bool enabled;
	u64 values[EXYNOS8890_CPUCLK_MAX_MEMBERS];
};

struct exynos8890_cpuclk_member {
	const char *name;
	enum exynos8890_calib_member_type type;
	struct clk *clk;
};

struct exynos8890_cpuclk {
	struct clk_hw hw;
	struct device *dev;
	struct mutex *transition_lock;

	struct clk *pll;
	struct clk *pll_user;
	struct clk *bus_user;
	struct clk *mux;
	struct clk *root;
	struct clk *switch_mux;
	struct clk *switch_div;
	struct clk *switch_gate;
	struct clk *bus_osc;
	struct clk *switch_parents[EXYNOS8890_CPUCLK_MAX_MUX_PARENTS];
	unsigned int num_switch_parents;

	struct exynos8890_cpuclk_member *members;
	unsigned int num_members;
	struct exynos8890_cpuclk_level *levels;
	unsigned int num_levels;

	struct exynos8890_calib_switch_entry *switches;
	unsigned int num_switches;
	u32 switch_use;
	u32 switch_notuse;

	struct regmap *sysreg;
	void __iomem *cmu_base;
	u32 ema_offset;
	u32 ema_assist_offset;
	struct exynos8890_calib_ema_entry *ema;
	unsigned int num_ema;
	struct exynos8890_calib_smpl_metadata smpl;
	u32 smpl_saved_word;
	bool smpl_saved_valid;

	int last_error;
	bool faulted;
};

struct exynos8890_cpuclk_provider {
	struct mutex transition_lock;
	struct clk_hw_onecell_data *clk_data;
	struct exynos8890_cpuclk domains[EXYNOS8890_CPUCLK_DOMAINS];
};

#define to_exynos8890_cpuclk(_hw) \
	container_of(_hw, struct exynos8890_cpuclk, hw)

static DEFINE_MUTEX(exynos8890_cpuclk_registry_lock);
static struct exynos8890_cpuclk *exynos8890_cpuclk_registry[EXYNOS8890_CPUCLK_DOMAINS];

/* Return a live provider with its shared transition lock held. */
static struct exynos8890_cpuclk *
exynos8890_cpuclk_lookup_and_lock(struct clk_hw *hw)
{
	struct exynos8890_cpuclk *cpuclk = NULL;
	unsigned int i;

	mutex_lock(&exynos8890_cpuclk_registry_lock);
	for (i = 0; i < EXYNOS8890_CPUCLK_DOMAINS; i++)
		if (exynos8890_cpuclk_registry[i] &&
		    &exynos8890_cpuclk_registry[i]->hw == hw) {
			cpuclk = exynos8890_cpuclk_registry[i];
			mutex_lock(cpuclk->transition_lock);
			break;
		}
	mutex_unlock(&exynos8890_cpuclk_registry_lock);

	return cpuclk;
}

static bool exynos8890_same_clk(struct clk *left, struct clk *right)
{
	return left && right && __clk_get_hw(left) == __clk_get_hw(right);
}

static int exynos8890_cpuclk_parent_index(struct exynos8890_cpuclk *cpuclk,
					  struct clk *mux, unsigned int index,
					  struct clk **parent)
{
	struct clk_hw *parent_hw;

	parent_hw = clk_hw_get_parent_by_index(__clk_get_hw(mux), index);
	if (!parent_hw)
		return -EINVAL;

	*parent = devm_clk_hw_get_clk(cpuclk->dev, parent_hw,
				       dev_name(cpuclk->dev));
	return PTR_ERR_OR_ZERO(*parent);
}

static int exynos8890_cpuclk_set_parent(struct clk *mux, struct clk *parent)
{
	int ret;

	if (exynos8890_same_clk(clk_get_parent(mux), parent))
		return 0;

	ret = clk_set_parent(mux, parent);
	if (ret)
		return ret;

	return exynos8890_same_clk(clk_get_parent(mux), parent) ? 0 : -EIO;
}

static int exynos8890_cpuclk_get_divider(struct clk *clk, u64 *value)
{
	struct clk *parent = clk_get_parent(clk);
	unsigned long parent_rate, rate;
	u64 ratio;

	if (!parent)
		return -EINVAL;

	parent_rate = clk_get_rate(parent);
	rate = clk_get_rate(clk);
	if (!parent_rate || !rate)
		return -EIO;

	ratio = DIV_ROUND_CLOSEST_ULL(parent_rate, rate);
	if (!ratio)
		return -ERANGE;

	*value = ratio - 1;
	return 0;
}

static int exynos8890_cpuclk_set_divider(struct clk *clk, u64 value)
{
	struct clk *parent = clk_get_parent(clk);
	unsigned long parent_rate, requested;
	u64 actual;
	int ret;

	if (!parent || value > ULONG_MAX - 1)
		return -EINVAL;

	parent_rate = clk_get_rate(parent);
	if (!parent_rate)
		return -EIO;

	requested = DIV_ROUND_CLOSEST_ULL(parent_rate, value + 1);
	ret = clk_set_rate(clk, requested);
	if (ret)
		return ret;

	ret = exynos8890_cpuclk_get_divider(clk, &actual);
	if (ret)
		return ret;

	return actual == value ? 0 : -ERANGE;
}

static int exynos8890_cpuclk_read_members(struct exynos8890_cpuclk *cpuclk,
					   u64 *values)
{
	unsigned int i;
	int ret;

	for (i = 0; i < cpuclk->num_members; i++) {
		struct exynos8890_cpuclk_member *member = &cpuclk->members[i];

		switch (member->type) {
		case EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ:
			values[i] = clk_get_rate(member->clk);
			if (!values[i])
				return -EIO;
			break;
		case EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE:
			ret = exynos8890_cpuclk_get_divider(member->clk,
							     &values[i]);
			if (ret)
				return ret;
			break;
		default:
			/* CPU tables are deliberately limited to PLLs and dividers. */
			return -EINVAL;
		}
	}

	return 0;
}

static int exynos8890_cpuclk_set_dividers(struct exynos8890_cpuclk *cpuclk,
					   const u64 *from,
					   const u64 *to, bool high)
{
	unsigned int i;
	int ret;

	for (i = 0; i < cpuclk->num_members; i++) {
		struct exynos8890_cpuclk_member *member = &cpuclk->members[i];

		if (member->type !=
		    EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE)
			continue;
		if (high ? from[i] >= to[i] : from[i] <= to[i])
			continue;

		ret = exynos8890_cpuclk_set_divider(member->clk, to[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int exynos8890_cpuclk_set_pll(struct exynos8890_cpuclk *cpuclk,
				      const u64 *values)
{
	unsigned int i;
	int ret;

	for (i = 0; i < cpuclk->num_members; i++) {
		struct exynos8890_cpuclk_member *member = &cpuclk->members[i];
		unsigned long rate;

		if (member->type != EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ)
			continue;
		if (!values[i] || values[i] > ULONG_MAX)
			return -ERANGE;

		rate = values[i];
		ret = clk_set_rate(member->clk, rate);
		if (ret)
			return ret;
		if (clk_get_rate(member->clk) != rate)
			return -EIO;
	}

	return 0;
}

static int exynos8890_cpuclk_find_level(struct exynos8890_cpuclk *cpuclk,
					 unsigned long rate, bool enabled_only)
{
	unsigned int i;

	for (i = 0; i < cpuclk->num_levels; i++)
		if (cpuclk->levels[i].rate_hz == rate &&
		    (!enabled_only || cpuclk->levels[i].enabled))
			return i;

	return -EINVAL;
}

/* Match vendor dfs_get_lv(): first characterized row at or below @rate. */
static int exynos8890_cpuclk_find_floor(struct exynos8890_cpuclk *cpuclk,
					 unsigned long rate)
{
	unsigned int i;

	for (i = 0; i < cpuclk->num_levels; i++)
		if (rate >= cpuclk->levels[i].rate_hz)
			return i;

	return -ERANGE;
}

static const struct exynos8890_calib_switch_entry *
exynos8890_cpuclk_choose_switch(struct exynos8890_cpuclk *cpuclk,
				       unsigned long old_rate,
				       unsigned long new_rate)
{
	unsigned long highest = max(old_rate, new_rate);
	unsigned int i;

	for (i = 0; i < cpuclk->num_switches; i++)
		if (highest >= cpuclk->switches[i].threshold_rate_hz)
			return &cpuclk->switches[i];

	return &cpuclk->switches[cpuclk->num_switches - 1];
}

static int exynos8890_cpuclk_configure_switch(
		struct exynos8890_cpuclk *cpuclk,
		const struct exynos8890_calib_switch_entry *entry)
{
	u64 divider;
	int ret;

	if (entry->mux_value >= cpuclk->num_switch_parents)
		return -EINVAL;

	/* Vendor programs the raw divider before changing the source mux. */
	ret = exynos8890_cpuclk_set_divider(
		cpuclk->switch_div, entry->divider_ratio_minus_one);
	if (ret)
		return ret;
	ret = exynos8890_cpuclk_set_parent(
		cpuclk->switch_mux, cpuclk->switch_parents[entry->mux_value]);
	if (ret)
		return ret;
	ret = exynos8890_cpuclk_get_divider(cpuclk->switch_div, &divider);
	if (ret)
		return ret;
	if (divider != entry->divider_ratio_minus_one ||
	    clk_get_rate(cpuclk->switch_div) != entry->threshold_rate_hz)
		return -ERANGE;

	return 0;
}

static int exynos8890_cpuclk_enable_switch(struct exynos8890_cpuclk *cpuclk)
{
	int ret;

	ret = clk_prepare_enable(cpuclk->switch_gate);
	if (ret)
		return ret;

	ret = exynos8890_cpuclk_set_parent(cpuclk->bus_user,
					   cpuclk->switch_gate);
	if (ret)
		clk_disable_unprepare(cpuclk->switch_gate);

	return ret;
}

static int exynos8890_cpuclk_disable_switch(struct exynos8890_cpuclk *cpuclk)
{
	int ret;

	ret = exynos8890_cpuclk_set_parent(cpuclk->bus_user, cpuclk->bus_osc);
	if (ret)
		return ret;

	ret = exynos8890_cpuclk_set_divider(cpuclk->switch_div, 0);
	if (ret)
		return ret;

	clk_disable_unprepare(cpuclk->switch_gate);
	return 0;
}

static int exynos8890_cpuclk_use_switch(struct exynos8890_cpuclk *cpuclk)
{
	return exynos8890_cpuclk_set_parent(cpuclk->mux, cpuclk->bus_user);
}

static int exynos8890_cpuclk_use_pll(struct exynos8890_cpuclk *cpuclk)
{
	int ret;

	ret = exynos8890_cpuclk_set_parent(cpuclk->pll_user, cpuclk->pll);
	if (ret)
		return ret;

	return exynos8890_cpuclk_set_parent(cpuclk->mux, cpuclk->pll_user);
}

static int exynos8890_cpuclk_restore(struct exynos8890_cpuclk *cpuclk,
				      const u64 *old_values,
				      const struct exynos8890_calib_switch_entry *entry,
				      const u64 *switch_values,
				      bool switch_held)
{
	u64 live[EXYNOS8890_CPUCLK_MAX_MEMBERS];
	int ret;

	ret = exynos8890_cpuclk_configure_switch(cpuclk, entry);
	if (ret)
		return ret;
	if (!switch_held) {
		ret = exynos8890_cpuclk_enable_switch(cpuclk);
		if (ret)
			return ret;
		switch_held = true;
	} else {
		ret = exynos8890_cpuclk_set_parent(cpuclk->bus_user,
						   cpuclk->switch_gate);
		if (ret)
			goto out;
	}
	ret = exynos8890_cpuclk_read_members(cpuclk, live);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, live, switch_values, true);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_use_switch(cpuclk);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, live, switch_values, false);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_set_pll(cpuclk, old_values);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, switch_values,
					    old_values, true);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_use_pll(cpuclk);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, switch_values,
					    old_values, false);
	if (ret)
		goto out;
	ret = exynos8890_cpuclk_disable_switch(cpuclk);
	if (!ret)
		switch_held = false;

out:
	/* On an unverified rollback, leave the qualified safe source powered. */
	if (ret && switch_held)
		dev_warn(cpuclk->dev, "%s safe switch remains enabled\n",
			 clk_hw_get_name(&cpuclk->hw));
	return ret;
}

static int exynos8890_cpuclk_transition(struct exynos8890_cpuclk *cpuclk,
					 unsigned long old_rate,
					 unsigned int new_level)
{
	const struct exynos8890_calib_switch_entry *entry;
	const u64 *target = cpuclk->levels[new_level].values;
	const u64 *switch_values;
	u64 old_values[EXYNOS8890_CPUCLK_MAX_MEMBERS];
	bool switch_enabled = false;
	int switch_level, restore_ret, ret;

	ret = exynos8890_cpuclk_read_members(cpuclk, old_values);
	if (ret)
		return ret;

	entry = exynos8890_cpuclk_choose_switch(
		cpuclk, old_rate, cpuclk->levels[new_level].rate_hz);
	switch_level = exynos8890_cpuclk_find_floor(
		cpuclk, entry->threshold_rate_hz);
	if (switch_level < 0)
		return switch_level;
	switch_values = cpuclk->levels[switch_level].values;

	ret = exynos8890_cpuclk_configure_switch(cpuclk, entry);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_enable_switch(cpuclk);
	if (ret)
		goto restore;
	switch_enabled = true;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, old_values,
					     switch_values, true);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_use_switch(cpuclk);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, old_values,
					     switch_values, false);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_set_pll(cpuclk, target);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, switch_values,
					     target, true);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_use_pll(cpuclk);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_set_dividers(cpuclk, switch_values,
					     target, false);
	if (ret)
		goto restore;
	ret = exynos8890_cpuclk_disable_switch(cpuclk);
	if (ret)
		goto restore;
	switch_enabled = false;

	if (clk_get_rate(cpuclk->root) != cpuclk->levels[new_level].rate_hz) {
		ret = -EIO;
		goto restore;
	}
	return 0;

restore:
	/* Balance our switch enable before the force-restore transaction. */
	if (switch_enabled &&
	    !exynos8890_same_clk(clk_get_parent(cpuclk->mux), cpuclk->bus_user)) {
		clk_disable_unprepare(cpuclk->switch_gate);
		switch_enabled = false;
	}
	restore_ret = exynos8890_cpuclk_restore(cpuclk, old_values, entry,
						switch_values, switch_enabled);
	if (restore_ret)
		dev_crit(cpuclk->dev,
			 "%s rollback failed after transition error %d: %d\n",
			 clk_hw_get_name(&cpuclk->hw), ret, restore_ret);

	return ret;
}

static unsigned long exynos8890_cpuclk_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct exynos8890_cpuclk *cpuclk = to_exynos8890_cpuclk(hw);

	return clk_get_rate(cpuclk->root);
}

static int exynos8890_cpuclk_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	struct exynos8890_cpuclk *cpuclk = to_exynos8890_cpuclk(hw);
	int ret;

	if (READ_ONCE(cpuclk->faulted))
		return READ_ONCE(cpuclk->last_error) ?: -EIO;
	ret = exynos8890_cpuclk_find_level(cpuclk, req->rate, true);
	if (ret < 0)
		return ret;

	return 0;
}

static int exynos8890_cpuclk_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct exynos8890_cpuclk *cpuclk = to_exynos8890_cpuclk(hw);
	unsigned long old_rate;
	int level, ret;

	mutex_lock(cpuclk->transition_lock);
	if (cpuclk->faulted) {
		ret = cpuclk->last_error ?: -EIO;
		goto out;
	}

	level = exynos8890_cpuclk_find_level(cpuclk, rate, true);
	if (level < 0) {
		ret = level;
		goto out;
	}
	old_rate = clk_get_rate(cpuclk->root);
	if (old_rate == rate) {
		ret = 0;
		goto out;
	}

	ret = exynos8890_cpuclk_transition(cpuclk, old_rate, level);
	if (ret) {
		cpuclk->faulted = true;
		cpuclk->last_error = ret;
		dev_err(cpuclk->dev, "%s transaction %lu -> %lu Hz failed: %d\n",
			clk_hw_get_name(hw), old_rate, rate, ret);
	}

out:
	mutex_unlock(cpuclk->transition_lock);
	return ret;
}

static const struct clk_ops exynos8890_cpuclk_ops = {
	.recalc_rate = exynos8890_cpuclk_recalc_rate,
	.determine_rate = exynos8890_cpuclk_determine_rate,
	.set_rate = exynos8890_cpuclk_set_rate,
};

static int exynos8890_cpuclk_copy_calibration(
		struct exynos8890_cpuclk *cpuclk,
		const struct exynos8890_calib_domain *domain,
		const struct exynos8890_calib_cpu_metadata *metadata)
{
	unsigned long previous_rate = ULONG_MAX;
	unsigned int i, j;

	if (!domain->num_opps || !domain->opps || !domain->num_members ||
	    !domain->members || !domain->member_values ||
	    domain->num_members > EXYNOS8890_CPUCLK_MAX_MEMBERS ||
	    !metadata->num_switches || !metadata->switches)
		return -EINVAL;

	cpuclk->num_levels = domain->num_opps;
	cpuclk->levels = devm_kcalloc(cpuclk->dev, cpuclk->num_levels,
				      sizeof(*cpuclk->levels), GFP_KERNEL);
	if (!cpuclk->levels)
		return -ENOMEM;
	cpuclk->num_members = domain->num_members;
	cpuclk->members = devm_kcalloc(cpuclk->dev, cpuclk->num_members,
				       sizeof(*cpuclk->members), GFP_KERNEL);
	if (!cpuclk->members)
		return -ENOMEM;

	for (i = 0; i < cpuclk->num_members; i++) {
		struct exynos8890_cpuclk_member *member = &cpuclk->members[i];

		if (!domain->members[i].name ||
		    (domain->members[i].type !=
			EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		     domain->members[i].type !=
			EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE))
			return -EINVAL;
		member->name = devm_kstrdup(cpuclk->dev,
					    domain->members[i].name, GFP_KERNEL);
		if (!member->name)
			return -ENOMEM;
		member->type = domain->members[i].type;
		member->clk = devm_clk_get(cpuclk->dev, member->name);
		if (IS_ERR(member->clk))
			return dev_err_probe(cpuclk->dev, PTR_ERR(member->clk),
					     "failed to map CPU member %s\n",
					     member->name);
	}

	for (i = 0; i < cpuclk->num_levels; i++) {
		struct exynos8890_cpuclk_level *level = &cpuclk->levels[i];

		if (!domain->opps[i].rate_hz ||
		    domain->opps[i].rate_hz >= previous_rate)
			return -EINVAL;
		level->rate_hz = domain->opps[i].rate_hz;
		level->enabled = domain->opps[i].enabled;
		previous_rate = level->rate_hz;
		for (j = 0; j < cpuclk->num_members; j++) {
			level->values[j] =
				exynos8890_calib_member_value(domain, i, j);
			if (cpuclk->members[j].type ==
			    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
			    (!level->values[j] || level->values[j] > ULONG_MAX))
				return -ERANGE;
			if (cpuclk->members[j].type ==
			    EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE &&
			    level->values[j] > 63)
				return -ERANGE;
		}
	}

	cpuclk->num_switches = metadata->num_switches;
	cpuclk->switches = devm_kmemdup(cpuclk->dev, metadata->switches,
					cpuclk->num_switches *
					sizeof(*cpuclk->switches), GFP_KERNEL);
	if (!cpuclk->switches)
		return -ENOMEM;
	for (i = 0; i < cpuclk->num_switches; i++) {
		if (!cpuclk->switches[i].threshold_rate_hz ||
		    cpuclk->switches[i].divider_ratio_minus_one > 15 ||
		    cpuclk->switches[i].mux_value >=
			EXYNOS8890_CPUCLK_MAX_MUX_PARENTS ||
		    (i && cpuclk->switches[i].threshold_rate_hz >=
			  cpuclk->switches[i - 1].threshold_rate_hz))
			return -EINVAL;
	}
	cpuclk->switch_use = metadata->switch_use;
	cpuclk->switch_notuse = metadata->switch_notuse;
	if (cpuclk->switch_use != 1 || cpuclk->switch_notuse != 0)
		return -EINVAL;

	cpuclk->ema_offset = metadata->ema_register_offset;
	cpuclk->ema_assist_offset = metadata->ema_assist_register_offset;
	cpuclk->num_ema = metadata->num_ema;
	if (!cpuclk->num_ema || !metadata->ema ||
	    cpuclk->ema_offset == EXYNOS8890_CALIB_NO_REGISTER)
		return -EINVAL;
	cpuclk->ema = devm_kmemdup(cpuclk->dev, metadata->ema,
				   cpuclk->num_ema * sizeof(*cpuclk->ema),
				   GFP_KERNEL);
	if (!cpuclk->ema)
		return -ENOMEM;
	for (i = 1; i < cpuclk->num_ema; i++)
		if (cpuclk->ema[i].min_voltage_uv >=
		    cpuclk->ema[i - 1].min_voltage_uv)
			return -EINVAL;
	cpuclk->smpl = metadata->smpl;
	if (cpuclk->smpl.register_offset == EXYNOS8890_CALIB_NO_REGISTER ||
	    !cpuclk->smpl.init_mask || !cpuclk->smpl.deinit_mask ||
	    !cpuclk->smpl.trigger_mask || !cpuclk->smpl.status_mask ||
	    ((cpuclk->smpl.init_mask | cpuclk->smpl.deinit_mask |
	      cpuclk->smpl.trigger_mask) & cpuclk->smpl.status_mask) ||
	    (cpuclk->smpl.init_value & ~cpuclk->smpl.init_mask) ||
	    (cpuclk->smpl.deinit_value & ~cpuclk->smpl.deinit_mask) ||
	    (cpuclk->smpl.trigger_value & ~cpuclk->smpl.trigger_mask))
		return -EINVAL;

	return 0;
}

static void exynos8890_cpuclk_iounmap(void *base)
{
	iounmap(base);
}

static int exynos8890_cpuclk_map_cmu(struct exynos8890_cpuclk *cpuclk,
				      const char *property)
{
	struct device_node *np;

	np = of_parse_phandle(cpuclk->dev->of_node, property, 0);
	if (!np)
		return -EINVAL;
	cpuclk->cmu_base = of_iomap(np, 0);
	of_node_put(np);
	if (!cpuclk->cmu_base)
		return -ENOMEM;

	return devm_add_action_or_reset(cpuclk->dev,
					exynos8890_cpuclk_iounmap,
					cpuclk->cmu_base);
}

static int exynos8890_cpuclk_smpl_update(struct exynos8890_cpuclk *cpuclk,
					  u32 mask, u32 value)
{
	void __iomem *reg = cpuclk->cmu_base + cpuclk->smpl.register_offset;
	u32 regval;

	regval = readl_relaxed(reg);
	regval = (regval & ~mask) | (value & mask);
	writel_relaxed(regval, reg);

	return (readl_relaxed(reg) & mask) == (value & mask) ? 0 : -EIO;
}

static int exynos8890_cpuclk_smpl_init(struct exynos8890_cpuclk *cpuclk)
{
	return exynos8890_cpuclk_smpl_update(cpuclk, cpuclk->smpl.init_mask,
					     cpuclk->smpl.init_value);
}

static int exynos8890_cpuclk_smpl_deinit(struct exynos8890_cpuclk *cpuclk)
{
	return exynos8890_cpuclk_smpl_update(cpuclk, cpuclk->smpl.deinit_mask,
					     cpuclk->smpl.deinit_value);
}

static int exynos8890_cpuclk_get_controls(struct exynos8890_cpuclk *cpuclk,
					   const char *prefix)
{
	static const char * const suffixes[] = {
		"pll", "pll-user", "bus-user", "mux", "root", "switch-mux",
		"switch-div", "switch-gate",
	};
	struct clk **clks[] = {
		&cpuclk->pll, &cpuclk->pll_user, &cpuclk->bus_user, &cpuclk->mux,
		&cpuclk->root, &cpuclk->switch_mux, &cpuclk->switch_div,
		&cpuclk->switch_gate,
	};
	char name[48];
	unsigned int i, parents;
	int ret;

	for (i = 0; i < ARRAY_SIZE(suffixes); i++) {
		snprintf(name, sizeof(name), "%s-%s", prefix, suffixes[i]);
		*clks[i] = devm_clk_get(cpuclk->dev, name);
		if (IS_ERR(*clks[i]))
			return dev_err_probe(cpuclk->dev, PTR_ERR(*clks[i]),
					     "failed to get %s\n", name);
	}

	parents = clk_hw_get_num_parents(__clk_get_hw(cpuclk->switch_mux));
	if (!parents || parents > EXYNOS8890_CPUCLK_MAX_MUX_PARENTS)
		return -EINVAL;
	cpuclk->num_switch_parents = parents;
	for (i = 0; i < parents; i++) {
		ret = exynos8890_cpuclk_parent_index(cpuclk, cpuclk->switch_mux,
						      i,
						      &cpuclk->switch_parents[i]);
		if (ret)
			return ret;
	}

	return exynos8890_cpuclk_parent_index(cpuclk, cpuclk->bus_user, 0,
					      &cpuclk->bus_osc);
}

static int exynos8890_cpuclk_register_domain(
		struct exynos8890_cpuclk_provider *provider,
		struct platform_device *pdev, enum exynos8890_cpuclk_id id,
		enum exynos8890_calib_domain_id calib_id, const char *prefix,
		const char *clock_name, const char *sysreg_property,
		const char *cmu_property)
{
	const struct exynos8890_calib_cpu_metadata *metadata;
	const struct exynos8890_calib_domain *domain;
	struct exynos8890_cpuclk *cpuclk = &provider->domains[id];
	struct clk_init_data init = {
		.ops = &exynos8890_cpuclk_ops,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	bool pll_found = false;
	unsigned int i;
	int ret;

	domain = exynos8890_calib_get_domain(calib_id);
	if (IS_ERR(domain))
		return dev_err_probe(&pdev->dev, PTR_ERR(domain),
				     "%s calibration is unavailable\n", prefix);
	metadata = exynos8890_calib_get_cpu_metadata(calib_id);
	if (IS_ERR(metadata))
		return dev_err_probe(&pdev->dev, PTR_ERR(metadata),
				     "%s CPU metadata is unavailable\n", prefix);

	cpuclk->dev = &pdev->dev;
	cpuclk->transition_lock = &provider->transition_lock;
	ret = exynos8890_cpuclk_copy_calibration(cpuclk, domain, metadata);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid %s CPU calibration\n", prefix);
	ret = exynos8890_cpuclk_get_controls(cpuclk, prefix);
	if (ret)
		return ret;

	/* The ECT PLL member and public control clock must be the same CCF HW. */
	for (i = 0; i < cpuclk->num_members; i++) {
		if (cpuclk->members[i].type !=
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ)
			continue;
		if (pll_found ||
		    !exynos8890_same_clk(cpuclk->members[i].clk, cpuclk->pll))
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "%s PLL has two CCF owners\n", prefix);
		pll_found = true;
	}
	if (!pll_found)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "%s calibration has no PLL member\n", prefix);

	cpuclk->sysreg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							sysreg_property);
	if (IS_ERR(cpuclk->sysreg))
		return dev_err_probe(&pdev->dev, PTR_ERR(cpuclk->sysreg),
				     "failed to map %s SYSREG\n", prefix);
	ret = exynos8890_cpuclk_map_cmu(cpuclk, cmu_property);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to map %s CMU SMPL register\n",
				     prefix);

	ret = clk_prepare_enable(cpuclk->pll);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to keep %s PLL enabled\n", prefix);

	init.name = clock_name;
	cpuclk->hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &cpuclk->hw);
	if (ret) {
		clk_disable_unprepare(cpuclk->pll);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register %s CPU clock\n", prefix);
	}
	provider->clk_data->hws[id] = &cpuclk->hw;

	return 0;
}

int exynos8890_cpuclk_set_ema(struct clk *clk, u32 voltage_uv)
{
	struct exynos8890_cpuclk *cpuclk;
	struct clk_hw *hw;
	unsigned int i;
	int ret;

	if (!clk)
		return -EINVAL;
	hw = __clk_get_hw(clk);
	cpuclk = exynos8890_cpuclk_lookup_and_lock(hw);
	if (!cpuclk)
		return -ENODEV;

	for (i = 0; i < cpuclk->num_ema; i++)
		if (voltage_uv >= cpuclk->ema[i].min_voltage_uv)
			break;
	if (i == cpuclk->num_ema)
		i = cpuclk->num_ema - 1;

	ret = regmap_write(cpuclk->sysreg, cpuclk->ema_offset,
			   cpuclk->ema[i].value);
	if (!ret && cpuclk->ema_assist_offset != EXYNOS8890_CALIB_NO_REGISTER)
		ret = regmap_write(cpuclk->sysreg, cpuclk->ema_assist_offset,
				   cpuclk->ema[i].assist_value);
	mutex_unlock(cpuclk->transition_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpuclk_set_ema);

int exynos8890_cpuclk_get_error(struct clk *clk)
{
	struct clk_hw *hw;
	unsigned int i;
	int ret = -ENODEV;

	if (!clk)
		return -EINVAL;
	hw = __clk_get_hw(clk);
	mutex_lock(&exynos8890_cpuclk_registry_lock);
	for (i = 0; i < EXYNOS8890_CPUCLK_DOMAINS; i++)
		if (exynos8890_cpuclk_registry[i] &&
		    &exynos8890_cpuclk_registry[i]->hw == hw) {
			ret = READ_ONCE(exynos8890_cpuclk_registry[i]->last_error);
			break;
		}
	mutex_unlock(&exynos8890_cpuclk_registry_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpuclk_get_error);

int exynos8890_cpuclk_smpl_status(struct clk *clk)
{
	struct exynos8890_cpuclk *cpuclk;
	struct clk_hw *hw;
	u32 value;

	if (!clk)
		return -EINVAL;
	hw = __clk_get_hw(clk);
	cpuclk = exynos8890_cpuclk_lookup_and_lock(hw);
	if (!cpuclk)
		return -ENODEV;

	value = readl_relaxed(cpuclk->cmu_base + cpuclk->smpl.register_offset);
	mutex_unlock(cpuclk->transition_lock);

	return !!(value & cpuclk->smpl.status_mask);
}
EXPORT_SYMBOL_GPL(exynos8890_cpuclk_smpl_status);

/*
 * Preserve vendor set_smpl semantics. This writes the characterized trigger
 * field; it must not be described as clearing the independent status bit.
 */
int exynos8890_cpuclk_smpl_trigger(struct clk *clk)
{
	struct exynos8890_cpuclk *cpuclk;
	struct clk_hw *hw;
	int ret;

	if (!clk)
		return -EINVAL;
	hw = __clk_get_hw(clk);
	cpuclk = exynos8890_cpuclk_lookup_and_lock(hw);
	if (!cpuclk)
		return -ENODEV;

	ret = exynos8890_cpuclk_smpl_update(cpuclk,
					    cpuclk->smpl.trigger_mask,
					    cpuclk->smpl.trigger_value);
	mutex_unlock(cpuclk->transition_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpuclk_smpl_trigger);

static int exynos8890_cpuclk_probe(struct platform_device *pdev)
{
	struct exynos8890_cpuclk_provider *provider;
	int ret;

	if (!exynos8890_apm_dvfs_ready())
		return -EPROBE_DEFER;

	ret = exynos8890_calib_init();
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "calibration provider is not ready\n");

	provider = devm_kzalloc(&pdev->dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	mutex_init(&provider->transition_lock);
	provider->clk_data = devm_kzalloc(
		&pdev->dev, struct_size(provider->clk_data, hws,
					EXYNOS8890_CPUCLK_DOMAINS), GFP_KERNEL);
	if (!provider->clk_data)
		return -ENOMEM;
	provider->clk_data->num = EXYNOS8890_CPUCLK_DOMAINS;

	ret = exynos8890_cpuclk_register_domain(
		provider, pdev, EXYNOS8890_CPUCLK_APOLLO,
		EXYNOS8890_CALIB_APOLLO, "apollo", "apollo-dvfs",
		"samsung,apollo-sysreg", "samsung,apollo-cmu");
	if (ret)
		return ret;
	ret = exynos8890_cpuclk_register_domain(
		provider, pdev, EXYNOS8890_CPUCLK_MONGOOSE,
		EXYNOS8890_CALIB_MONGOOSE, "mongoose", "mongoose-dvfs",
		"samsung,mongoose-sysreg", "samsung,mongoose-cmu");
	if (ret) {
		clk_disable_unprepare(provider->domains[EXYNOS8890_CPUCLK_APOLLO].pll);
		return ret;
	}
	ret = exynos8890_cpuclk_smpl_init(
		&provider->domains[EXYNOS8890_CPUCLK_APOLLO]);
	if (ret) {
		exynos8890_cpuclk_smpl_deinit(
			&provider->domains[EXYNOS8890_CPUCLK_APOLLO]);
		goto disable_plls;
	}
	ret = exynos8890_cpuclk_smpl_init(
		&provider->domains[EXYNOS8890_CPUCLK_MONGOOSE]);
	if (ret) {
		exynos8890_cpuclk_smpl_deinit(
			&provider->domains[EXYNOS8890_CPUCLK_MONGOOSE]);
		exynos8890_cpuclk_smpl_deinit(
			&provider->domains[EXYNOS8890_CPUCLK_APOLLO]);
		goto disable_plls;
	}

	ret = devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get,
					  provider->clk_data);
	if (ret) {
		exynos8890_cpuclk_smpl_deinit(
			&provider->domains[EXYNOS8890_CPUCLK_MONGOOSE]);
		exynos8890_cpuclk_smpl_deinit(
			&provider->domains[EXYNOS8890_CPUCLK_APOLLO]);
		goto disable_plls;
	}

	platform_set_drvdata(pdev, provider);
	mutex_lock(&exynos8890_cpuclk_registry_lock);
	exynos8890_cpuclk_registry[EXYNOS8890_CPUCLK_APOLLO] =
		&provider->domains[EXYNOS8890_CPUCLK_APOLLO];
	exynos8890_cpuclk_registry[EXYNOS8890_CPUCLK_MONGOOSE] =
		&provider->domains[EXYNOS8890_CPUCLK_MONGOOSE];
	mutex_unlock(&exynos8890_cpuclk_registry_lock);
	return 0;

disable_plls:
	clk_disable_unprepare(provider->domains[EXYNOS8890_CPUCLK_MONGOOSE].pll);
	clk_disable_unprepare(provider->domains[EXYNOS8890_CPUCLK_APOLLO].pll);
	return ret;
}

static void exynos8890_cpuclk_remove(struct platform_device *pdev)
{
	struct exynos8890_cpuclk_provider *provider = platform_get_drvdata(pdev);
	unsigned int i;

	mutex_lock(&exynos8890_cpuclk_registry_lock);
	exynos8890_cpuclk_registry[EXYNOS8890_CPUCLK_APOLLO] = NULL;
	exynos8890_cpuclk_registry[EXYNOS8890_CPUCLK_MONGOOSE] = NULL;
	mutex_lock(&provider->transition_lock);
	mutex_unlock(&exynos8890_cpuclk_registry_lock);
	for (i = 0; i < EXYNOS8890_CPUCLK_DOMAINS; i++) {
		provider->domains[i].last_error = -ENODEV;
		provider->domains[i].faulted = true;
	}
	exynos8890_cpuclk_smpl_deinit(
		&provider->domains[EXYNOS8890_CPUCLK_MONGOOSE]);
	exynos8890_cpuclk_smpl_deinit(
		&provider->domains[EXYNOS8890_CPUCLK_APOLLO]);
	clk_disable_unprepare(provider->domains[EXYNOS8890_CPUCLK_MONGOOSE].pll);
	clk_disable_unprepare(provider->domains[EXYNOS8890_CPUCLK_APOLLO].pll);
	mutex_unlock(&provider->transition_lock);
}

static int __maybe_unused exynos8890_cpuclk_suspend_noirq(struct device *dev)
{
	struct exynos8890_cpuclk_provider *provider = dev_get_drvdata(dev);
	unsigned int i;

	/* All clock consumers are suspended before their provider. */
	for (i = 0; i < EXYNOS8890_CPUCLK_DOMAINS; i++) {
		struct exynos8890_cpuclk *cpuclk = &provider->domains[i];

		cpuclk->smpl_saved_word = readl_relaxed(
			cpuclk->cmu_base + cpuclk->smpl.register_offset);
		cpuclk->smpl_saved_valid = true;
	}

	return 0;
}

static int __maybe_unused exynos8890_cpuclk_resume_noirq(struct device *dev)
{
	struct exynos8890_cpuclk_provider *provider = dev_get_drvdata(dev);
	int ret = 0;
	unsigned int i;

	for (i = 0; i < EXYNOS8890_CPUCLK_DOMAINS; i++) {
		struct exynos8890_cpuclk *cpuclk = &provider->domains[i];
		void __iomem *reg;
		u32 restore_word, writable_mask, value;

		if (!cpuclk->smpl_saved_valid)
			continue;
		reg = cpuclk->cmu_base + cpuclk->smpl.register_offset;
		value = readl_relaxed(reg);
		writable_mask = cpuclk->smpl.init_mask |
				cpuclk->smpl.deinit_mask |
				cpuclk->smpl.trigger_mask;
		restore_word = (value & ~writable_mask) |
			       (cpuclk->smpl_saved_word & writable_mask);
		writel_relaxed(restore_word, reg);
		value = readl_relaxed(reg);
		if ((value & writable_mask) !=
		    (restore_word & writable_mask)) {
			WRITE_ONCE(cpuclk->last_error, -EIO);
			WRITE_ONCE(cpuclk->faulted, true);
			ret = -EIO;
		}
	}

	return ret;
}

static const struct dev_pm_ops exynos8890_cpuclk_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos8890_cpuclk_suspend_noirq,
				      exynos8890_cpuclk_resume_noirq)
};

static const struct of_device_id exynos8890_cpuclk_of_match[] = {
	{ .compatible = "samsung,exynos8890-cpu-clock" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpuclk_of_match);

static struct platform_driver exynos8890_cpuclk_driver = {
	.probe = exynos8890_cpuclk_probe,
	.remove = exynos8890_cpuclk_remove,
	.driver = {
		.name = "exynos8890-cpu-clock",
		.of_match_table = exynos8890_cpuclk_of_match,
		.pm = pm_sleep_ptr(&exynos8890_cpuclk_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos8890_cpuclk_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 native CPU-domain clocks");
MODULE_LICENSE("GPL");
