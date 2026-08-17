/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *	      http://www.samsung.com/
 *
 * Exynos - Support SoC specific Reboot
 * Author: Hosung Kim <hosung0.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/input.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/gpio/legacy.h>

static void __iomem *exynos_pmu_base = NULL;

static const char *const mngs_cores[] = {
	"arm,mongoose-m1",
	NULL,
};

static bool is_mngs_cpu(struct device_node *cn)
{
	const char *const *lc;
	for (lc = mngs_cores; *lc; lc++)
		if (of_device_is_compatible(cn, *lc))
			return true;
	return false;
}

static int soc_has_mongoose(void)
{
	struct device_node *cn = NULL;
	u32 mngs_cpu_cnt = 0;

	/* find arm,mongoose-m1 compatable in device tree */
	while ((cn = of_find_node_by_type(cn, "cpu"))) {
		if (is_mngs_cpu(cn))
			mngs_cpu_cnt++;
	}
	return mngs_cpu_cnt;
}

/* defines for MNGS reset */
#define PEND_MNGS (1 << 1)
#define PEND_APOLLO (1 << 0)
#define DEFAULT_VAL_CPU_RESET_DISABLE (0xFFFFFFFC)
#define RESET_DISABLE_GPR_CPUPORESET (1 << 15)
#define RESET_DISABLE_WDT_CPUPORESET (1 << 12)
#define RESET_DISABLE_CORERESET (1 << 9)
#define RESET_DISABLE_CPUPORESET (1 << 8)
#define RESET_DISABLE_WDT_PRESET_DBG (1 << 25)
#define RESET_DISABLE_PRESET_DBG (1 << 18)
#define RESET_DISABLE_L2RESET (1 << 16)
#define RESET_DISABLE_WDT_L2RESET (1 << 31)

#define EXYNOS_PMU_CPU_RESET_DISABLE_FROM_SOFTRESET (0x041C)
#define EXYNOS_PMU_CPU_RESET_DISABLE_FROM_WDTRESET (0x0414)
#define EXYNOS_PMU_ATLAS_CPU0_RESET (0x200C)
#define EXYNOS_PMU_ATLAS_DBG_RESET (0x244C)
#define EXYNOS_PMU_ATLAS_NONCPU_RESET (0x240C)
#define EXYNOS_PMU_SWRESET (0x0400)
#define EXYNOS_PMU_RESET_SEQUENCER_CONFIGURATION (0x0500)
#define EXYNOS_PMU_PS_HOLD_CONTROL (0x330C)

