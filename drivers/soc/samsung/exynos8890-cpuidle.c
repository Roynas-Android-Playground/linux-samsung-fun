// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 C2 and cluster-power-down coordination.
 *
 * Port of the CPU-local part of Cronos_8890 drivers/soc/samsung/
 * exynos-powermode.c enter_c2()/wakeup_from_c2(), minus SICD/system-idle
 * promotion which stays unsupported here:
 *
 * This follows Cronos_8890's enter_c2()/wakeup_from_c2() contract: both
 * clusters expose per-core C2, only the non-boot cluster may enter CPD,
 * the local architectural timer remains the wake source, and whichever
 * CPU wakes first clears the cluster-global CPU sequencer state.
 *
 * Boot-cluster collapse is refused, same as vendor ("no benefit").
 *
 * The cpufreq<->cluster exclusion comes with this commit:
 * exynos8890_cpd_block()/exynos8890_cpd_unblock() mirror vendor's
 * block_cpd()/release_cpd() and must be called around frequency changes
 * once DVFS runs concurrently with idle.
 *
 * The DT node remains the board-level opt-in gate.
 */

#include <linux/atomic.h>
#include <linux/arch_topology.h>
#include <linux/bits.h>
#include <linux/clockchips.h>
#include <linux/context_tracking.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/tick.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>

#include <linux/soc/samsung/exynos8890-cpupm.h>
#include <linux/soc/samsung/exynos8890-cpuidle.h>

#define EXYNOS8890_PMU_CPU_CONFIG_BASE	0x2000
#define EXYNOS8890_PMU_CPU_STRIDE	0x80
#define EXYNOS8890_CPU_LOCAL_PWR_CFG	GENMASK(3, 0)
#define EXYNOS8890_C2_STATE		0x00010000
/* Cronos PSCI_CLUSTER_SLEEP: affinity level 1, state ID/type both zero. */
#define EXYNOS8890_CPD_STATE		0x01000000

/* Default CPD residency: below this much promised idle, stay in plain C2 */
#define EXYNOS8890_CPD_RESIDENCY_DEFAULT_US	3000
#define EXYNOS8890_CLUSTER_COUNT		2

struct exynos8890_c2_stats {
	u64 attempts;
	u64 before_psci;
	u64 after_psci;
	u64 psci_fail;
	u64 rollback_fail;
	u64 wake_pmu_bad;
	u32 last_before;
	u32 last_after;
	int last_psci_ret;
};

static DEFINE_PER_CPU(struct exynos8890_c2_stats, exynos8890_c2_stats);

struct exynos8890_cpuidle;

struct exynos8890_idle_driver {
	struct cpuidle_driver driver;
	struct cpumask cpus;
	struct exynos8890_cpuidle *idle;
};

struct exynos8890_cpuidle {
	void __iomem *pmu;
	struct exynos8890_idle_driver clusters[EXYNOS8890_CLUSTER_COUNT];
	atomic_t faulted;
	unsigned int cpd_residency_us;
};

/*
 * Cluster coordination state. c2_lock keeps c2_mask, cluster_idle_state
 * and cpd_blocked mutually consistent across concurrent idle entries,
 * exactly like vendor's c2_lock.
 */
static DEFINE_SPINLOCK(c2_lock);
static struct cpumask c2_mask;
static int cluster_idle_state[EXYNOS8890_CLUSTER_COUNT];
static bool cpd_blocked;
static unsigned int boot_cluster;

static unsigned int cluster_id_of(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
}

static s64 next_event_time_us(unsigned int cpu)
{
	ktime_t next = READ_ONCE(per_cpu(cpuidle_dev, cpu).next_hrtimer);

	if (!next)
		return 0;

	return ktime_to_us(ktime_sub(next, ktime_get()));
}

/*
 * Decide whether the whole non-boot cluster may collapse behind this
 * CPU's C2 entry. Called with c2_lock held, after this CPU was marked in
 * c2_mask. Vendor refuses CPD for the boot cluster outright.
 */
