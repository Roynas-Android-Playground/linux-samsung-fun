/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos8890 PM event codes - port of vendor Cronos_8890
 * include/soc/samsung/exynos-pm.h.
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_PM_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_PM_H

#include <linux/notifier.h>

enum exynos_pm_event {
	/* CPU is entering the LPA state */
	LPA_ENTER,
	LPA_ENTER_FAIL,
	LPA_EXIT,

	/* CPU is entering/exiting the SICD / SICD_AUD state */
	SICD_ENTER,
	SICD_AUD_ENTER,
	SICD_EXIT,
	SICD_AUD_EXIT,
};

#ifdef CONFIG_EXYNOS8890_PM

int exynos_pm_register_notifier(struct notifier_block *nb);
int exynos_pm_unregister_notifier(struct notifier_block *nb);
int exynos_pm_notify(enum exynos_pm_event event);
bool exynos8890_pm_system_sleep_completed(void);

#else

static inline int exynos_pm_register_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline int exynos_pm_unregister_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline int exynos_pm_notify(enum exynos_pm_event event)
{
	return 0;
}

static inline bool exynos8890_pm_system_sleep_completed(void)
{
	return false;
}

#endif /* CONFIG_EXYNOS8890_PM */

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_PM_H */