static void mngs_reset_control(int en)
{
	u32 reg_val, val;
	u32 mngs_cpu_cnt = soc_has_mongoose();

	if (mngs_cpu_cnt == 0 || !exynos_pmu_base)
		return;

	if (en) {
		/* reset disable for MNGS */
		pr_err("%s: mngs cpu reset disable\n", __func__);
		reg_val = readl(exynos_pmu_base +
				EXYNOS_PMU_CPU_RESET_DISABLE_FROM_SOFTRESET);
		if (reg_val & (PEND_MNGS | PEND_APOLLO)) {
			reg_val &= ~(PEND_MNGS | PEND_APOLLO);
			writel(reg_val,
			       exynos_pmu_base +
				       EXYNOS_PMU_CPU_RESET_DISABLE_FROM_SOFTRESET);
		}

		reg_val = readl(exynos_pmu_base +
				EXYNOS_PMU_CPU_RESET_DISABLE_FROM_WDTRESET);
		if (reg_val != DEFAULT_VAL_CPU_RESET_DISABLE) {
			reg_val &= ~(PEND_MNGS | PEND_APOLLO);
			writel(reg_val,
			       exynos_pmu_base +
				       EXYNOS_PMU_CPU_RESET_DISABLE_FROM_WDTRESET);
		}

		for (val = 0; val < mngs_cpu_cnt; val++) {
			reg_val = readl(exynos_pmu_base +
					EXYNOS_PMU_ATLAS_CPU0_RESET +
					(val * 0x80));
			reg_val |= (RESET_DISABLE_WDT_CPUPORESET |
				    RESET_DISABLE_CORERESET |
				    RESET_DISABLE_CPUPORESET);
			writel(reg_val, exynos_pmu_base +
						EXYNOS_PMU_ATLAS_CPU0_RESET +
						(val * 0x80));
		}

		reg_val = readl(exynos_pmu_base + EXYNOS_PMU_ATLAS_DBG_RESET);
		reg_val |= (RESET_DISABLE_WDT_PRESET_DBG |
			    RESET_DISABLE_PRESET_DBG);
		writel(reg_val, exynos_pmu_base + EXYNOS_PMU_ATLAS_DBG_RESET);

		reg_val =
			readl(exynos_pmu_base + EXYNOS_PMU_ATLAS_NONCPU_RESET);
		reg_val |= (RESET_DISABLE_L2RESET | RESET_DISABLE_WDT_L2RESET);
		writel(reg_val,
		       exynos_pmu_base + EXYNOS_PMU_ATLAS_NONCPU_RESET);
	} else {
		/* reset enable for MNGS */
		pr_err("%s: mngs cpu reset enable before s/w reset\n",
		       __func__);
		for (val = 0; val < mngs_cpu_cnt; val++) {
			reg_val = readl(exynos_pmu_base +
					EXYNOS_PMU_ATLAS_CPU0_RESET +
					(val * 0x80));
			reg_val &= ~(RESET_DISABLE_WDT_CPUPORESET |
				     RESET_DISABLE_CORERESET |
				     RESET_DISABLE_CPUPORESET);
			writel(reg_val, exynos_pmu_base +
						EXYNOS_PMU_ATLAS_CPU0_RESET +
						(val * 0x80));
		}

		reg_val = readl(exynos_pmu_base + EXYNOS_PMU_ATLAS_DBG_RESET);
		reg_val &= ~(RESET_DISABLE_WDT_PRESET_DBG |
			     RESET_DISABLE_PRESET_DBG);
		writel(reg_val, exynos_pmu_base + EXYNOS_PMU_ATLAS_DBG_RESET);

		reg_val =
			readl(exynos_pmu_base + EXYNOS_PMU_ATLAS_NONCPU_RESET);
		reg_val &= ~(RESET_DISABLE_L2RESET | RESET_DISABLE_WDT_L2RESET);
		writel(reg_val,
		       exynos_pmu_base + EXYNOS_PMU_ATLAS_NONCPU_RESET);
	}
}

#define DFD_EDPCSR_DUMP_EN (1 << 0)
#define DFD_L2RSTDISABLE_MNGS_EN (1 << 11)
#define DFD_DBGL1RSTDISABLE_MNGS_EN (1 << 10)
#define DFD_L2RSTDISABLE_APOLLO_EN (1 << 9)
#define DFD_DBGL1RSTDISABLE_APOLLO_EN (1 << 8)
#define DFD_CLEAR_L2RSTDISABLE_MNGS (1 << 7)
#define DFD_CLEAR_DBGL1RSTDISABLE_MNGS (1 << 6)
#define DFD_CLEAR_L2RSTDISABLE_APOLLO (1 << 5)
#define DFD_CLEAR_DBGL1RSTDISABLE_APOLLO (1 << 4)

static void dfd_set_dump_gpr(int en)
{
	u32 reg_val;

	if (en) {
		reg_val = DFD_EDPCSR_DUMP_EN | DFD_L2RSTDISABLE_MNGS_EN |
			  DFD_DBGL1RSTDISABLE_MNGS_EN |
			  DFD_L2RSTDISABLE_APOLLO_EN |
			  DFD_DBGL1RSTDISABLE_APOLLO_EN;
		writel(reg_val,
		       exynos_pmu_base +
			       EXYNOS_PMU_RESET_SEQUENCER_CONFIGURATION);
	} else {
		reg_val = readl(exynos_pmu_base +
				EXYNOS_PMU_RESET_SEQUENCER_CONFIGURATION);
		if (reg_val) {
			reg_val = DFD_EDPCSR_DUMP_EN |
				  DFD_CLEAR_L2RSTDISABLE_MNGS |
				  DFD_CLEAR_DBGL1RSTDISABLE_MNGS |
				  DFD_CLEAR_L2RSTDISABLE_APOLLO |
				  DFD_CLEAR_DBGL1RSTDISABLE_APOLLO;
		}
		writel(reg_val,
		       exynos_pmu_base +
			       EXYNOS_PMU_RESET_SEQUENCER_CONFIGURATION);
	}
}

