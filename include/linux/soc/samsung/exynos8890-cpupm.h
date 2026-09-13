/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos8890 CPU/cluster PMU power-control interface.
 *
 * CPU_CONFIG controls local power; CPU_STATUS, NONCPU_STATUS and L2_STATUS
 * report CPU and cluster state. CPUSEQ_OPTION arms cluster power-down:
 *
 *   cluster_up()   == CPU sequencer DISABLED (CPUSEQ_OPTION bit0 = 0)
 *   cluster_down() == CPU sequencer ENABLED  (bit0 = 1)
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_EXYNOS8890_CPUPM

bool exynos8890_cpupm_ready(void);
int exynos8890_cpu_power_up(unsigned int cpu);
int exynos8890_cpu_power_down(unsigned int cpu);
int exynos8890_cpu_power_config_read(unsigned int cpu, u32 *value);
/* Return 1 if powered, 0 if off, or a negative register-access error. */
int exynos8890_cpu_power_state(unsigned int cpu);
int exynos8890_cluster_up(unsigned int cluster);
int exynos8890_cluster_down(unsigned int cluster);
/* Return 1 if both NONCPU and L2 are powered, 0 if not, or a negative error. */
int exynos8890_cluster_power_state(unsigned int cluster);

#else /* !CONFIG_EXYNOS8890_CPUPM */

static inline bool exynos8890_cpupm_ready(void) { return false; }
static inline int exynos8890_cpu_power_up(unsigned int cpu) { return -ENODEV; }
static inline int exynos8890_cpu_power_down(unsigned int cpu) { return -ENODEV; }
static inline int exynos8890_cpu_power_config_read(unsigned int cpu, u32 *value)
{
	return -ENODEV;
}
static inline int exynos8890_cpu_power_state(unsigned int cpu) { return -ENODEV; }
static inline int exynos8890_cluster_up(unsigned int cluster) { return -ENODEV; }
static inline int exynos8890_cluster_down(unsigned int cluster) { return -ENODEV; }
static inline int exynos8890_cluster_power_state(unsigned int cluster)
{
	return -ENODEV;
}

#endif /* CONFIG_EXYNOS8890_CPUPM */

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUPM_H */
