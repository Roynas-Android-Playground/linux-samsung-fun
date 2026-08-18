/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exynos Specific Extensions for Synopsys DW Multimedia Card Interface driver
 *
 * Copyright (C) 2012-2014 Samsung Electronics Co., Ltd.
 */

#ifndef _DW_MMC_EXYNOS_H_
#define _DW_MMC_EXYNOS_H_

#define SDMMC_CLKSEL			0x09C
#define SDMMC_CLKSEL64			0x0A8

/* Extended Register's Offset */
#define SDMMC_HS400_DQS_EN		0x180
#define SDMMC_HS400_ASYNC_FIFO_CTRL	0x184
#define SDMMC_HS400_DLINE_CTRL		0x188

/*
 * Samsung-specific Hardware Auto Clock Gating control. Not part of the
 * generic Synopsys DesignWare MMC register map. On exynos8890, the
 * controller's clock domain is gated by the SoC's Q-active/HWACG logic;
 * MMC_HWACG_CONTROL must be deasserted while running the low-speed (<=
 * 400kHz) identification clock and asserted once switched to the real
 * operating frequency, or the first data-phase transfer after the switch
 * times out (DRTO) because the CIU clock isn't kept ungated in time.
 */
#define SDMMC_FORCE_CLK_STOP		0x0B0
#define MMC_HWACG_CONTROL		BIT(4)
#define SDMMC_AXI_BURST_LEN		0x0B4
#define SDMMC_VOLTAGE_INT_EXTRA_MASK	GENMASK(26, 24)

/*
 * exynos8890's SD/MMC data path runs through an inline FMP (Flash Memory
 * Protector) hardware block, sitting at a fixed +0x1000 offset from the
 * controller's own register base, that is left in a blocking/undefined
 * state by the bootloader. Program descriptor bypass through EL3 before the
 * controller is powered for data transfers, matching Samsung's secure ABI.
 */
#define EXYNOS8890_FMP_OFFSET		0x1000
#define EXYNOS8890_SD_FIFOTH		0x300f0030
#define SMC_CMD_FMP			0xC2001810
#define FMP_SECURITY			0x8
#define FMP_DESC_OFF			0x0

/* CLKSEL register defines */
#define SDMMC_CLKSEL_CCLK_SAMPLE(x)	(((x) & 7) << 0)
#define SDMMC_CLKSEL_CCLK_DRIVE(x)	(((x) & 7) << 16)
#define SDMMC_CLKSEL_CCLK_DIVIDER(x)	(((x) & 7) << 24)
#define SDMMC_CLKSEL_GET_DRV_WD3(x)	(((x) >> 16) & 0x7)
#define SDMMC_CLKSEL_GET_DIV(x)		(((x) >> 24) & 0x7)
#define SDMMC_CLKSEL_UP_SAMPLE(x, y)	(((x) & ~SDMMC_CLKSEL_CCLK_SAMPLE(7)) |\
					 SDMMC_CLKSEL_CCLK_SAMPLE(y))
#define SDMMC_CLKSEL_TIMING(x, y, z)	(SDMMC_CLKSEL_CCLK_SAMPLE(x) |	\
					 SDMMC_CLKSEL_CCLK_DRIVE(y) |	\
					 SDMMC_CLKSEL_CCLK_DIVIDER(z))
#define SDMMC_CLKSEL_TIMING_MASK	SDMMC_CLKSEL_TIMING(0x7, 0x7, 0x7)
#define SDMMC_CLKSEL_WAKEUP_INT		BIT(11)

/* RCLK_EN register defines */
#define DATA_STROBE_EN			BIT(0)
#define AXI_NON_BLOCKING_WR	BIT(7)

/* DLINE_CTRL register defines */
#define DQS_CTRL_RD_DELAY(x, y)		(((x) & ~0x3FF) | ((y) & 0x3FF))
#define DQS_CTRL_GET_RD_DELAY(x)	((x) & 0x3FF)

/* Protector Register */
#define SDMMC_EMMCP_BASE	0x1000
#define SDMMC_MPSTAT		(SDMMC_EMMCP_BASE + 0x0008)
#define SDMMC_MPSECURITY	(SDMMC_EMMCP_BASE + 0x0010)
#define SDMMC_MPSBEGIN0		(SDMMC_EMMCP_BASE + 0x0200)
#define SDMMC_MPSEND0		(SDMMC_EMMCP_BASE + 0x0204)
#define SDMMC_MPSCTRL0		(SDMMC_EMMCP_BASE + 0x020C)

/* SMU control defines */
#define SDMMC_MPSCTRL_SECURE_READ_BIT		BIT(7)
#define SDMMC_MPSCTRL_SECURE_WRITE_BIT		BIT(6)
#define SDMMC_MPSCTRL_NON_SECURE_READ_BIT	BIT(5)
#define SDMMC_MPSCTRL_NON_SECURE_WRITE_BIT	BIT(4)
#define SDMMC_MPSCTRL_USE_FUSE_KEY		BIT(3)
#define SDMMC_MPSCTRL_ECB_MODE			BIT(2)
#define SDMMC_MPSCTRL_ENCRYPTION		BIT(1)
#define SDMMC_MPSCTRL_VALID			BIT(0)

/* Maximum number of Ending sector */
#define SDMMC_ENDING_SEC_NR_MAX	0xFFFFFFFF

/* Fixed clock divider */
#define EXYNOS4210_FIXED_CIU_CLK_DIV	2
#define EXYNOS4412_FIXED_CIU_CLK_DIV	4
#define HS400_FIXED_CIU_CLK_DIV		1

/* Minimal required clock frequency for cclkin, unit: HZ */
#define EXYNOS_CCLKIN_MIN	50000000
#define EXYNOS8890_CCLKIN_MIN	25000000

/* SD identification-mode clock ceiling, unit: HZ */
#define EXYNOS_CCLKIN_MIN_IDENT	400000

#endif /* _DW_MMC_EXYNOS_H_ */
