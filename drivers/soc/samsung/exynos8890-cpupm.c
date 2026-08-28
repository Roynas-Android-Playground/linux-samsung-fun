// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 CPU/cluster PMU power control.
 *
 * Port of vendor Cronos_8890 drivers/soc/samsung/exynos-pmu.c CPU power
 * ops onto the modern regmap-based exynos-pmu driver:
 *
 *   cpu up/down    : PMU_CPU_CONFIG LOCAL_PWR_CFG (0xf) set/clear
 *   cpu state      : PMU_CPU_STATUS LOCAL_PWR_CFG
 *   cluster up     : PMU_CPUSEQ_OPTION bit0 = 0 (sequencer DISABLED)
 *   cluster down   : PMU_CPUSEQ_OPTION bit0 = 1 (sequencer ENABLED)
 *   cluster state  : NONCPU_STATUS(0xf) && L2_STATUS(0x7)
 *
 * Register semantics (offsets from vendor source, verified against the
 * earlier read-only boot dump):
 *   CPU regs: base 0x2000, stride 0x80,
 *             index = (aff1 << 2) | aff0
 *   Cluster regs: NONCPU_STATUS 0x2404, CPUSEQ_OPTION 0x2488,
 *             L2_STATUS 0x2604, stride 0x20
 *
 * The vendor kernel also enforces one lifecycle invariant on every
 * CPU_STARTING event: the non-boot cluster's sequencer must be disabled
 * (cluster_up). S-Boot already leaves it that way on this device (boot
 * dump: non-boot CPUSEQ_OPTION=0x1000, bit0 clear), but hotplug/C2 paths
 * flip it, so restore it here exactly like vendor's INT_MAX-priority
 * notifier did. Boot-cluster CPD is not supported by Samsung and never
 * gets touched.
 */

#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/smp_plat.h>

#include <linux/soc/samsung/exynos8890-cpupm.h>

#define PMU_CP_STAT			0x0038

#define PMU_CPU_CONFIG_BASE		0x2000
#define PMU_CPU_STATUS_BASE		0x2004
#define PMU_CPU_ADDR_OFFSET		0x80
#define CPU_LOCAL_PWR_CFG		0xF

#define PMU_NONCPU_STATUS_BASE		0x2404
#define PMU_CPUSEQ_OPTION_BASE		0x2488
#define PMU_L2_STATUS_BASE		0x2604
#define PMU_CLUSTER_ADDR_OFFSET		0x20
#define NONCPU_LOCAL_PWR_CFG		0xF
#define L2_LOCAL_PWR_CFG		0x7

static struct regmap *pmureg;

bool exynos8890_cpupm_ready(void)
{
	return pmureg != NULL;
}
EXPORT_SYMBOL_GPL(exynos8890_cpupm_ready);
static unsigned int boot_cluster;
static DEFINE_SPINLOCK(cpupm_lock);

static unsigned int pmu_cpu_offset(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);

	return ((MPIDR_AFFINITY_LEVEL(mpidr, 1) << 2 |
		 MPIDR_AFFINITY_LEVEL(mpidr, 0)) * PMU_CPU_ADDR_OFFSET);
}

void exynos8890_cpu_power_up(unsigned int cpu)
{
	regmap_update_bits(pmureg, PMU_CPU_CONFIG_BASE + pmu_cpu_offset(cpu),
			   CPU_LOCAL_PWR_CFG, CPU_LOCAL_PWR_CFG);
}
EXPORT_SYMBOL_GPL(exynos8890_cpu_power_up);

void exynos8890_cpu_power_down(unsigned int cpu)
{
	regmap_update_bits(pmureg, PMU_CPU_CONFIG_BASE + pmu_cpu_offset(cpu),
			   CPU_LOCAL_PWR_CFG, 0);
}
EXPORT_SYMBOL_GPL(exynos8890_cpu_power_down);

int exynos8890_cpu_power_state(unsigned int cpu)
{
	unsigned int val;

	if (!pmureg)
		return -ENODEV;

	regmap_read(pmureg, PMU_CPU_STATUS_BASE + pmu_cpu_offset(cpu), &val);

	return (val & CPU_LOCAL_PWR_CFG) == CPU_LOCAL_PWR_CFG;
}
EXPORT_SYMBOL_GPL(exynos8890_cpu_power_state);

/*
 * Vendor: "While Exynos with multi cluster supports to shutdown down both
 * cluster, there is no benefit in boot cluster." Callers must gate on
 * is_boot_cluster(); this function does not.
 */
