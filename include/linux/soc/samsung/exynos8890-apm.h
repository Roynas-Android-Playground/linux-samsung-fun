/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

/* Claim exclusive direct-PMIC DVFS ownership from an APM provider. */
#if IS_REACHABLE(CONFIG_EXYNOS8890_APM)
int exynos8890_apm_dvfs_claim(struct device *dev, bool claim);
bool exynos8890_apm_dvfs_ready(void);
#else
static inline int exynos8890_apm_dvfs_claim(struct device *dev, bool claim)
{
	return -ENODEV;
}

static inline bool exynos8890_apm_dvfs_ready(void)
{
	return false;
}
#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H */
