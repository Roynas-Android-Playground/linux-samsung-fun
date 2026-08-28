/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos8890 CPU/cluster PMU power-control interface.
 *
 * Mirrors vendor Cronos_8890 drivers/soc/samsung/exynos-pmu.c:
 * exynos_cpu.power_up/power_down/power_state/cluster_up/cluster_down/
 * cluster_state over the PMU CPU_CONFIG / NONCPU_STATUS / L2_STATUS /
 * CPUSEQ_OPTION registers. Semantics preserved verbatim - in particular
 *
 *   cluster_up()   == CPU sequencer DISABLED (CPUSEQ_OPTION bit0 = 0)
 *   cluster_down() == CPU sequencer ENABLED  (bit0 = 1)
 *
 * which reads backwards but is what Samsung shipped.
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H

#include <linux/types.h>

#ifdef CONFIG_EXYNOS8890_CPUPM

bool exynos8890_cpupm_ready(void);
void exynos8890_cpu_power_up(unsigned int cpu);
void exynos8890_cpu_power_down(unsigned int cpu);
/* returns true when the CPU's LOCAL_PWR_CFG shows the domain powered */
int exynos8890_cpu_power_state(unsigned int cpu);
void exynos8890_cluster_up(unsigned int cluster);
void exynos8890_cluster_down(unsigned int cluster);
/* true when both NONCPU and L2 status show the cluster fully powered */
int exynos8890_cluster_power_state(unsigned int cluster);

#else /* !CONFIG_EXYNOS8890_CPUPM */

static inline bool exynos8890_cpupm_ready(void) { return false; }
static inline void exynos8890_cpu_power_up(unsigned int cpu) {}
static inline void exynos8890_cpu_power_down(unsigned int cpu) {}
static inline int exynos8890_cpu_power_state(unsigned int cpu) { return -ENODEV; }
static inline void exynos8890_cluster_up(unsigned int cluster) {}
static inline void exynos8890_cluster_down(unsigned int cluster) {}
static inline int exynos8890_cluster_power_state(unsigned int cluster)
{
	return -ENODEV;
}

#endif /* CONFIG_EXYNOS8890_CPUPM */

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H */