static bool cpd_available_locked(struct exynos8890_cpuidle *idle,
				 unsigned int cpu)
{
	const struct cpumask *cluster_mask;
	unsigned int member;
	s64 residency = idle->cpd_residency_us;

	if (cpd_blocked)
		return false;

	if (cluster_id_of(cpu) == boot_cluster)
		return false;

	cluster_mask = cpu_coregroup_mask(cpu);

	for_each_cpu_and(member, cluster_mask, cpu_online_mask) {
		if (!cpumask_test_cpu(member, &c2_mask))
			return false;
		if (next_event_time_us(member) < residency)
			return false;
	}

	return true;
}

static unsigned int exynos8890_cpu_config_reg(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	u32 index;

	index = (MPIDR_AFFINITY_LEVEL(mpidr, 1) << 2) |
		MPIDR_AFFINITY_LEVEL(mpidr, 0);
	return EXYNOS8890_PMU_CPU_CONFIG_BASE +
		index * EXYNOS8890_PMU_CPU_STRIDE;
}

static int __cpuidle exynos8890_enter_wfi(struct cpuidle_device *dev,
					  struct cpuidle_driver *drv, int index)
{
	cpu_do_idle();
	return index;
}

static int __cpuidle exynos8890_enter_wfi_rcu(void)
{
	ct_cpuidle_enter();
	cpu_do_idle();
	ct_cpuidle_exit();
	return 0;
}

static int noinstr exynos8890_cpd_finisher(unsigned long state)
{
	return psci_ops.cpu_suspend(state, __pa_symbol(cpu_resume));
}

static int __cpuidle exynos8890_enter_c2(struct cpuidle_device *dev,
					 struct cpuidle_driver *drv, int index)
{
	struct exynos8890_idle_driver *cluster_drv = container_of(drv,
						struct exynos8890_idle_driver,
						driver);
	struct exynos8890_cpuidle *idle = cluster_drv->idle;
	struct exynos8890_c2_stats *stats = this_cpu_ptr(&exynos8890_c2_stats);
	unsigned int cpu = smp_processor_id();
	unsigned int cluster = cluster_id_of(cpu);
	bool cpd_taken = false;
	void __iomem *config;
	u32 before, after;
	unsigned long flags;
	int ret;

	if (unlikely(atomic_read(&idle->faulted) || cpu != dev->cpu))
		return exynos8890_enter_wfi_rcu();

	stats->attempts++;
	ret = cpu_pm_enter();
	if (ret)
		return -1;

