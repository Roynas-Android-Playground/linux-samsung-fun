/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H

#include <linux/types.h>

struct device;

/* Claim or release exclusive direct-PMIC DVFS ownership from an APM provider. */
int exynos8890_apm_dvfs_claim(struct device *dev, bool claim);

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_APM_H */
