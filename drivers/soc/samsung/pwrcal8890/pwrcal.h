/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal pwrcal8890 sources still include "../pwrcal.h" (vendor's
 * original relative path). The real declarations live in the public
 * header so cal_dfs_*()/cal_clk_*() consumers outside this directory
 * (exynos8890-cpufreq, the exynos8890 devfreq drivers) share exactly one
 * copy - this just redirects internal includes to it.
 */
#include <linux/soc/samsung/exynos8890-pwrcal.h>
