/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos8890 cpufreq <-> cluster-power-down exclusion.
 *
 * Vendor Cronos_8890 exynos-powermode.c block_cpd()/release_cpd(): the
 * cpufreq driver must prevent cluster power-down (CPD) while a frequency
 * transition is in flight, because collapsing the cluster mid-transition
 * is unsafe on Exynos8890.
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUIDLE_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUIDLE_H

#ifdef CONFIG_EXYNOS8890_CPUIDLE

void exynos8890_cpd_block(void);
void exynos8890_cpd_unblock(void);
bool exynos8890_cpd_is_active(unsigned int cpu);

#else

static inline void exynos8890_cpd_block(void) {}
static inline void exynos8890_cpd_unblock(void) {}
static inline bool exynos8890_cpd_is_active(unsigned int cpu) { return false; }

#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_CPUIDLE_H */
