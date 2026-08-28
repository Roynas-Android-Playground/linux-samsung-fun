// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 dual-cluster CPU frequency scaling.
 *
 * CPUFreq owns policy, voltage and cross-domain constraints.  Native
 * aggregate CCF clocks own every CPU clock-register transition.  Immutable
 * calibration is copied into ordinary OPP tables at probe; writable PWRCAL is
 * intentionally absent from this path.
 */

#include <linux/clk.h>
#include <linux/clk/samsung.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/smp.h>
#include <linux/soc/samsung/exynos8890-apm.h>
#include <linux/soc/samsung/exynos8890-calibration.h>
#include <linux/soc/samsung/exynos8890-cpuidle.h>
#include <linux/thermal.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#define EXYNOS8890_CLUSTERS		2
#define EXYNOS8890_APOLLO		0
#define EXYNOS8890_MONGOOSE		1
#define EXYNOS8890_APM_STATUS		0x2504
#define EXYNOS8890_APM_RUN		BIT(0)
#define EXYNOS8890_COLD_TEMP_MC		15000
#define EXYNOS8890_COLD_OFFSET_UV	25000
#define EXYNOS8890_COLD_LIMIT_UV	1350000
#define EXYNOS8890_COLD_POLL_MS		1000
#define EXYNOS8890_TRANSITION_LATENCY	100000

struct exynos8890_bus_lock {
	unsigned long rate_hz;
	unsigned long mif_hz;
};

#define CPU_BUS_LOCK(_cpu_khz, _mif_khz) \
	{ (unsigned long)(_cpu_khz) * HZ_PER_KHZ, \
	  (unsigned long)(_mif_khz) * HZ_PER_KHZ }

static const struct exynos8890_bus_lock apollo_bus_locks[] = {
	CPU_BUS_LOCK(1976000, 0), CPU_BUS_LOCK(1898000, 0),
	CPU_BUS_LOCK(1794000, 0), CPU_BUS_LOCK(1690000, 0),
	CPU_BUS_LOCK(1586000, 1014000), CPU_BUS_LOCK(1482000, 1014000),
	CPU_BUS_LOCK(1378000, 1014000), CPU_BUS_LOCK(1274000, 1014000),
	CPU_BUS_LOCK(1170000, 845000), CPU_BUS_LOCK(1066000, 845000),
	CPU_BUS_LOCK(962000, 845000), CPU_BUS_LOCK(858000, 676000),
	CPU_BUS_LOCK(754000, 676000), CPU_BUS_LOCK(650000, 546000),
	CPU_BUS_LOCK(546000, 421000), CPU_BUS_LOCK(442000, 0),
	CPU_BUS_LOCK(338000, 0), CPU_BUS_LOCK(234000, 0),
	CPU_BUS_LOCK(130000, 0),
};

static const struct exynos8890_bus_lock mongoose_bus_locks[] = {
	CPU_BUS_LOCK(3016000, 1794000), CPU_BUS_LOCK(2912000, 1794000),
	CPU_BUS_LOCK(2808000, 1794000), CPU_BUS_LOCK(2704000, 1794000),
	CPU_BUS_LOCK(2600000, 1794000), CPU_BUS_LOCK(2496000, 1716000),
	CPU_BUS_LOCK(2392000, 1716000), CPU_BUS_LOCK(2288000, 1716000),
	CPU_BUS_LOCK(2184000, 1539000), CPU_BUS_LOCK(2080000, 1539000),
	CPU_BUS_LOCK(1976000, 1352000), CPU_BUS_LOCK(1872000, 1144000),
	CPU_BUS_LOCK(1768000, 1144000), CPU_BUS_LOCK(1664000, 1014000),
	CPU_BUS_LOCK(1560000, 1014000), CPU_BUS_LOCK(1456000, 845000),
	CPU_BUS_LOCK(1352000, 845000), CPU_BUS_LOCK(1248000, 676000),
	CPU_BUS_LOCK(1144000, 546000), CPU_BUS_LOCK(1040000, 546000),
	CPU_BUS_LOCK(936000, 421000), CPU_BUS_LOCK(832000, 421000),
	CPU_BUS_LOCK(728000, 421000), CPU_BUS_LOCK(624000, 0),
	CPU_BUS_LOCK(520000, 0), CPU_BUS_LOCK(416000, 0),
	CPU_BUS_LOCK(312000, 0), CPU_BUS_LOCK(208000, 0),
};

