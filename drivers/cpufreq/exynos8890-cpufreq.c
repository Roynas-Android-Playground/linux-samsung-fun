// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 dual-cluster CPU frequency scaling.
 *
 * The driver consumes fuse-qualified ECT rows and performs CAL-equivalent
 * BUS-PLL switching. It refuses to probe unless the APM Cortex-M3 is off and
 * a MIF devfreq provider is available, so Linux never races autonomous DVFS
 * firmware or omits the vendor CPU-to-memory frequency floor.
 */

#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/samsung/exynos8890-apm.h>
#include <linux/soc/samsung/exynos8890-ect.h>
#include <linux/mfd/syscon.h>

#define EXYNOS8890_CLUSTERS		2
#define EXYNOS8890_APOLLO		0
#define EXYNOS8890_MONGOOSE		1
#define EXYNOS8890_APM_STATUS		0x2504
#define EXYNOS8890_APM_STATUS_ON	BIT(0)
#define EXYNOS8890_APOLLO_EMA_CON	0x320
#define EXYNOS8890_MNGS_EMA_CON	0x314
#define EXYNOS8890_MNGS_ASSIST_CON	0x1040
#define EXYNOS8890_COLD_OFFSET_UV	25000
#define EXYNOS8890_COLD_LIMIT_UV	1350000
#define EXYNOS8890_TRANSITION_LATENCY	100000

struct exynos8890_bus_lock {
	u32 rate_khz;
	u32 mif_khz;
};

static const struct exynos8890_bus_lock apollo_bus_locks[] = {
	{ 1976000, 0 }, { 1898000, 0 }, { 1794000, 0 }, { 1690000, 0 },
	{ 1586000, 1014000 }, { 1482000, 1014000 }, { 1378000, 1014000 },
	{ 1274000, 1014000 }, { 1170000, 845000 }, { 1066000, 845000 },
	{ 962000, 845000 }, { 858000, 676000 }, { 754000, 676000 },
	{ 650000, 546000 }, { 546000, 421000 }, { 442000, 0 },
	{ 338000, 0 }, { 234000, 0 }, { 130000, 0 },
};

static const struct exynos8890_bus_lock mongoose_bus_locks[] = {
	{ 3016000, 1794000 }, { 2912000, 1794000 }, { 2808000, 1794000 },
	{ 2704000, 1794000 }, { 2600000, 1794000 }, { 2496000, 1716000 },
	{ 2392000, 1716000 }, { 2288000, 1716000 }, { 2184000, 1539000 },
	{ 2080000, 1539000 }, { 1976000, 1352000 }, { 1872000, 1144000 },
	{ 1768000, 1144000 }, { 1664000, 1014000 }, { 1560000, 1014000 },
	{ 1456000, 845000 }, { 1352000, 845000 }, { 1248000, 676000 },
	{ 1144000, 546000 }, { 1040000, 546000 }, { 936000, 421000 },
	{ 832000, 421000 }, { 728000, 421000 }, { 624000, 0 },
	{ 520000, 0 }, { 416000, 0 }, { 312000, 0 }, { 208000, 0 },
};

static const unsigned long switch_rates[] = {
	1056000000, 528000000, 352000000, 264000000, 176000000, 96000000,
};

struct exynos8890_cluster {
	const char *name;
	struct exynos8890_ect_cpu_domain ect;
	struct cpufreq_frequency_table table[EXYNOS8890_ECT_MAX_LEVELS + 1];
	u32 num_table;
	struct clk *pll;
	struct clk *pll_user;
	struct clk *bus_user;
	struct clk *mux;
	struct clk *root;
	struct clk *switch_div;
	struct clk *switch_gate;
	struct clk *members[EXYNOS8890_ECT_MAX_CLOCKS];
	struct regulator *regulator;
	struct regmap *ema;
	const struct exynos8890_bus_lock *bus_locks;
	size_t num_bus_locks;
	u32 max_supported_khz;
	struct cpumask cpus;
	struct mutex lock;
	bool faulted;
	bool voltage_degraded;
};

struct exynos8890_cpufreq {
	struct device *dev;
	struct exynos8890_cluster cluster[EXYNOS8890_CLUSTERS];
	struct regmap *pmu;
	struct device *mif_dev;
	struct device *apm_dev;
	struct device_link *mif_link;
	struct device_link *apm_link;
	int (*apm_claim)(struct device *dev, bool claim);
	bool apm_claimed;
	struct dev_pm_qos_request mif_qos[EXYNOS8890_CLUSTERS];
};

