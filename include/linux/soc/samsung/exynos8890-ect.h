/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H

#include <linux/types.h>

#define EXYNOS8890_ECT_MAX_LEVELS	32
#define EXYNOS8890_ECT_MAX_CLOCKS	16
#define EXYNOS8890_ECT_CLOCK_NAME_LEN	48

struct exynos8890_ect_cpu_level {
	u32 rate_khz;
	u32 voltage_uv;
	bool enabled;
	u32 clock_values[EXYNOS8890_ECT_MAX_CLOCKS];
};

struct exynos8890_ect_cpu_domain {
	char name[EXYNOS8890_ECT_CLOCK_NAME_LEN];
	u32 max_frequency;
	u32 min_frequency;
	s32 boot_level;
	s32 resume_level;
	u32 num_clocks;
	u32 num_levels;
	char clock_names[EXYNOS8890_ECT_MAX_CLOCKS][EXYNOS8890_ECT_CLOCK_NAME_LEN];
	struct exynos8890_ect_cpu_level levels[EXYNOS8890_ECT_MAX_LEVELS];
};

int exynos8890_ect_get_cpu_domain(const char *name,
				  struct exynos8890_ect_cpu_domain *domain);

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H */
