/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CLK_EXYNOS8890_DVFS_H
#define __CLK_EXYNOS8890_DVFS_H

#include <linux/init.h>

struct device_node;
struct samsung_clk_provider;

int __init
exynos8890_dvfs_register_top(struct samsung_clk_provider *ctx,
			     struct device_node *np);

int __init
exynos8890_dvfs_register_g3d(struct samsung_clk_provider *ctx,
			     struct device_node *np);

#endif /* __CLK_EXYNOS8890_DVFS_H */
