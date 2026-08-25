// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 CPU power-management lifecycle glue.
 *
 * Samsung's vendor kernel disables the PMU CPU sequencer for the non-boot
 * cluster on every CPU_STARTING event. The sequencer is enabled only when
 * the vendor power-mode code deliberately enters cluster power-down (CPD).
 *
 * Keep that lifecycle invariant here without enabling CPD, system idle or
 * any other deep-idle policy. This is not a generic "safety" override: it
 * reproduces the vendor CPU-starting transition at the same point in CPU
 * bring-up, before the GIC CPU interface is initialized.
 */

#include <linux/bitops.h>
#include <linux/cpuhotplug.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/regmap.h>

#include <asm/cputype.h>
#include <asm/smp_plat.h>

#define EXYNOS8890_PMU_CPUSEQ_OPTION_BASE	0x2488
#define EXYNOS8890_PMU_CLUSTER_STRIDE		0x20
#define EXYNOS8890_CPUSEQ_ENABLE		BIT(0)

static struct regmap *exynos8890_pmureg;

static unsigned int exynos8890_cluster_id(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);

	return MPIDR_AFFINITY_LEVEL(mpidr, 1);
}

static int exynos8890_cpu_starting(unsigned int cpu)
{
	unsigned int cluster = exynos8890_cluster_id(cpu);
	unsigned int boot_cluster = exynos8890_cluster_id(0);
	unsigned int reg;
	u32 before, after;
	int ret;

	/* Vendor only normalizes the non-boot cluster on CPU_STARTING. */
	if (cluster == boot_cluster)
		return 0;

	reg = EXYNOS8890_PMU_CPUSEQ_OPTION_BASE +
		cluster * EXYNOS8890_PMU_CLUSTER_STRIDE;

	ret = regmap_read(exynos8890_pmureg, reg, &before);
	if (ret) {
		pr_err("exynos8890-pmu: CPU%u failed to read cluster%u CPUSEQ_OPTION: %d\n",
		       cpu, cluster, ret);
		return 0;
	}

	ret = regmap_update_bits(exynos8890_pmureg, reg,
				 EXYNOS8890_CPUSEQ_ENABLE, 0);
	if (ret) {
		pr_err("exynos8890-pmu: CPU%u failed to disable cluster%u CPU sequencer: %d\n",
		       cpu, cluster, ret);
		return 0;
	}

	ret = regmap_read(exynos8890_pmureg, reg, &after);
	if (ret) {
		pr_err("exynos8890-pmu: CPU%u failed to verify cluster%u CPUSEQ_OPTION: %d\n",
		       cpu, cluster, ret);
		return 0;
	}

	if (after & EXYNOS8890_CPUSEQ_ENABLE) {
		pr_err("exynos8890-pmu: CPU%u cluster%u CPU sequencer remained enabled (before=%#x after=%#x)\n",
		       cpu, cluster, before, after);
	} else if (before & EXYNOS8890_CPUSEQ_ENABLE) {
		pr_info("exynos8890-pmu: CPU%u cluster%u CPU sequencer disabled (CPUSEQ_OPTION %#x -> %#x)\n",
			cpu, cluster, before, after);
	} else {
		pr_debug("exynos8890-pmu: CPU%u cluster%u CPU sequencer already disabled (CPUSEQ_OPTION %#x)\n",
			 cpu, cluster, after);
	}

	/* Samsung's CPU notifier never vetoed CPU_STARTING on PMU errors. */
	return 0;
}

static int __init exynos8890_cpu_pm_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos8890-pmu");
	if (!np)
		return 0;

	exynos8890_pmureg = syscon_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(exynos8890_pmureg)) {
		ret = PTR_ERR(exynos8890_pmureg);
		pr_err("exynos8890-pmu: failed to get PMU regmap: %d\n", ret);
		return ret;
	}

	/*
	 * The vendor CPU_STARTING notifier has priority INT_MAX while its GICv3
	 * notifier has priority 100. The dedicated CPUHP state is therefore
	 * ordered immediately before CPUHP_AP_IRQ_GIC_STARTING.
	 *
	 * Use nocalls because register_hotcpu_notifier() did not synthesize a
	 * CPU_STARTING event for CPUs that were already online at registration.
	 */
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_EXYNOS8890_PMU_STARTING,
					"soc/exynos8890-pmu:starting",
					exynos8890_cpu_starting, NULL);
	if (ret)
		pr_err("exynos8890-pmu: failed to register CPU-starting hook: %d\n",
		       ret);

	return ret;
}
arch_initcall(exynos8890_cpu_pm_init);