static DEFINE_MUTEX(exynos8890_driver_lock);
static struct exynos8890_cpufreq *exynos8890_cpufreq_data;

static u32 exynos8890_mif_lock(struct exynos8890_cluster *cluster, u32 rate)
{
	int i;

	for (i = 0; i < cluster->num_bus_locks; i++)
		if (cluster->bus_locks[i].rate_khz == rate)
			return cluster->bus_locks[i].mif_khz;
	return U32_MAX;
}

static int exynos8890_update_mif(struct exynos8890_cpufreq *data,
				 unsigned int cluster, u32 rate_khz)
{
	int ret = dev_pm_qos_update_request(&data->mif_qos[cluster], rate_khz);

	return ret < 0 ? ret : 0;
}

static int exynos8890_set_ema(struct exynos8890_cluster *cluster, u32 voltage)
{
	if (cluster == &exynos8890_cpufreq_data->cluster[EXYNOS8890_APOLLO])
		return regmap_write(cluster->ema, EXYNOS8890_APOLLO_EMA_CON, 0x492);

	if (regmap_write(cluster->ema, EXYNOS8890_MNGS_ASSIST_CON, 0))
		return -EIO;
	if (voltage >= 1106000)
		return regmap_write(cluster->ema, EXYNOS8890_MNGS_EMA_CON, 0xe91b9);
	if (voltage >= 900000)
		return regmap_write(cluster->ema, EXYNOS8890_MNGS_EMA_CON, 0x1091b9);
	return regmap_write(cluster->ema, EXYNOS8890_MNGS_EMA_CON, 0x1095b9);
}

static int exynos8890_set_member_dividers(struct exynos8890_cluster *cluster,
					 unsigned int level, unsigned int other,
					 bool safe)
{
	int i, ret;

	for (i = 1; i < cluster->ect.num_clocks; i++) {
		u32 divider = cluster->ect.levels[level].clock_values[i];
		unsigned long parent_rate, rate;

		if (safe)
			divider = max(divider,
				      cluster->ect.levels[other].clock_values[i]);
		parent_rate = clk_get_rate(clk_get_parent(cluster->members[i]));
		if (!parent_rate)
			return -EINVAL;
		rate = DIV_ROUND_UP(parent_rate, divider + 1);
		ret = clk_set_rate(cluster->members[i], rate);
		if (ret)
			return ret;
	}
	return 0;
}

static int exynos8890_switch_clock(struct exynos8890_cluster *cluster,
				   unsigned int old_level,
				   unsigned int new_level)
{
	unsigned long old_rate = cluster->ect.levels[old_level].rate_khz * 1000UL;
	unsigned long new_rate = cluster->ect.levels[new_level].rate_khz * 1000UL;
	unsigned long limit = min(old_rate, new_rate);
	unsigned long parent_rate, request, selected = 0;
	unsigned int divider = 0;
	bool on_bus = false, pll_changed = false;
	int i, restore_ret, ret;

	for (i = 0; i < ARRAY_SIZE(switch_rates); i++) {
		divider = DIV_ROUND_UP(switch_rates[i], limit);
		if (divider >= 1 && divider <= 64) {
			selected = switch_rates[i];
			break;
		}
	}
	if (!selected)
		return -ERANGE;

	ret = clk_set_rate(cluster->switch_div, selected);
	if (ret)
		return ret;
	ret = clk_prepare_enable(cluster->switch_gate);
	if (ret)
		return ret;
	ret = clk_set_parent(cluster->bus_user, cluster->switch_gate);
	if (ret)
		goto out;
	ret = exynos8890_set_member_dividers(cluster, new_level, old_level, true);
	if (ret)
		goto out;
	parent_rate = clk_get_rate(clk_get_parent(cluster->root));
	request = DIV_ROUND_UP(parent_rate, divider);
	ret = clk_set_rate(cluster->root, request);
	if (ret)
		goto out;
	ret = clk_set_parent(cluster->mux, cluster->bus_user);
	if (ret)
		goto out;
	on_bus = true;
	if (clk_get_rate(cluster->root) > limit) {
		ret = -ERANGE;
		goto restore_parent;
	}
	ret = clk_set_rate(cluster->pll, new_rate);
	if (ret)
		goto restore_parent;
	pll_changed = true;

	/* Leave the switch source before applying target divider decreases. */
	ret = clk_set_parent(cluster->mux, cluster->pll_user);
	if (ret)
		goto restore_parent;
	on_bus = false;
	ret = clk_set_rate(cluster->root, new_rate);
	if (!ret)
		ret = exynos8890_set_member_dividers(cluster, new_level, old_level,
						       false);
	if (ret || clk_get_rate(cluster->root) != new_rate) {
		cluster->faulted = true;
		/* The CPU reached its new rate; retain conservative dividers. */
		if (clk_get_rate(cluster->root) == new_rate)
			ret = 0;
		else
			ret = ret ?: -EIO;
	}
	goto out;

restore_parent:
	if (pll_changed) {
		restore_ret = clk_set_rate(cluster->pll, old_rate);
		if (restore_ret)
			cluster->faulted = true;
	}
	restore_ret = clk_set_parent(cluster->mux, cluster->pll_user);
	if (!restore_ret) {
		on_bus = false;
		if (pll_changed &&
		    (clk_set_rate(cluster->root, old_rate) ||
		     exynos8890_set_member_dividers(cluster, old_level, new_level,
						      false)))
			cluster->faulted = true;
	} else {
		cluster->faulted = true;
	}
out:
	/* Never gate the source while it can still be feeding the CPUs. */
	if (!on_bus)
		clk_disable_unprepare(cluster->switch_gate);
	return ret;
}