struct exynos8890_cluster {
	const char *name;
	enum exynos8890_calib_domain_id calib_id;
	struct device *cpu_dev;
	struct clk *root_clk;
	struct regulator *regulator;
	const struct exynos8890_bus_lock *bus_locks;
	size_t num_bus_locks;
	struct cpumask cpus;
	struct mutex lock;
	struct thermal_zone_device *thermal_zone;
	const char *thermal_zone_name;
	bool cold_active;
	bool cold_known;
	bool opps_added;
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
	struct delayed_work cold_work;
	bool cold_polling;
};

static DEFINE_MUTEX(exynos8890_driver_lock);
static DEFINE_MUTEX(exynos8890_transition_lock);
static struct exynos8890_cpufreq *exynos8890_cpufreq_data;

static unsigned long exynos8890_mif_lock(struct exynos8890_cluster *cluster,
					 unsigned long rate_hz)
{
	unsigned long mif_hz = cluster->bus_locks[0].mif_hz;
	int i;

	for (i = 0; i < cluster->num_bus_locks; i++) {
		if (rate_hz > cluster->bus_locks[i].rate_hz)
			break;
		mif_hz = cluster->bus_locks[i].mif_hz;
	}
	return mif_hz;
}

static int exynos8890_update_mif(struct exynos8890_cpufreq *data,
				 unsigned int cluster, unsigned long mif_hz)
{
	s32 qos_khz;
	int ret;

	/* The devfreq QoS ABI is kHz even though clocks and OPPs use Hz. */
	qos_khz = mif_hz ? DIV_ROUND_UP(mif_hz, HZ_PER_KHZ) : 0;
	ret = dev_pm_qos_update_request(&data->mif_qos[cluster], qos_khz);
	return ret < 0 ? ret : 0;
}

static unsigned int exynos8890_cpufreq_get(unsigned int cpu)
{
	struct exynos8890_cpufreq *data = READ_ONCE(exynos8890_cpufreq_data);
	int i;

	if (!data)
		return 0;
	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (cpumask_test_cpu(cpu, &data->cluster[i].cpus))
			return clk_get_rate(data->cluster[i].root_clk) /
				HZ_PER_KHZ;
	return 0;
}

static void exynos8890_cpd_wake(void *unused)
{
}

static bool exynos8890_cluster_is_cold(struct exynos8890_cluster *cluster,
					bool *known)
{
	int temperature;

	*known = false;
	if (!cluster->thermal_zone) {
		cluster->thermal_zone = thermal_zone_get_zone_by_name(
			cluster->thermal_zone_name);
		if (IS_ERR(cluster->thermal_zone)) {
			cluster->thermal_zone = NULL;
			return cluster->cold_active;
		}
	}
	if (thermal_zone_get_temp(cluster->thermal_zone, &temperature))
		return cluster->cold_active;
	*known = true;
	return temperature < EXYNOS8890_COLD_TEMP_MC;
}

static u32 exynos8890_effective_voltage(u32 voltage_uv, bool cold)
{
	if (!cold || voltage_uv > EXYNOS8890_COLD_LIMIT_UV)
		return voltage_uv;
	return min(voltage_uv + EXYNOS8890_COLD_OFFSET_UV,
		   EXYNOS8890_COLD_LIMIT_UV);
}

