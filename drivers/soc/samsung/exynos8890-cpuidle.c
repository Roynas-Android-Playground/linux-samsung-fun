// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 local-core C2 with vendor cluster-down coordination.
 *
 * Port of the CPU-local part of Cronos_8890 drivers/soc/samsung/
 * exynos-powermode.c enter_c2()/wakeup_from_c2(), minus SICD/system-idle
 * promotion which stays unsupported here:
 *
 *   enter:
 *     - clear this CPU's PMU LOCAL_PWR_CFG
 *     - mark this CPU in c2_mask
 *     - if this is the last online core of the NON-BOOT cluster and every
 *       core in the cluster has enough timer residency and CPD is not
 *       blocked: cluster_down() (CPUSEQ bit0=1) and remember it
 *     - PSCI CPU_SUSPEND into C2
 *   wake:
 *     - if we collapsed the cluster: cluster_up() (bit0=0)
 *     - restore LOCAL_PWR_CFG if firmware left it cleared
 *     - clear c2_mask
 *
 * Boot-cluster collapse is refused, same as vendor ("no benefit").
 *
 * The cpufreq<->cluster exclusion comes with this commit:
 * exynos8890_cpd_block()/exynos8890_cpd_unblock() mirror vendor's
 * block_cpd()/release_cpd() and must be called around frequency changes
 * once DVFS runs concurrently with idle.
 *
 * Opt-in is CONFIG_EXYNOS8890_CPUIDLE itself; no boot flag gates it.
 */

#include <linux/atomic.h>
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
#include <linux/hrtimer.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/smp_plat.h>

#include <linux/soc/samsung/exynos8890-cpupm.h>
#include <linux/soc/samsung/exynos8890-cpuidle.h>

#define EXYNOS8890_PMU_CPU_CONFIG_BASE	0x2000
#define EXYNOS8890_PMU_CPU_STRIDE	0x80
#define EXYNOS8890_CPU_LOCAL_PWR_CFG	GENMASK(3, 0)
#define EXYNOS8890_C2_STATE		0x00010000

/* Default CPD residency: below this much promised idle, stay in plain C2 */
#define EXYNOS8890_CPD_RESIDENCY_DEFAULT_US	50000

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
static unsigned int boot_cluster;