static unsigned int exynos8890_cpufreq_get(unsigned int cpu)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	int i;

	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (cpumask_test_cpu(cpu, &data->cluster[i].cpus))
			return clk_get_rate(data->cluster[i].root) / 1000;
	return 0;
}

static int exynos8890_target_index(struct cpufreq_policy *policy,
				   unsigned int index)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	struct exynos8890_cluster *cluster = policy->driver_data;
	unsigned int new_level = cluster->table[index].driver_data;
	unsigned int old_rate = exynos8890_cpufreq_get(policy->cpu);
	unsigned int old_level, new_rate, new_voltage;
	u32 mif_lock, old_mif_lock;
	int delay, old_voltage, ret;

	for (old_level = 0; old_level < cluster->ect.num_levels; old_level++)
		if (cluster->ect.levels[old_level].rate_khz == old_rate)
			break;
	if (old_level == cluster->ect.num_levels)
		return -EINVAL;
	new_rate = cluster->ect.levels[new_level].rate_khz;
	if (new_rate == old_rate)
		return 0;
	new_voltage = cluster->ect.levels[new_level].voltage_uv;
	if (new_voltage <= EXYNOS8890_COLD_LIMIT_UV - EXYNOS8890_COLD_OFFSET_UV)
		new_voltage += EXYNOS8890_COLD_OFFSET_UV;
	mif_lock = exynos8890_mif_lock(cluster, new_rate);
	old_mif_lock = exynos8890_mif_lock(cluster, old_rate);
	if (mif_lock == U32_MAX || old_mif_lock == U32_MAX)
		return -EINVAL;
	old_voltage = regulator_get_voltage(cluster->regulator);
	if (old_voltage < 0)
		return old_voltage;

	mutex_lock(&cluster->lock);
	if (cluster->faulted) {
		ret = -EIO;
		goto out_unlock;
	}
	if (new_rate > old_rate) {
		ret = exynos8890_update_mif(data, cluster - data->cluster, mif_lock);
		if (ret)
			goto out_unlock;
		ret = regulator_set_voltage_triplet(cluster->regulator, new_voltage,
						    new_voltage, new_voltage);
		if (ret) {
			exynos8890_update_mif(data, cluster - data->cluster,
					      old_mif_lock);
			goto out_unlock;
		}
		delay = regulator_set_voltage_time(cluster->regulator, old_voltage,
						   new_voltage);
		if (delay > 0)
			usleep_range(delay, delay + 50);
		ret = exynos8890_set_ema(cluster, new_voltage);
		if (ret) {
			regulator_set_voltage_triplet(cluster->regulator, old_voltage,
						      old_voltage, old_voltage);
			exynos8890_update_mif(data, cluster - data->cluster,
					      old_mif_lock);
			goto out_unlock;
		}
	}
	ret = exynos8890_switch_clock(cluster, old_level, new_level);
	if (ret) {
		cluster->faulted = true;
		goto out_unlock;
	}
	if (new_rate < old_rate) {
		ret = exynos8890_set_ema(cluster, new_voltage);
		if (!ret)
			ret = regulator_set_voltage_triplet(cluster->regulator,
							    new_voltage, new_voltage,
							    new_voltage);
		if (!ret) {
			delay = regulator_set_voltage_time(cluster->regulator,
							   old_voltage, new_voltage);
			if (delay > 0)
				usleep_range(delay, delay + 50);
		}
		if (!ret)
			ret = exynos8890_update_mif(data, cluster - data->cluster,
						     mif_lock);
		if (ret < 0) {
			cluster->voltage_degraded = true;
			dev_warn(data->dev,
				 "CPU clock changed but conservative voltage/MIF cleanup failed: %d\n",
				 ret);
			ret = 0;
		}
	}