static int exynos8890_set_regulator(struct exynos8890_cluster *cluster,
				     int old_uv, u32 new_uv)
{
	int delay, ret;

	ret = regulator_set_voltage_triplet(cluster->regulator, new_uv,
					    new_uv, new_uv);
	if (ret)
		return ret;
	delay = regulator_set_voltage_time(cluster->regulator, old_uv, new_uv);
	if (delay > 0)
		usleep_range(delay, delay + 50);
	return 0;
}

/* EMA follows a voltage raise and precedes a voltage reduction. */
static int exynos8890_set_voltage(struct exynos8890_cluster *cluster,
				   int old_uv, u32 new_uv)
{
	int actual_uv, delay, rollback_actual, rollback_ret, set_ret, ret;

	if (new_uv > old_uv) {
		ret = exynos8890_set_regulator(cluster, old_uv, new_uv);
		if (ret) {
			set_ret = ret;
			actual_uv = regulator_get_voltage(cluster->regulator);
			if (actual_uv == old_uv)
				return set_ret;
			if (actual_uv == new_uv) {
				/*
				 * The selector changed despite the error.  Wait for the
				 * physical ramp before matching EMA to the higher rail.
				 */
				delay = regulator_set_voltage_time(cluster->regulator,
							   old_uv, new_uv);
				if (delay < 0) {
					ret = delay;
					goto rollback_raised_rail;
				}
				if (delay > 0)
					usleep_range(delay, delay + 50);
				ret = exynos8890_cpuclk_set_ema(cluster->root_clk,
							      new_uv);
				if (!ret) {
					cluster->voltage_degraded = true;
					dev_warn(cluster->cpu_dev,
						 "%s regulator reported %d after reaching %u uV\n",
						 cluster->name, set_ret, new_uv);
					return 0;
				}

				/* EMA may be partially updated; restore it first. */
				rollback_ret = exynos8890_cpuclk_set_ema(
					cluster->root_clk, old_uv);
				if (rollback_ret) {
					cluster->faulted = true;
					cluster->voltage_degraded = true;
					dev_crit(cluster->cpu_dev,
						 "%s cannot restore EMA after raised-rail error %d: %d\n",
						 cluster->name, ret, rollback_ret);
					return ret;
				}
			} else {
				ret = set_ret;
			}

rollback_raised_rail:
			rollback_ret = exynos8890_set_regulator(cluster,
					actual_uv > 0 ? actual_uv : new_uv, old_uv);
			rollback_actual = regulator_get_voltage(cluster->regulator);
			if (rollback_actual == old_uv) {
				dev_warn(cluster->cpu_dev,
					 "%s recovered ambiguous voltage raise error %d\n",
					 cluster->name, ret);
				return ret;
			}

			cluster->faulted = true;
			cluster->voltage_degraded = true;
			dev_crit(cluster->cpu_dev,
				 "%s raised-rail state is unverified after %d: rollback %d, rail %d uV\n",
				 cluster->name, ret, rollback_ret, rollback_actual);
			return ret;
		}
		ret = exynos8890_cpuclk_set_ema(cluster->root_clk, new_uv);
		if (!ret)
			return 0;

		/* Restore EMA while the rail is still at the safer high voltage. */
		rollback_ret = exynos8890_cpuclk_set_ema(cluster->root_clk,
							 old_uv);
		if (!rollback_ret)
			rollback_ret = exynos8890_set_regulator(cluster, new_uv,
							   old_uv);
		if (rollback_ret) {
			cluster->faulted = true;
			cluster->voltage_degraded = true;
			dev_crit(cluster->cpu_dev,
				 "%s voltage/EMA rollback failed after %d: %d\n",
				 cluster->name, ret, rollback_ret);
		}
		return ret;
	}
	ret = exynos8890_cpuclk_set_ema(cluster->root_clk, new_uv);
	if (ret) {
		rollback_ret = exynos8890_cpuclk_set_ema(cluster->root_clk,
							 old_uv);
		if (rollback_ret) {
			cluster->faulted = true;
			cluster->voltage_degraded = true;
			dev_crit(cluster->cpu_dev,
				 "%s EMA rollback failed after %d: %d\n",
				 cluster->name, ret, rollback_ret);
		}
		return ret;
	}
	if (new_uv == old_uv)
		return 0;
	ret = exynos8890_set_regulator(cluster, old_uv, new_uv);
	if (!ret)
		return 0;

	/* A failed voltage reduction is recoverable only at the old voltage. */
	actual_uv = regulator_get_voltage(cluster->regulator);
	if (actual_uv == new_uv) {
		cluster->voltage_degraded = true;
		dev_warn(cluster->cpu_dev,
			 "%s regulator reported %d after reaching %u uV\n",
			 cluster->name, ret, new_uv);
		return 0;
	}
	if (actual_uv == old_uv) {
		rollback_ret = exynos8890_cpuclk_set_ema(cluster->root_clk,
							 old_uv);
		if (!rollback_ret)
			return ret;
	} else {
		rollback_ret = actual_uv < 0 ? actual_uv : -EUCLEAN;
	}

	cluster->faulted = true;
	cluster->voltage_degraded = true;
	dev_crit(cluster->cpu_dev,
		 "%s voltage/EMA state is unverified after %d: %d (rail %d uV)\n",
		 cluster->name, ret, rollback_ret, actual_uv);
	return ret;
}