#define EXYNOS_PMU_INFORM2	0x0808
#define EXYNOS_PMU_INFORM3	0x080c

#define INFORM2_REBOOT_MAGIC	0x12345678
#define INFORM3_MODE_NORMAL	0x12345670
#define INFORM3_MODE_DOWNLOAD	0x12345671
#define INFORM3_MODE_UPLOAD	0x12345672
#define INFORM3_MODE_RECOVERY	0x12345674
#define INFORM3_MODE_BOOTLOADER	0x1234567d

static int exynos_reboot_notifier(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	u32 restart_inform;

	if (!exynos_pmu_base)
		return NOTIFY_DONE;

	restart_inform = INFORM3_MODE_NORMAL;

	if (cmd) {
		if (!strcmp((char *)cmd, "recovery"))
			restart_inform = INFORM3_MODE_RECOVERY;
		else if (!strcmp((char *)cmd, "download"))
			restart_inform = INFORM3_MODE_DOWNLOAD;
		else if (!strcmp((char *)cmd, "bootloader"))
			restart_inform = INFORM3_MODE_BOOTLOADER;
		else if (!strcmp((char *)cmd, "ramdump") ||
			 !strcmp((char *)cmd, "upload"))
			restart_inform = INFORM3_MODE_UPLOAD;
	}

	writel(INFORM2_REBOOT_MAGIC,
	       exynos_pmu_base + EXYNOS_PMU_INFORM2);
	writel(restart_inform, exynos_pmu_base + EXYNOS_PMU_INFORM3);

	/* Check reset_sequencer_configuration register */
	if (readl(exynos_pmu_base + EXYNOS_PMU_RESET_SEQUENCER_CONFIGURATION) &
	    DFD_EDPCSR_DUMP_EN) {
		dfd_set_dump_gpr(0);
		mngs_reset_control(0);
	}

	/* Do S/W Reset */
	__raw_writel(0x1, exynos_pmu_base + EXYNOS_PMU_SWRESET);
	return NOTIFY_DONE;
}

static struct notifier_block exynos_restart_nb = {
	.notifier_call = exynos_reboot_notifier,
	.priority = 200,
};

static int __init exynos_reboot_setup(struct device_node *np)
{
	int err = 0;
	u32 id;

	if (!of_property_read_u32(np, "pmu_base", &id)) {
		exynos_pmu_base = ioremap(id, SZ_16K);
		if (!exynos_pmu_base) {
			pr_err("%s: failed to map to exynos-pmu-base address 0x%x\n",
			       __func__, id);
			err = -ENOMEM;
		}
	}

	of_node_put(np);
	return err;
}

static const struct of_device_id reboot_of_match[] __initconst = {
	{ .compatible = "exynos8890,reboot", .data = exynos_reboot_setup },
	{},
};

typedef int (*reboot_initcall_t)(const struct device_node *);

static int __init exynos_reboot_init(void)
{
	struct device_node *np;
	const struct of_device_id *matched_np;
	reboot_initcall_t init_fn;
	int rc;

	np = of_find_matching_node_and_match(NULL, reboot_of_match,
					     &matched_np);
	if (!np)
		return -ENODEV;

	rc = register_restart_handler(&exynos_restart_nb);
	if (rc) {
		pr_err("%s: failed to register restart handler\n", __func__);
		of_node_put(np);
		return rc;
	}

	init_fn = (reboot_initcall_t)matched_np->data;

	return init_fn(np);
}
subsys_initcall(exynos_reboot_init);