out_unlock:
	mutex_unlock(&cluster->lock);
	return ret;
}

static int exynos8890_policy_init(struct cpufreq_policy *policy)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	struct exynos8890_cluster *cluster = NULL;
	u32 resume_rate;
	int i;

	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (cpumask_test_cpu(policy->cpu, &data->cluster[i].cpus)) {
			cluster = &data->cluster[i];
			break;
		}
	if (!cluster)
		return -ENODEV;
	cpumask_copy(policy->cpus, &cluster->cpus);
	cpumask_copy(policy->related_cpus, &cluster->cpus);
	policy->driver_data = cluster;
	policy->freq_table = cluster->table;
	policy->cpuinfo.transition_latency = EXYNOS8890_TRANSITION_LATENCY;
	policy->cur = exynos8890_cpufreq_get(policy->cpu);
	if (cluster->ect.resume_level < 0)
		return -EINVAL;
	resume_rate = cluster->ect.levels[cluster->ect.resume_level].rate_khz;
	for (i = 0; i < cluster->num_table; i++)
		if (cluster->table[i].frequency == resume_rate) {
			policy->suspend_freq = resume_rate;
			break;
		}
	if (i == cluster->num_table)
		return -EINVAL;
	return cpufreq_table_validate_and_sort(policy);
}

static struct cpufreq_driver exynos8890_cpufreq_driver = {
	.name = "exynos8890",
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = exynos8890_target_index,
	.get = exynos8890_cpufreq_get,
	.init = exynos8890_policy_init,
	.suspend = cpufreq_generic_suspend,
};

static struct clk *exynos8890_get_cluster_clock(struct platform_device *pdev,
						const char *prefix,
						const char *suffix)
{
	char name[64];

	snprintf(name, sizeof(name), "%s-%s", prefix, suffix);
	return devm_clk_get(&pdev->dev, name);
}

static int exynos8890_check_cluster_clocks(struct exynos8890_cluster *cluster)
{
	struct clk *clocks[] = {
		cluster->pll, cluster->pll_user, cluster->bus_user, cluster->mux,
		cluster->root, cluster->switch_div, cluster->switch_gate,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(clocks); i++)
		if (IS_ERR(clocks[i]))
			return PTR_ERR(clocks[i]);
	return 0;
}