static int exynos8890_opp_voltage(struct exynos8890_cluster *cluster,
				  unsigned long rate_hz, u32 *voltage_uv)
{
	struct dev_pm_opp *opp;

	opp = dev_pm_opp_find_freq_exact(cluster->cpu_dev, rate_hz, true);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	*voltage_uv = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	return *voltage_uv ? 0 : -EINVAL;
}

static int exynos8890_target_index(struct cpufreq_policy *policy,
				   unsigned int index)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	struct exynos8890_cluster *cluster = policy->driver_data;
	unsigned int cluster_idx = cluster - data->cluster;
	unsigned long new_rate =
		(unsigned long)policy->freq_table[index].frequency * HZ_PER_KHZ;
	unsigned long old_rate, new_mif, old_mif, actual_rate;
	u32 new_voltage;
	bool cold, cold_known, cpd_held = false;
	int old_voltage, clock_error, ret;

	ret = exynos8890_opp_voltage(cluster, new_rate, &new_voltage);
	if (ret)
		return ret;
	mutex_lock(&exynos8890_transition_lock);
	mutex_lock(&cluster->lock);
	if (cluster->faulted) {
		ret = -EIO;
		goto out_unlock;
	}
	clock_error = exynos8890_cpuclk_get_error(cluster->root_clk);
	if (clock_error) {
		cluster->faulted = true;
		ret = clock_error;
		goto out_unlock;
	}

	old_rate = clk_get_rate(cluster->root_clk);
	if (!old_rate) {
		ret = -EIO;
		goto out_unlock;
	}
	cold = exynos8890_cluster_is_cold(cluster, &cold_known);
	new_voltage = exynos8890_effective_voltage(new_voltage, cold);
	new_mif = exynos8890_mif_lock(cluster, new_rate);
	old_mif = exynos8890_mif_lock(cluster, old_rate);
	old_voltage = regulator_get_voltage(cluster->regulator);
	if (old_voltage < 0) {
		ret = old_voltage;
		goto out_unlock;
	}

	if (cluster_idx == EXYNOS8890_MONGOOSE) {
		unsigned int state_cpu = cpumask_first(&cluster->cpus);
		unsigned int wake_cpu = cpumask_any_and(&cluster->cpus,
							 cpu_online_mask);

		exynos8890_cpd_block();
		cpd_held = true;
		if (exynos8890_cpd_is_active(state_cpu)) {
			if (wake_cpu >= nr_cpu_ids) {
				ret = -ENODEV;
				goto out_cpd;
			}

			ret = smp_call_function_single(wake_cpu,
						       exynos8890_cpd_wake,
						       NULL, true);
			if (ret)
				goto out_cpd;
			if (exynos8890_cpd_is_active(state_cpu)) {
				ret = -EBUSY;
				goto out_cpd;
			}
		}
	}

	if (new_mif > old_mif) {
		ret = exynos8890_update_mif(data, cluster_idx, new_mif);
		if (ret)
			goto out_cpd;
	}
	if (new_voltage > old_voltage) {
		ret = exynos8890_set_voltage(cluster, old_voltage, new_voltage);
		if (ret) {
			if (new_mif > old_mif)
				exynos8890_update_mif(data, cluster_idx, old_mif);
			goto out_cpd;
		}
	}

	ret = clk_set_rate(cluster->root_clk, new_rate);
	clock_error = exynos8890_cpuclk_get_error(cluster->root_clk);
	actual_rate = clk_get_rate(cluster->root_clk);
	if (!ret && clock_error)
		ret = clock_error;
	if (!ret && actual_rate != new_rate)
		ret = -EIO;
	if (ret) {
		/* Clock owner attempted rollback.  Keep voltage and MIF high. */
		cluster->faulted = true;
		dev_err(data->dev,
			"%s clock transition %lu -> %lu Hz failed (%lu Hz): %d\n",
			cluster->name, old_rate, new_rate, actual_rate, ret);
		goto out_cpd;
	}

	if (new_voltage < old_voltage) {
		ret = exynos8890_set_voltage(cluster, old_voltage, new_voltage);
		if (ret) {
			cluster->voltage_degraded = true;
			if (cluster->faulted) {
				dev_crit(data->dev,
					 "%s frequency changed with an unverified voltage state: %d\n",
					 cluster->name, ret);
				goto out_cpd;
			} else {
				dev_warn(data->dev,
					 "%s frequency changed; safe high-voltage cleanup failed: %d\n",
					 cluster->name, ret);
				ret = 0;
			}
		}
	}
	if (new_mif < old_mif) {
		int qos_ret = exynos8890_update_mif(data, cluster_idx, new_mif);

		if (qos_ret) {
			cluster->voltage_degraded = true;
			dev_warn(data->dev, "%s MIF floor cleanup failed: %d\n",
				 cluster->name, qos_ret);
		}
	}
	if (cold_known) {
		cluster->cold_known = true;
		cluster->cold_active = cold;
	}