struct exynos8890_cpuidle {
	void __iomem *pmu;
	struct cpuidle_driver driver;
	struct cpumask cpus;
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
static int cluster_idle_state[2];
static bool cpd_blocked;
static unsigned int boot_cluster;

static unsigned int cluster_id_of(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
}

static s64 next_event_time_us(unsigned int cpu)
{
	ktime_t delta_next;
	ktime_t len;

	/* Must be called for the local CPU: reads this CPU's tick device */
	len = tick_nohz_get_sleep_length(&delta_next);

	return ktime_to_us(len);
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

	cluster_mask = topology_sibling_cpumask(cpu);

	for_each_cpu_and(member, cluster_mask, cpu_online_mask) {
		if (!cpumask_test_cpu(member, &c2_mask))
			return false;
		if (member != cpu && next_event_time_us(member) < residency)
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

static int __cpuidle exynos8890_enter_c2(struct cpuidle_device *dev,
					 struct cpuidle_driver *drv, int index)
{
	struct exynos8890_cpuidle *idle = container_of(drv,
						       struct exynos8890_cpuidle,
						       driver);
	struct exynos8890_c2_stats *stats = this_cpu_ptr(&exynos8890_c2_stats);
	unsigned int cpu = smp_processor_id();
	unsigned int cluster = cluster_id_of(cpu);
	bool cluster_was_down = false;
	bool cpd_taken = false;
	bool early_wakeup;
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
	cpd_taken = cpd_available_locked(idle, cpu);
	if (cpd_taken) {
		exynos8890_cluster_down(cluster);
		cluster_idle_state[cluster] = 1;
		cluster_was_down = true;
	}
	spin_unlock_irqrestore(&c2_lock, flags);

	stats->before_psci++;
	dsb(sy);
	ret = psci_cpu_suspend_enter(EXYNOS8890_C2_STATE);
	stats->last_psci_ret = ret;
	stats->after_psci++;

	early_wakeup = ret != 0;

	/* Vendor wakeup_from_c2(): undo the cluster collapse first. */
	if (cluster_was_down) {
		exynos8890_cluster_up(cluster);
		cluster_idle_state[cluster] = 0;
	}

	after = readl_relaxed(config);
	stats->last_after = after;

	if (ret) {
		stats->psci_fail++;
		writel_relaxed(before, config);
		dsb(sy);
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

	spin_lock_irqsave(&c2_lock, flags);
	cpumask_clear_cpu(cpu, &c2_mask);
	spin_unlock_irqrestore(&c2_lock, flags);

out_pm:
	cpu_pm_exit();
	return ret ? -1 : index;
}

static int exynos8890_cpuidle_probe(struct platform_device *pdev)
{
	struct device_node *pmu_np;
	struct exynos8890_cpuidle *idle;
	u32 state = 0;
	int cpu, ret;

	/*
	 * Hardware finding (herolte, 2026-08-25): every C2 variant tried -
	 * boot-cluster included, CPD on or off - wedges a random core in
	 * idle within ~20s of boot. The core stops responding to pseudo-NMI
	 * while LOCAL_PWR_CFG reads powered-on again, i.e. the core went
	 * down through CPU_SUSPEND and the wake never reached it.
	 *
	 * Vendor Cronos runs this exact PSCI call too, but its EL3 monitor
	 * pairs it with CAL PM-table sequences (exynos_prepare_cp_call /
	 * wakeup_cp_call via the exynos_pm notifier chain) that this port
	 * does not have. Until those are ported, C2 stays disabled at the
	 * DT level: the driver only probes when the node is enabled.
	 */
	if (!of_property_read_bool(pdev->dev.of_node, "status") ||
	    !of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

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

	idle->driver.name = "exynos8890-c2";
	idle->driver.owner = THIS_MODULE;
	idle->driver.safe_state_index = 0;
	idle->driver.state_count = 2;
	idle->driver.states[0] = (struct cpuidle_state) {
		.name = "WFI",
		.desc = "ARM WFI",
		.exit_latency_ns = 1000,
		.target_residency_ns = 1000,
		.enter = exynos8890_enter_wfi,
	};
	idle->driver.states[1] = (struct cpuidle_state) {
		.name = "C2",
		.desc = "Exynos8890 C2 + cluster down",
		.exit_latency_ns = 90000,
		.target_residency_ns = 2000000,
		.flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE |
			 CPUIDLE_FLAG_OFF,
		.enter = exynos8890_enter_c2,
	};

	/*
	 * The boot cluster must never enter C2. Vendor Cronos only ever
	 * registers its C2-capable idle state for non-boot-cluster CPUs;
	 * on this hardware a boot-cluster (cpu0/Mongoose) PSCI CPU_SUSPEND
	 * into 0x00010000 never wakes - RCU stalls on cpu0 with the core
	 * unresponsive to pseudo-NMI, while every other core idles fine.
	 * Restrict the whole driver to the non-boot cluster; boot-cluster
	 * cores fall back to the default arch WFI loop.
	 */
	cpumask_clear(&idle->cpus);
	for_each_possible_cpu(cpu) {
		if (cluster_id_of(cpu) != boot_cluster)
			cpumask_set_cpu(cpu, &idle->cpus);
	}

	platform_set_drvdata(pdev, idle);
	ret = cpuidle_register(&idle->driver, &idle->cpus);
	if (ret)
		goto err_unmap;

	dev_info(&pdev->dev,
		 "C2 registered for non-boot cluster %*pbl (CPD residency %uus)\n",
		 cpumask_pr_args(&idle->cpus), idle->cpd_residency_us);
	return 0;

err_unmap:
	iounmap(idle->pmu);
	return ret;
}

static void exynos8890_cpuidle_remove(struct platform_device *pdev)
{
	struct exynos8890_cpuidle *idle = platform_get_drvdata(pdev);

	if (!idle)
		return;
	cpuidle_unregister(&idle->driver);
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