static int exynos8890_init_cluster(struct platform_device *pdev,
				   struct exynos8890_cluster *cluster,
				   const char *prefix, const char *domain)
{
	char clock_name[64];
	unsigned int n = 0;
	int cpu, i, ret;

	cluster->name = prefix;
	ret = exynos8890_ect_get_cpu_domain(domain, &cluster->ect);
	if (ret)
		return ret;

	cluster->pll = exynos8890_get_cluster_clock(pdev, prefix, "pll");
	cluster->pll_user = exynos8890_get_cluster_clock(pdev, prefix, "pll-user");
	cluster->bus_user = exynos8890_get_cluster_clock(pdev, prefix, "bus-user");
	cluster->mux = exynos8890_get_cluster_clock(pdev, prefix, "mux");
	cluster->root = exynos8890_get_cluster_clock(pdev, prefix, "root");
	cluster->switch_div = exynos8890_get_cluster_clock(pdev, prefix,
							   "switch-div");
	cluster->switch_gate = exynos8890_get_cluster_clock(pdev, prefix,
							    "switch-gate");
	ret = exynos8890_check_cluster_clocks(cluster);
	if (ret)
		return ret;

	for (i = 0; i < cluster->ect.num_levels; i++) {
		if (!cluster->ect.levels[i].enabled)
			continue;
		if (cluster->ect.levels[i].rate_khz > cluster->max_supported_khz)
			continue;
		if (clk_round_rate(cluster->pll,
				   cluster->ect.levels[i].rate_khz * 1000UL) !=
		    cluster->ect.levels[i].rate_khz * 1000UL)
			continue;
		cluster->table[n].driver_data = i;
		cluster->table[n++].frequency = cluster->ect.levels[i].rate_khz;
	}
	cluster->table[n].frequency = CPUFREQ_TABLE_END;
	if (!n)
		return -EINVAL;
	cluster->num_table = n;

	for (i = 0; i < cluster->ect.num_clocks; i++) {
		cluster->members[i] = devm_clk_get(&pdev->dev,
						  cluster->ect.clock_names[i]);
		if (IS_ERR(cluster->members[i]))
			return PTR_ERR(cluster->members[i]);
	}
	cluster->regulator = devm_regulator_get(&pdev->dev, prefix);
	if (IS_ERR(cluster->regulator))
		return PTR_ERR(cluster->regulator);
	snprintf(clock_name, sizeof(clock_name), "samsung,%s-sysreg", prefix);
	cluster->ema = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       clock_name);
	if (IS_ERR(cluster->ema))
		return PTR_ERR(cluster->ema);
	mutex_init(&cluster->lock);
	cpumask_clear(&cluster->cpus);
	for_each_possible_cpu(cpu) {
		struct device_node *np = of_get_cpu_node(cpu, NULL);
		const char *compatible = prefix[0] == 'a' ? "arm,cortex-a53" :
							       "arm,mongoose-m1";

		if (np && of_device_is_compatible(np, compatible))
			cpumask_set_cpu(cpu, &cluster->cpus);
		of_node_put(np);
	}
	return cpumask_empty(&cluster->cpus) ? -ENODEV : 0;
}

static int exynos8890_seed_mif_floor(struct exynos8890_cpufreq *data,
				     unsigned int index)
{
	struct exynos8890_cluster *cluster = &data->cluster[index];
	u32 rate = clk_get_rate(cluster->root) / 1000;
	u32 mif = exynos8890_mif_lock(cluster, rate);
	int i;

	if (mif == U32_MAX)
		return -EINVAL;
	for (i = 0; i < cluster->num_table; i++)
		if (cluster->table[i].frequency == rate)
			return exynos8890_update_mif(data, index, mif);
	return -EINVAL;
}