out_cpd:
	if (cpd_held)
		exynos8890_cpd_unblock();
out_unlock:
	mutex_unlock(&cluster->lock);
	mutex_unlock(&exynos8890_transition_lock);
	return ret;
}

static int exynos8890_policy_init(struct cpufreq_policy *policy)
{
	struct exynos8890_cpufreq *data = exynos8890_cpufreq_data;
	struct exynos8890_cluster *cluster = NULL;
	struct cpufreq_frequency_table *table;
	int i, ret;

	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (cpumask_test_cpu(policy->cpu, &data->cluster[i].cpus)) {
			cluster = &data->cluster[i];
			break;
		}
	if (!cluster)
		return -ENODEV;
	ret = dev_pm_opp_init_cpufreq_table(cluster->cpu_dev, &table);
	if (ret)
		return ret;
	cpumask_copy(policy->cpus, &cluster->cpus);
	cpumask_copy(policy->related_cpus, &cluster->cpus);
	policy->driver_data = cluster;
	policy->freq_table = table;
	policy->clk = cluster->root_clk;
	policy->cpuinfo.transition_latency = EXYNOS8890_TRANSITION_LATENCY;
	policy->cur = clk_get_rate(cluster->root_clk) / HZ_PER_KHZ;
	ret = cpufreq_table_validate_and_sort(policy);
	if (ret)
		dev_pm_opp_free_cpufreq_table(cluster->cpu_dev,
					      &policy->freq_table);
	return ret;
}

