/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __EXYNOS8890_DMC_INTERNAL_H
#define __EXYNOS8890_DMC_INTERNAL_H

#include <linux/io.h>
#include <linux/types.h>

struct exynos8890_dmc;

/*
 * These are physical SFR addresses, used only as stable keys by the timing
 * port.  exynos8890_dmc_{read,write}() resolves them against the mappings
 * owned by the DMC platform device; no caller dereferences them directly.
 */
#define CMU_TOP_BASE		0x10570000UL
#define CMU_CCORE_BASE		0x105b0000UL
#define CMU_MIF0_BASE		0x10850000UL
#define CMU_MIF1_BASE		0x10950000UL
#define CMU_MIF2_BASE		0x10a50000UL
#define CMU_MIF3_BASE		0x10b50000UL
#define PMU_ALIVE_BASE		0x105c0000UL
#define DMC_MISC_CCORE_BASE	0x10520000UL
#define SMC0_BASE		0x10800000UL
#define SMC1_BASE		0x10900000UL
#define SMC2_BASE		0x10a00000UL
#define SMC3_BASE		0x10b00000UL
#define LPDDR4_PHY0_BASE	0x10820000UL
#define LPDDR4_PHY1_BASE	0x10920000UL
#define LPDDR4_PHY2_BASE	0x10a20000UL
#define LPDDR4_PHY3_BASE	0x10b20000UL
#define DMC_MISC0_BASE		0x10890000UL
#define DMC_MISC1_BASE		0x10990000UL
#define DMC_MISC2_BASE		0x10a90000UL
#define DMC_MISC3_BASE		0x10b90000UL

u32 exynos8890_dmc_read_addr(uintptr_t sfr);
void exynos8890_dmc_write_addr(uintptr_t sfr, u32 value);

/* Register constants are address keys, never directly dereferenced. */
#define exynos8890_dmc_read(_sfr) \
	exynos8890_dmc_read_addr((uintptr_t)(_sfr))
#define exynos8890_dmc_write(_sfr, _value) \
	exynos8890_dmc_write_addr((uintptr_t)(_sfr), (_value))
int exynos8890_dmc_timing_init(struct exynos8890_dmc *dmc);
void exynos8890_dmc_timing_exit(struct exynos8890_dmc *dmc);
int exynos8890_dmc_program_timing(unsigned long target_hz,
				  unsigned int timing_set);

#endif /* __EXYNOS8890_DMC_INTERNAL_H */