static int exynos8890_cpufreq_probe(struct platform_device *pdev)
{
	struct exynos8890_cpufreq *data;
	struct device_node *supplier_node;
	u32 apm_status;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = &pdev->dev;
	data->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						    "samsung,pmu");
	if (IS_ERR(data->pmu))
		return PTR_ERR(data->pmu);

	supplier_node = of_parse_phandle(pdev->dev.of_node, "samsung,apm", 0);
	if (!supplier_node)
		return -EINVAL;
	data->apm_dev = bus_find_device_by_of_node(&platform_bus_type,
						   supplier_node);
	of_node_put(supplier_node);
	if (!data->apm_dev || !device_is_bound(data->apm_dev)) {
		ret = -EPROBE_DEFER;
		goto out_put_apm;
	}
	data->apm_link = device_link_add(&pdev->dev, data->apm_dev,
					 DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!data->apm_link) {
		ret = -EINVAL;
		goto out_put_apm;
	}
	ret = regmap_read(data->pmu, EXYNOS8890_APM_STATUS, &apm_status);
	if (ret)
		goto out_put_apm;
	if (apm_status & EXYNOS8890_APM_STATUS_ON) {
		ret = dev_err_probe(&pdev->dev, -EBUSY,
				    "APM firmware still owns closed-loop DVFS\n");
		goto out_put_apm;
	}
	data->apm_claim = symbol_get(exynos8890_apm_dvfs_claim);
	if (!data->apm_claim) {
		ret = -EPROBE_DEFER;
		goto out_put_apm;
	}
	ret = data->apm_claim(data->apm_dev, true);
	if (ret)
		goto out_put_apm_symbol;
	data->apm_claimed = true;

	supplier_node = of_parse_phandle(pdev->dev.of_node,
					 "samsung,mif-devfreq", 0);
	if (!supplier_node) {
		ret = -EINVAL;
		goto out_release_apm;
	}
	data->mif_dev = bus_find_device_by_of_node(&platform_bus_type,
						   supplier_node);
	of_node_put(supplier_node);
	if (!data->mif_dev || !device_is_bound(data->mif_dev)) {
		ret = -EPROBE_DEFER;
		goto out_put_mif;
	}
	data->mif_link = device_link_add(&pdev->dev, data->mif_dev,
					 DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!data->mif_link) {
		ret = -EINVAL;
		goto out_put_mif;
	}
	ret = dev_pm_qos_add_request(data->mif_dev, &data->mif_qos[0],
				     DEV_PM_QOS_MIN_FREQUENCY, 0);
	if (ret)
		goto out_put_mif;
	ret = dev_pm_qos_add_request(data->mif_dev, &data->mif_qos[1],
				     DEV_PM_QOS_MIN_FREQUENCY, 0);
	if (ret)
		goto out_remove_qos0;

	data->cluster[0].bus_locks = apollo_bus_locks;
	data->cluster[0].num_bus_locks = ARRAY_SIZE(apollo_bus_locks);
	data->cluster[0].max_supported_khz = 1586000;
	data->cluster[1].bus_locks = mongoose_bus_locks;
	data->cluster[1].num_bus_locks = ARRAY_SIZE(mongoose_bus_locks);
	data->cluster[1].max_supported_khz = 2288000;
	ret = exynos8890_init_cluster(pdev, &data->cluster[0], "apollo",
				      "dvfs_little");
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_init_cluster(pdev, &data->cluster[1], "mongoose",
				      "dvfs_big");
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_seed_mif_floor(data, 0);
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_seed_mif_floor(data, 1);
	if (ret)
		goto out_remove_qos;

	platform_set_drvdata(pdev, data);
	mutex_lock(&exynos8890_driver_lock);
	if (exynos8890_cpufreq_data) {
		ret = -EBUSY;
		goto out_unlock;
	}
	exynos8890_cpufreq_data = data;
	ret = cpufreq_register_driver(&exynos8890_cpufreq_driver);
	if (ret)
		exynos8890_cpufreq_data = NULL;
	mutex_unlock(&exynos8890_driver_lock);
	if (!ret)
		return 0;
	goto out_remove_qos;

out_unlock:
	mutex_unlock(&exynos8890_driver_lock);
out_remove_qos:
	dev_pm_qos_remove_request(&data->mif_qos[1]);
out_remove_qos0:
	dev_pm_qos_remove_request(&data->mif_qos[0]);
out_put_mif:
	put_device(data->mif_dev);
out_release_apm:
	if (data->apm_claimed)
		data->apm_claim(data->apm_dev, false);
out_put_apm_symbol:
	if (data->apm_claim)
		symbol_put(exynos8890_apm_dvfs_claim);
out_put_apm:
	put_device(data->apm_dev);
	return ret;
}

static void exynos8890_cpufreq_remove(struct platform_device *pdev)
{
	struct exynos8890_cpufreq *data = platform_get_drvdata(pdev);

	mutex_lock(&exynos8890_driver_lock);
	cpufreq_unregister_driver(&exynos8890_cpufreq_driver);
	if (exynos8890_cpufreq_data == data)
		exynos8890_cpufreq_data = NULL;
	mutex_unlock(&exynos8890_driver_lock);
	dev_pm_qos_remove_request(&data->mif_qos[1]);
	dev_pm_qos_remove_request(&data->mif_qos[0]);
	if (data->apm_claimed)
		data->apm_claim(data->apm_dev, false);
	if (data->apm_claim)
		symbol_put(exynos8890_apm_dvfs_claim);
	put_device(data->mif_dev);
	put_device(data->apm_dev);
}

static const struct of_device_id exynos8890_cpufreq_of_match[] = {
	{ .compatible = "samsung,exynos8890-cpufreq" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpufreq_of_match);

static struct platform_driver exynos8890_cpufreq_platdrv = {
	.probe = exynos8890_cpufreq_probe,
	.remove = exynos8890_cpufreq_remove,
	.driver = {
		.name = "exynos8890-cpufreq",
		.of_match_table = exynos8890_cpufreq_of_match,
	},
};
module_platform_driver(exynos8890_cpufreq_platdrv);

MODULE_DESCRIPTION("Samsung Exynos8890 dual-cluster CPUFreq driver");
MODULE_LICENSE("GPL");