static void exynos8890_policy_exit(struct cpufreq_policy *policy)
{
	struct exynos8890_cluster *cluster = policy->driver_data;

	dev_pm_opp_free_cpufreq_table(cluster->cpu_dev, &policy->freq_table);
}

static struct cpufreq_driver exynos8890_cpufreq_driver = {
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.name = "exynos8890",
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = exynos8890_target_index,
	.get = exynos8890_cpufreq_get,
	.init = exynos8890_policy_init,
	.exit = exynos8890_policy_exit,
	.suspend = cpufreq_generic_suspend,
	.register_em = cpufreq_register_em_with_opp,
};

static int exynos8890_add_opps(struct exynos8890_cluster *cluster)
{
	const struct exynos8890_calib_domain *domain;
	unsigned int i, added = 0;
	int ret;

	domain = exynos8890_calib_get_domain(cluster->calib_id);
	if (IS_ERR(domain))
		return PTR_ERR(domain);
	for (i = 0; i < domain->num_opps; i++) {
		const struct exynos8890_calib_opp *opp = &domain->opps[i];

		if (!opp->enabled)
			continue;
		if (!opp->rate_hz || !opp->voltage_uv) {
			ret = -EINVAL;
			goto remove;
		}
		ret = dev_pm_opp_add(cluster->cpu_dev, opp->rate_hz,
				     opp->voltage_uv);
		if (ret)
			goto remove;
		added++;
	}
	if (!added)
		return -ENODEV;
	ret = dev_pm_opp_set_sharing_cpus(cluster->cpu_dev, &cluster->cpus);
	if (ret)
		goto remove;
	cluster->opps_added = true;
	return 0;
remove:
	dev_pm_opp_remove_all_dynamic(cluster->cpu_dev);
	return ret;
}

static int exynos8890_init_cluster(struct platform_device *pdev,
				   struct exynos8890_cluster *cluster,
				   const char *prefix,
				   enum exynos8890_calib_domain_id calib_id)
{
	const char *compatible;
	int cpu, ret;

	cluster->name = prefix;
	cluster->calib_id = calib_id;
	cluster->thermal_zone_name = prefix[0] == 'a' ? "apollo-thermal" :
							    "mngs-thermal";
	cluster->root_clk = devm_clk_get(&pdev->dev, prefix);
	if (IS_ERR(cluster->root_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(cluster->root_clk),
				     "failed to get %s aggregate CPU clock\n",
				     prefix);
	ret = exynos8890_cpuclk_smpl_status(cluster->root_clk);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read %s SMPL status\n", prefix);
	if (ret)
		dev_warn(&pdev->dev,
			 "%s SMPL status is latched; hardware has no documented clear sequence\n",
			 prefix);
	cluster->regulator = devm_regulator_get(&pdev->dev, prefix);
	if (IS_ERR(cluster->regulator))
		return dev_err_probe(&pdev->dev, PTR_ERR(cluster->regulator),
				     "failed to get %s regulator\n", prefix);
	mutex_init(&cluster->lock);
	cpumask_clear(&cluster->cpus);
	compatible = prefix[0] == 'a' ? "arm,cortex-a53" : "arm,mongoose-m1";
	for_each_possible_cpu(cpu) {
		struct device_node *np = of_get_cpu_node(cpu, NULL);

		if (np && of_device_is_compatible(np, compatible))
			cpumask_set_cpu(cpu, &cluster->cpus);
		of_node_put(np);
	}
	if (cpumask_empty(&cluster->cpus))
		return -ENODEV;
	cluster->cpu_dev = get_cpu_device(cpumask_first(&cluster->cpus));
	if (!cluster->cpu_dev)
		return -ENODEV;
	ret = exynos8890_add_opps(cluster);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to create %s OPP table\n", prefix);
	return 0;
}

static int exynos8890_seed_mif_floor(struct exynos8890_cpufreq *data,
				     unsigned int index)
{
	struct exynos8890_cluster *cluster = &data->cluster[index];
	unsigned long rate_hz = clk_get_rate(cluster->root_clk);

	if (!rate_hz)
		return -EINVAL;
	return exynos8890_update_mif(data, index,
				     exynos8890_mif_lock(cluster, rate_hz));
}

static int exynos8890_initialize_cluster_voltage(
		struct exynos8890_cluster *cluster)
{
	struct dev_pm_opp *opp;
	unsigned long opp_rate, opp_voltage;
	u32 required_uv;
	int actual_uv, ret;