	config = idle->pmu + exynos8890_cpu_config_reg(cpu);
	before = readl_relaxed(config);
	stats->last_before = before;
	if (unlikely((before & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		     EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		atomic_set(&idle->faulted, 1);
		ret = -EIO;
		goto out_pm;
	}

	writel_relaxed(before & ~EXYNOS8890_CPU_LOCAL_PWR_CFG, config);
	after = readl_relaxed(config);
	if (unlikely(after & EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		writel_relaxed(before, config);
		dsb(sy);
		atomic_set(&idle->faulted, 1);
		ret = -EIO;
		goto out_pm;
	}

	/*
	 * Vendor enter_c2(): mark us down, then maybe collapse the cluster
	 * behind this entry. The spinlock section must not touch anything
	 * that sleeps.
	 */
	spin_lock_irqsave(&c2_lock, flags);
	cpumask_set_cpu(cpu, &c2_mask);
	if (next_event_time_us(cpu) >= idle->cpd_residency_us &&
	    cpd_available_locked(idle, cpu)) {
		exynos8890_cluster_down(cluster);
		cluster_idle_state[cluster] = 1;
		cpd_taken = true;
	}
	spin_unlock_irqrestore(&c2_lock, flags);

	stats->before_psci++;
	dsb(sy);
	if (cpd_taken)
		ret = cpu_suspend(EXYNOS8890_CPD_STATE,
				  exynos8890_cpd_finisher);
	else
		ret = psci_cpu_suspend_enter(EXYNOS8890_C2_STATE);
	stats->last_psci_ret = ret;
	stats->after_psci++;

	/* Vendor restores local power immediately on an aborted suspend. */
	if (ret)
		exynos8890_cpu_power_up(cpu);

	/* Whichever core wakes first owns the cluster-global CPD unwind. */
	spin_lock_irqsave(&c2_lock, flags);
	if (cluster_idle_state[cluster]) {
		exynos8890_cluster_up(cluster);
		cluster_idle_state[cluster] = 0;
	}
	cpumask_clear_cpu(cpu, &c2_mask);
	spin_unlock_irqrestore(&c2_lock, flags);

	after = readl_relaxed(config);
	stats->last_after = after;

	if (ret) {
		stats->psci_fail++;
		if ((readl_relaxed(config) & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		    EXYNOS8890_CPU_LOCAL_PWR_CFG) {
			stats->rollback_fail++;
			exynos8890_cpu_power_up(cpu);
			atomic_set(&idle->faulted, 1);
		}
	} else if (unlikely((after & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
			    EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		/* Firmware lost our power-on; put it back like vendor would */
		stats->wake_pmu_bad++;
		exynos8890_cpu_power_up(cpu);
	}

out_pm:
	cpu_pm_exit();
	return ret ? -1 : index;
}

void exynos8890_cpd_block(void)
{
	unsigned long flags;

	spin_lock_irqsave(&c2_lock, flags);
	cpd_blocked = true;
	spin_unlock_irqrestore(&c2_lock, flags);
}
EXPORT_SYMBOL_GPL(exynos8890_cpd_block);

void exynos8890_cpd_unblock(void)
{
	unsigned long flags;

	spin_lock_irqsave(&c2_lock, flags);
	cpd_blocked = false;
	spin_unlock_irqrestore(&c2_lock, flags);
}
EXPORT_SYMBOL_GPL(exynos8890_cpd_unblock);

bool exynos8890_cpd_is_active(unsigned int cpu)
{
	unsigned int cluster = cluster_id_of(cpu);
	unsigned long flags;
	bool active;

	if (cluster >= EXYNOS8890_CLUSTER_COUNT)
		return false;

	spin_lock_irqsave(&c2_lock, flags);
	active = cluster_idle_state[cluster];
	spin_unlock_irqrestore(&c2_lock, flags);

	return active;
}
EXPORT_SYMBOL_GPL(exynos8890_cpd_is_active);

static void exynos8890_init_idle_driver(struct exynos8890_idle_driver *cluster,
					bool is_boot_cluster)
{
	struct cpuidle_driver *drv = &cluster->driver;

	drv->name = is_boot_cluster ? "exynos8890-c2-boot" :
					    "exynos8890-c2-nonboot";
	drv->owner = THIS_MODULE;
	drv->cpumask = &cluster->cpus;
	drv->safe_state_index = 0;
	drv->state_count = 2;
	drv->states[0] = (struct cpuidle_state) {
		.name = "WFI",
		.desc = "ARM WFI",
		.exit_latency_ns = 1000,
		.target_residency_ns = 1000,
		.enter = exynos8890_enter_wfi,
	};
	drv->states[1] = (struct cpuidle_state) {
		.name = "C2",
		/* Cronos parser used entry + exit latency for this field. */
		.exit_latency_ns = is_boot_cluster ? 125000 : 105000,
		.target_residency_ns = is_boot_cluster ? 750000 : 2000000,
		/* Cronos DT deliberately has no local-timer-stop property. */
		.flags = CPUIDLE_FLAG_RCU_IDLE,
		.enter = exynos8890_enter_c2,
	};
	strscpy(drv->states[1].desc,
		is_boot_cluster ? "Exynos8890 boot-cluster C2" :
				  "Exynos8890 non-boot C2/CPD");
}

static int exynos8890_cpuidle_probe(struct platform_device *pdev)
{
	struct device_node *pmu_np;
	struct exynos8890_cpuidle *idle;
	u32 state = 0;
	int cluster, cpu, ret;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;
	if (!exynos8890_cpupm_ready())
		return -EPROBE_DEFER;

	ret = of_property_read_u32(pdev->dev.of_node,
				   "samsung,psci-suspend-param", &state);
	if (ret || state != EXYNOS8890_C2_STATE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid PSCI C2 state %#x\n", state);

	idle = devm_kzalloc(&pdev->dev, sizeof(*idle), GFP_KERNEL);
	if (!idle)
		return -ENOMEM;
	atomic_set(&idle->faulted, 0);
	idle->cpd_residency_us = EXYNOS8890_CPD_RESIDENCY_DEFAULT_US;
	of_property_read_u32(pdev->dev.of_node, "samsung,cpd-residency-us",
			     &idle->cpd_residency_us);
	boot_cluster = cluster_id_of(0);

	pmu_np = of_parse_phandle(pdev->dev.of_node, "samsung,pmu-syscon", 0);
	if (!pmu_np)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing PMU phandle\n");
	idle->pmu = of_iomap(pmu_np, 0);
	of_node_put(pmu_np);
	if (!idle->pmu)
		return dev_err_probe(&pdev->dev, -ENOMEM, "failed to map PMU\n");

	for_each_possible_cpu(cpu) {
		u32 value = readl_relaxed(idle->pmu +
					  exynos8890_cpu_config_reg(cpu));

		if ((value & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		    EXYNOS8890_CPU_LOCAL_PWR_CFG) {
			dev_err(&pdev->dev,
				"CPU%d starts with invalid PMU config %#x\n",
				cpu, value);
			ret = -EIO;
			goto err_unmap;
		}
	}

	for (cluster = 0; cluster < EXYNOS8890_CLUSTER_COUNT; cluster++) {
		idle->clusters[cluster].idle = idle;
		cpumask_clear(&idle->clusters[cluster].cpus);
	}

	for_each_possible_cpu(cpu) {
		cluster = cluster_id_of(cpu);
		if (cluster >= EXYNOS8890_CLUSTER_COUNT) {
			ret = -EINVAL;
			goto err_unmap;
		}
		cpumask_set_cpu(cpu, &idle->clusters[cluster].cpus);
	}

	platform_set_drvdata(pdev, idle);
	for (cluster = 0; cluster < EXYNOS8890_CLUSTER_COUNT; cluster++) {
		struct exynos8890_idle_driver *cluster_drv =
			&idle->clusters[cluster];

		if (cpumask_empty(&cluster_drv->cpus))
			continue;
		exynos8890_init_idle_driver(cluster_drv,
					    cluster == boot_cluster);
		ret = cpuidle_register(&cluster_drv->driver, NULL);
		if (ret)
			goto err_unregister;
	}

	dev_info(&pdev->dev, "vendor C2 registered on all CPUs; CPD residency %uus\n",
		 idle->cpd_residency_us);
	return 0;

err_unregister:
	while (--cluster >= 0) {
		if (!cpumask_empty(&idle->clusters[cluster].cpus))
			cpuidle_unregister(&idle->clusters[cluster].driver);
	}
err_unmap:
	iounmap(idle->pmu);
	return ret;
}

static void exynos8890_cpuidle_remove(struct platform_device *pdev)
{
	struct exynos8890_cpuidle *idle = platform_get_drvdata(pdev);

	if (!idle)
		return;
	for (int cluster = 0; cluster < EXYNOS8890_CLUSTER_COUNT; cluster++) {
		if (!cpumask_empty(&idle->clusters[cluster].cpus))
			cpuidle_unregister(&idle->clusters[cluster].driver);
	}
	iounmap(idle->pmu);
}

static const struct of_device_id exynos8890_cpuidle_of_match[] = {
	{ .compatible = "samsung,exynos8890-cpuidle-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpuidle_of_match);

static struct platform_driver exynos8890_cpuidle_driver = {
	.probe = exynos8890_cpuidle_probe,
	.remove = exynos8890_cpuidle_remove,
	.driver = {
		.name = "exynos8890-cpuidle-test",
		.of_match_table = exynos8890_cpuidle_of_match,
	},
};
module_platform_driver(exynos8890_cpuidle_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 local-core C2 with cluster-down coordination");
MODULE_LICENSE("GPL");
