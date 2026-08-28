/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_DMC_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_DMC_H

#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct exynos8890_dmc;

#if IS_ENABLED(CONFIG_EXYNOS8890_DMC)
struct exynos8890_dmc *exynos8890_dmc_get(struct device *consumer);
void exynos8890_dmc_put(struct exynos8890_dmc *dmc);
unsigned long exynos8890_dmc_get_rate(struct exynos8890_dmc *dmc);
int exynos8890_dmc_get_voltage(struct exynos8890_dmc *dmc,
			       unsigned long rate_hz, u32 *voltage_uv);
int exynos8890_dmc_set_rate(struct exynos8890_dmc *dmc,
			    unsigned long target_rate_hz);
void exynos8890_dmc_dump(struct exynos8890_dmc *dmc);
#else
static inline struct exynos8890_dmc *
exynos8890_dmc_get(struct device *consumer)
{
	return ERR_PTR(-ENODEV);
}

static inline int exynos8890_dmc_get_voltage(struct exynos8890_dmc *dmc,
					     unsigned long rate_hz,
					     u32 *voltage_uv)
{
	return -ENODEV;
}

static inline void exynos8890_dmc_put(struct exynos8890_dmc *dmc) { }

static inline unsigned long
exynos8890_dmc_get_rate(struct exynos8890_dmc *dmc)
{
	return 0;
}

static inline int exynos8890_dmc_set_rate(struct exynos8890_dmc *dmc,
					  unsigned long target_rate_hz)
{
	return -ENODEV;
}

static inline void exynos8890_dmc_dump(struct exynos8890_dmc *dmc) { }
#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_DMC_H */