	opp_rate = clk_get_rate(cluster->root_clk);
	if (!opp_rate)
		return -EIO;
	opp = dev_pm_opp_find_freq_ceil(cluster->cpu_dev, &opp_rate);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	opp_voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	if (!opp_voltage || opp_voltage > U32_MAX)
		return -ERANGE;

	/* Until the sensor answers, assume the characterized cold condition. */
	cluster->cold_active = true;
	cluster->cold_known = false;
	required_uv = exynos8890_effective_voltage(opp_voltage, true);
	actual_uv = regulator_get_voltage(cluster->regulator);
	if (actual_uv < 0)
		return actual_uv;
	if (actual_uv < required_uv) {
		ret = exynos8890_set_voltage(cluster, actual_uv, required_uv);
		if (ret)
			return ret;
		actual_uv = regulator_get_voltage(cluster->regulator);
		if (actual_uv < 0)
			return actual_uv;
	}
	if (actual_uv < required_uv)
		return -ERANGE;

	return exynos8890_cpuclk_set_ema(cluster->root_clk, actual_uv);
}

static void exynos8890_cold_work(struct work_struct *work)
{
	struct exynos8890_cpufreq *data = container_of(
		to_delayed_work(work), struct exynos8890_cpufreq, cold_work);
	int i;

	mutex_lock(&exynos8890_transition_lock);
	for (i = 0; i < EXYNOS8890_CLUSTERS; i++) {
		struct exynos8890_cluster *cluster = &data->cluster[i];
		unsigned long rate_hz;
		bool cold, known;
		u32 voltage_uv;
		int old_uv, ret;

		mutex_lock(&cluster->lock);
		if (cluster->faulted)
			goto next;
		cold = exynos8890_cluster_is_cold(cluster, &known);
		if (!known || (cluster->cold_known &&
			       cold == cluster->cold_active))
			goto next;
		rate_hz = clk_get_rate(cluster->root_clk);
		ret = exynos8890_opp_voltage(cluster, rate_hz, &voltage_uv);
		if (ret)
			goto next;
		voltage_uv = exynos8890_effective_voltage(voltage_uv, cold);
		old_uv = regulator_get_voltage(cluster->regulator);
		if (old_uv < 0)
			goto next;
		ret = exynos8890_set_voltage(cluster, old_uv, voltage_uv);
		if (ret) {
			dev_warn(data->dev, "%s cold-voltage update failed: %d\n",
				 cluster->name, ret);
			goto next;
		}
		cluster->cold_known = true;
		cluster->cold_active = cold;
next:
		mutex_unlock(&cluster->lock);
	}
	mutex_unlock(&exynos8890_transition_lock);
	if (READ_ONCE(data->cold_polling))
		schedule_delayed_work(&data->cold_work,
				      msecs_to_jiffies(EXYNOS8890_COLD_POLL_MS));
}

static int __maybe_unused exynos8890_cpufreq_suspend(struct device *dev)
{
	struct exynos8890_cpufreq *data = dev_get_drvdata(dev);

	WRITE_ONCE(data->cold_polling, false);
	cancel_delayed_work_sync(&data->cold_work);
	/* Pair with an in-flight target transaction before suppliers suspend. */
	mutex_lock(&exynos8890_transition_lock);
	mutex_unlock(&exynos8890_transition_lock);

	return 0;
}

static int __maybe_unused exynos8890_cpufreq_resume(struct device *dev)
{
	struct exynos8890_cpufreq *data = dev_get_drvdata(dev);

	WRITE_ONCE(data->cold_polling, true);
	schedule_delayed_work(&data->cold_work,
			      msecs_to_jiffies(EXYNOS8890_COLD_POLL_MS));

	return 0;
}

static const struct dev_pm_ops exynos8890_cpufreq_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(exynos8890_cpufreq_suspend,
				exynos8890_cpufreq_resume)
};