void exynos8890_cluster_up(unsigned int cluster)
{
	spin_lock(&cpupm_lock);
	regmap_update_bits(pmureg, PMU_CPUSEQ_OPTION_BASE +
				   cluster * PMU_CLUSTER_ADDR_OFFSET, 1, 0);
	spin_unlock(&cpupm_lock);
}
EXPORT_SYMBOL_GPL(exynos8890_cluster_up);

void exynos8890_cluster_down(unsigned int cluster)
{
	spin_lock(&cpupm_lock);
	regmap_update_bits(pmureg, PMU_CPUSEQ_OPTION_BASE +
				   cluster * PMU_CLUSTER_ADDR_OFFSET, 1, 1);
	spin_unlock(&cpupm_lock);
}
EXPORT_SYMBOL_GPL(exynos8890_cluster_down);

int exynos8890_cluster_power_state(unsigned int cluster)
{
	unsigned int offset = cluster * PMU_CLUSTER_ADDR_OFFSET;
	unsigned int noncpu_stat, l2_stat;

	if (!pmureg)
		return -ENODEV;

	regmap_read(pmureg, PMU_NONCPU_STATUS_BASE + offset, &noncpu_stat);
	regmap_read(pmureg, PMU_L2_STATUS_BASE + offset, &l2_stat);

	return ((l2_stat & L2_LOCAL_PWR_CFG) == L2_LOCAL_PWR_CFG) &&
	       ((noncpu_stat & NONCPU_LOCAL_PWR_CFG) == NONCPU_LOCAL_PWR_CFG);
}
EXPORT_SYMBOL_GPL(exynos8890_cluster_power_state);

static bool is_boot_cluster(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1) == boot_cluster;
}

/*
 * Vendor lifecycle invariant: whenever a CPU starts in the non-boot
 * cluster, its sequencer must be back in the cluster-up (disabled) state.
 */
static int exynos8890_cpupm_starting(unsigned int cpu)
{
	if (!is_boot_cluster(cpu))
		exynos8890_cluster_up(MPIDR_AFFINITY_LEVEL(
						cpu_logical_map(cpu), 1));

	return 0;
}

static int __init exynos8890_cpupm_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	unsigned int cluster, val;
	int cpu;

	/*
	 * We are a child node of the PMU system-controller itself
	 * (populated by exynos-pmu via devm_of_platform_populate()), so
	 * get the regmap the parent driver registered for its own node.
	 * This mirrors how other children of a syscon node do it
	 * (e.g. npcm7xx KCS, Aspeed LPC/p2a-ctrl, mvebu GPIO).
	 */
	if (pdev->dev.parent && pdev->dev.parent->of_node)
		pmureg = syscon_node_to_regmap(pdev->dev.parent->of_node);
	else
		pmureg = ERR_PTR(-ENODEV);

	if (IS_ERR(pmureg)) {
		/* DT-only fallback: explicit phandle to the PMU syscon */
		pmureg = syscon_regmap_lookup_by_phandle(np,
							 "samsung,syscon-phandle");
		if (IS_ERR(pmureg))
			return dev_err_probe(&pdev->dev, PTR_ERR(pmureg),
					     "failed to get PMU regmap\n");
	}

	boot_cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(0), 1);

	for (cluster = 0; cluster < 2; cluster++) {
		regmap_read(pmureg, PMU_CPUSEQ_OPTION_BASE +
				    cluster * PMU_CLUSTER_ADDR_OFFSET, &val);
		pr_info("exynos8890-cpupm: cluster%u (%s) CPUSEQ_OPTION=%#x\n",
			cluster, cluster == boot_cluster ? "boot" : "non-boot",
			val);
	}

	for_each_possible_cpu(cpu) {
		regmap_read(pmureg, PMU_CPU_CONFIG_BASE + pmu_cpu_offset(cpu),
			    &val);
		pr_info("exynos8890-cpupm: cpu%u CPU_CONFIG=%#x "
			"local_pwr_cfg=%#x\n",
			cpu, val, val & CPU_LOCAL_PWR_CFG);
	}

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "soc/exynos8890/cpupm:starting",
			  exynos8890_cpupm_starting, NULL);

	return 0;
}

static const struct of_device_id exynos8890_cpupm_of_match[] = {
	{ .compatible = "samsung,exynos8890-cpupm" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpupm_of_match);

static struct platform_driver exynos8890_cpupm_driver = {
	.probe = exynos8890_cpupm_probe,
	.driver = {
		.name = "exynos8890-cpupm",
		.of_match_table = exynos8890_cpupm_of_match,
	},
};
builtin_platform_driver(exynos8890_cpupm_driver);
