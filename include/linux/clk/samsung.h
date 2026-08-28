/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Krzysztof Kozlowski <krzk@kernel.org>
 */

#ifndef __LINUX_CLK_SAMSUNG_H_
#define __LINUX_CLK_SAMSUNG_H_

#include <linux/compiler_types.h>
#include <linux/errno.h>
#include <linux/types.h>

struct clk;
struct device_node;

int exynos8890_clk_sync_dmc(void);

#ifdef CONFIG_S3C64XX_COMMON_CLK
void s3c64xx_clk_init(struct device_node *np, unsigned long xtal_f,
		      unsigned long xusbxti_f, bool s3c6400,
		      void __iomem *base);
#else
static inline void s3c64xx_clk_init(struct device_node *np,
				    unsigned long xtal_f,
				    unsigned long xusbxti_f,
				    bool s3c6400, void __iomem *base) { }
#endif /* CONFIG_S3C64XX_COMMON_CLK */

#if IS_ENABLED(CONFIG_EXYNOS_ARM64_COMMON_CLK)
/*
 * Controls physically located beside the Exynos8890 CPU clock domains. The
 * CPU clock provider owns EMA and SMPL registers; CPUFreq only supplies the
 * characterized voltage and consumes the read-only SMPL diagnostic.
 */
int exynos8890_cpuclk_set_ema(struct clk *clk, u32 voltage_uv);
int exynos8890_cpuclk_get_error(struct clk *clk);
int exynos8890_cpuclk_smpl_status(struct clk *clk);
int exynos8890_cpuclk_smpl_trigger(struct clk *clk);
#else
static inline int exynos8890_cpuclk_set_ema(struct clk *clk, u32 voltage_uv)
{
	return -EOPNOTSUPP;
}

static inline int exynos8890_cpuclk_get_error(struct clk *clk)
{
	return -EOPNOTSUPP;
}

static inline int exynos8890_cpuclk_smpl_status(struct clk *clk)
{
	return -EOPNOTSUPP;
}

static inline int exynos8890_cpuclk_smpl_trigger(struct clk *clk)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __LINUX_CLK_SAMSUNG_H_ */