static void exynos8890_remove_opps(struct exynos8890_cpufreq *data)
{
	int i;

	for (i = 0; i < EXYNOS8890_CLUSTERS; i++)
		if (data->cluster[i].opps_added) {
			dev_pm_opp_remove_all_dynamic(data->cluster[i].cpu_dev);
			data->cluster[i].opps_added = false;
		}
}

static int exynos8890_cpufreq_probe(struct platform_device *pdev)
{
	struct exynos8890_cpufreq *data;
	struct device_node *supplier_node;
	u32 apm_status;
	int ret;

	ret = exynos8890_calib_init();
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "calibration provider is not ready\n");
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
	if (apm_status & EXYNOS8890_APM_RUN) {
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
	data->cluster[EXYNOS8890_APOLLO].num_bus_locks =
		ARRAY_SIZE(apollo_bus_locks);
	data->cluster[EXYNOS8890_MONGOOSE].bus_locks = mongoose_bus_locks;
	data->cluster[EXYNOS8890_MONGOOSE].num_bus_locks =
		ARRAY_SIZE(mongoose_bus_locks);
	ret = exynos8890_init_cluster(pdev,
		&data->cluster[EXYNOS8890_APOLLO], "apollo",
		EXYNOS8890_CALIB_APOLLO);
	if (ret)
		goto out_remove_qos;
	ret = exynos8890_init_cluster(pdev,
		&data->cluster[EXYNOS8890_MONGOOSE], "mongoose",
		EXYNOS8890_CALIB_MONGOOSE);
	if (ret)
		goto out_remove_opps;
	ret = exynos8890_initialize_cluster_voltage(
		&data->cluster[EXYNOS8890_APOLLO]);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to establish safe Apollo voltage/EMA\n");
		goto out_remove_opps;
	}
	ret = exynos8890_initialize_cluster_voltage(
		&data->cluster[EXYNOS8890_MONGOOSE]);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			      "failed to establish safe Mongoose voltage/EMA\n");
		goto out_remove_opps;
	}
	ret = exynos8890_seed_mif_floor(data, EXYNOS8890_APOLLO);
	if (ret)
		goto out_remove_opps;
	ret = exynos8890_seed_mif_floor(data, EXYNOS8890_MONGOOSE);
	if (ret)
		goto out_remove_opps;

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
	if (ret)
		goto out_remove_opps;
	INIT_DELAYED_WORK(&data->cold_work, exynos8890_cold_work);
	WRITE_ONCE(data->cold_polling, true);
	schedule_delayed_work(&data->cold_work,
			      msecs_to_jiffies(EXYNOS8890_COLD_POLL_MS));
	return 0;

out_unlock:
	mutex_unlock(&exynos8890_driver_lock);
out_remove_opps:
	exynos8890_remove_opps(data);
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

	WRITE_ONCE(data->cold_polling, false);
	cancel_delayed_work_sync(&data->cold_work);
	mutex_lock(&exynos8890_driver_lock);
	cpufreq_unregister_driver(&exynos8890_cpufreq_driver);
	if (exynos8890_cpufreq_data == data)
		exynos8890_cpufreq_data = NULL;
	mutex_unlock(&exynos8890_driver_lock);
	exynos8890_remove_opps(data);
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
		.pm = pm_sleep_ptr(&exynos8890_cpufreq_pm_ops),
	},
};
module_platform_driver(exynos8890_cpufreq_platdrv);

MODULE_DESCRIPTION("Samsung Exynos8890 native dual-cluster CPUFreq driver");
MODULE_LICENSE("GPL");
