// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 dual-cluster CPU frequency scaling.
 *
 * Thin wrapper over the ported pwrcal DFS layer
 * (drivers/soc/samsung/pwrcal8890/), matching vendor's own
 * exynos-mp-cpufreq-cal.c calling convention: cal_dfs_set_rate() owns the
 * entire PLL-switch/divider dance and cal_dfs_set_ema() owns per-voltage
 * EMA tuning internally - this driver only owns what's Linux-framework
 * specific and outside pwrcal's scope: the cpufreq_driver glue, regulator
 * voltage sequencing around each rate change, and the MIF PM QoS floor.
 *
 * It still refuses to probe unless the APM Cortex-M3 is off and a MIF
 * devfreq provider is available, so Linux never races autonomous DVFS
 * firmware or omits the vendor CPU-to-memory frequency floor.
 */

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
#include <linux/soc/samsung/exynos8890-pwrcal.h>
#include <linux/mfd/syscon.h>

#define EXYNOS8890_CLUSTERS		2
#define EXYNOS8890_APOLLO		0
#define EXYNOS8890_MONGOOSE		1
#define EXYNOS8890_APM_STATUS		0x2504
#define EXYNOS8890_APM_STATUS_ON	BIT(0)
#define EXYNOS8890_COLD_OFFSET_UV	25000
#define EXYNOS8890_COLD_LIMIT_UV	1350000
#define EXYNOS8890_TRANSITION_LATENCY	100000

/* Matches cal_dfs_get_rate_asv_table()'s own internal "rate[48]" cap. */
#define EXYNOS8890_DFS_MAX_LEVELS	48

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

struct exynos8890_cluster {
	const char *name;
	unsigned int vclk_id;
	struct cpufreq_frequency_table table[EXYNOS8890_DFS_MAX_LEVELS + 1];
	u32 voltage_uv[EXYNOS8890_DFS_MAX_LEVELS];
	u32 num_table;
	struct regulator *regulator;
	const struct exynos8890_bus_lock *bus_locks;
	size_t num_bus_locks;
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

static unsigned int exynos8890_cpufreq_get(unsigned int cpu)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	int i;

	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (cpumask_test_cpu(cpu, &data->cluster[i].cpus))
			return cal_dfs_get_rate(data->cluster[i].vclk_id);
	return 0;
}

static int exynos8890_target_index(struct cpufreq_policy *policy,
				   unsigned int index)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	struct exynos8890_cluster *cluster = policy->driver_data;
	unsigned int new_rate = cluster->table[index].frequency;
	unsigned int old_rate = exynos8890_cpufreq_get(policy->cpu);
	unsigned int cluster_idx = cluster - data->cluster;
	u32 new_voltage = cluster->voltage_uv[index];
	u32 mif_lock, old_mif_lock;
	int delay, old_voltage, ret;

	if (new_rate == old_rate)
		return 0;
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
		ret = exynos8890_update_mif(data, cluster_idx, mif_lock);
		if (ret)
			goto out_unlock;
		ret = regulator_set_voltage_triplet(cluster->regulator, new_voltage,
						    new_voltage, new_voltage);
		if (ret) {
			exynos8890_update_mif(data, cluster_idx, old_mif_lock);
			goto out_unlock;
		}
		delay = regulator_set_voltage_time(cluster->regulator, old_voltage,
						   new_voltage);
		if (delay > 0)
			usleep_range(delay, delay + 50);
		if (cal_dfs_set_ema(cluster->vclk_id, new_voltage) < 0) {
			regulator_set_voltage_triplet(cluster->regulator, old_voltage,
						      old_voltage, old_voltage);
			exynos8890_update_mif(data, cluster_idx, old_mif_lock);
			ret = -EIO;
			goto out_unlock;
		}
	}

	if (cal_dfs_set_rate(cluster->vclk_id, new_rate) < 0) {
		cluster->faulted = true;
		ret = -EIO;
		goto out_unlock;
	}
	ret = 0;

	if (new_rate < old_rate) {
		ret = cal_dfs_set_ema(cluster->vclk_id, new_voltage) < 0 ? -EIO : 0;
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
			ret = exynos8890_update_mif(data, cluster_idx, mif_lock);
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

static int exynos8890_init_cluster(struct platform_device *pdev,
				   struct exynos8890_cluster *cluster,
				   const char *prefix, const char *vclk_name)
{
	struct dvfs_rate_volt asv_table[EXYNOS8890_DFS_MAX_LEVELS];
	int num_levels, i, cpu;

	cluster->name = prefix;
	cluster->vclk_id = cal_dfs_get((char *)vclk_name);
	if (cluster->vclk_id == 0xFFFFFFFF)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unknown pwrcal DFS domain %s\n", vclk_name);

	num_levels = cal_dfs_get_rate_asv_table(cluster->vclk_id, asv_table);
	if (num_levels <= 0)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "empty pwrcal ASV rate table for %s\n",
				     vclk_name);
	if (num_levels > EXYNOS8890_DFS_MAX_LEVELS)
		num_levels = EXYNOS8890_DFS_MAX_LEVELS;

	for (i = 0; i < num_levels; i++) {
		cluster->table[i].driver_data = i;
		cluster->table[i].frequency = asv_table[i].rate;
		cluster->voltage_uv[i] = asv_table[i].volt;
	}
	cluster->table[num_levels].frequency = CPUFREQ_TABLE_END;
	cluster->num_table = num_levels;

	cluster->regulator = devm_regulator_get(&pdev->dev, prefix);
	if (IS_ERR(cluster->regulator))
		return PTR_ERR(cluster->regulator);
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
	u32 rate = cal_dfs_get_rate(cluster->vclk_id);
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

	data->cluster[EXYNOS8890_APOLLO].bus_locks = apollo_bus_locks;
	data->cluster[EXYNOS8890_APOLLO].num_bus_locks = ARRAY_SIZE(apollo_bus_locks);
	data->cluster[EXYNOS8890_MONGOOSE].bus_locks = mongoose_bus_locks;
	data->cluster[EXYNOS8890_MONGOOSE].num_bus_locks = ARRAY_SIZE(mongoose_bus_locks);
	ret = exynos8890_init_cluster(pdev, &data->cluster[EXYNOS8890_APOLLO],
				      "apollo", "dvfs_little");
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_init_cluster(pdev, &data->cluster[EXYNOS8890_MONGOOSE],
				      "mongoose", "dvfs_big");
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_seed_mif_floor(data, EXYNOS8890_APOLLO);
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_seed_mif_floor(data, EXYNOS8890_MONGOOSE);
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
