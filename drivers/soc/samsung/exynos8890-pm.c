// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 system suspend coordination.
 *
 * The PMU sleep policy is native Exynos code and CMU context is owned only by
 * the Samsung common clock framework.  In particular, this path never calls
 * PWRCAL and never saves, restores, or force-enables CMU registers.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include <linux/psci.h>
#include <uapi/linux/psci.h>

#include <asm/memory.h>
#include <asm/suspend.h>

#include <linux/soc/samsung/exynos8890-pm.h>
#include <linux/soc/samsung/exynos8890-idle-ip.h>
#include <linux/soc/samsung/exynos-pmu.h>

#include "exynos8890-pmu.h"

#define EXYNOS8890_NUM_POWER_MODES	9
#define EXYNOS8890_SLEEP_MODE		8

/*
 * Cronos' pseudo-index 132 was translated inside its old arm64 PSCI code to
 * this actual state: ID 0, type 0, affinity level 3.  Passing 132 directly to
 * modern psci_cpu_suspend_enter() loses that translation and also selects its
 * context-retaining path with a zero resume address.  Keep the firmware ABI,
 * but use cpu_suspend() explicitly so the complete arm64 context and physical
 * cpu_resume address are supplied.
 */
#define EXYNOS8890_PSCI_SYSTEM_SLEEP_STATE \
	(3U << PSCI_0_2_POWER_STATE_AFFL_SHIFT)

#define EXYNOS8890_PA_GPIO_ALIVE	0x10580000
#define WAKEUP_STAT_EINT		BIT(0)
#define WAKEUP_STAT_RTC_ALARM		BIT(1)
#define WAKEUP_STAT_INT_MBOX		BIT(24)

/* PMU register offsets */
#define EXYNOS_PMU_WAKEUP_STAT		0x0600
#define EXYNOS_PMU_WAKEUP_MASK		0x0610
#define EXYNOS_PMU_WAKEUP_MASK2		0x0614
#define EXYNOS_PMU_WAKEUP_MASK3		0x0618

#define EXYNOS8890_PMU_MNGS_LPI_MASK	0x0020
#define EXYNOS8890_MNGS_LPI_AUD		BIT(15)
#define EXYNOS8890_MNGS_LPI_CAM1	BIT(16)

#define EXYNOS_PMU_IDLE_IP(x)		(0x03E0 + (x) * 4)
#define EXYNOS_PMU_IDLE_IP_MASK(x)	(0x03F0 + (x) * 4)
#define NUM_IDLE_IP_REG			4

#define NUM_WAKEUP_MASK			3

static void __iomem *eint_base;
static struct regmap *pmu_regmap;
static struct regmap *pmu_mngs_regmap;
static bool early_wakeup = true;
static bool suspend_entered;
static bool system_sleep_completed;

bool exynos8890_pm_system_sleep_completed(void)
{
	return READ_ONCE(system_sleep_completed);
}
EXPORT_SYMBOL_GPL(exynos8890_pm_system_sleep_completed);

/*
 * Per-powerdown-mode wakeup masks (WAKEUP_MASK/2/3), parsed from DT.
 * Index order matches vendor's sys_powerdown enum: sicd, sicd_cpd,
 * sicd_aud, aftr, stop, dstop, lpd, alpa, sleep.
 */
static u32 wakeup_mask[EXYNOS8890_NUM_POWER_MODES][NUM_WAKEUP_MASK];

/*
 * Per-mode idle-IP masks. Bit n set = IP n masked out of the sequencer's
 * idle check. Starts all-ones (ignore every IP); DT idle_ip_mask child
 * nodes clear bits for IPs that must gate the given mode.
 */
static u32 idle_ip_mask[EXYNOS8890_NUM_POWER_MODES][NUM_IDLE_IP_REG];
static DEFINE_SPINLOCK(idle_ip_lock);

/* ------------------------------------------------------------------ */
/* notifier chain                                                      */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_CPU_IDLE
static DEFINE_RWLOCK(pm_notifier_lock);
static RAW_NOTIFIER_HEAD(pm_notifier_chain);

int exynos_pm_register_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	write_lock_irqsave(&pm_notifier_lock, flags);
	ret = raw_notifier_chain_register(&pm_notifier_chain, nb);
	write_unlock_irqrestore(&pm_notifier_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos_pm_register_notifier);

int exynos_pm_unregister_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	write_lock_irqsave(&pm_notifier_lock, flags);
	ret = raw_notifier_chain_unregister(&pm_notifier_chain, nb);
	write_unlock_irqrestore(&pm_notifier_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos_pm_unregister_notifier);

int exynos_pm_notify(enum exynos_pm_event event)
{
	int ret;

	read_lock(&pm_notifier_lock);
	ret = raw_notifier_call_chain(&pm_notifier_chain, event, NULL);
	read_unlock(&pm_notifier_lock);

	return notifier_to_errno(ret);
}
EXPORT_SYMBOL_GPL(exynos_pm_notify);

#else /* !CONFIG_CPU_IDLE */

int exynos_pm_register_notifier(struct notifier_block *nb) { return 0; }
EXPORT_SYMBOL_GPL(exynos_pm_register_notifier);
int exynos_pm_unregister_notifier(struct notifier_block *nb) { return 0; }
EXPORT_SYMBOL_GPL(exynos_pm_unregister_notifier);
int exynos_pm_notify(enum exynos_pm_event event) { return 0; }
EXPORT_SYMBOL_GPL(exynos_pm_notify);

#endif /* CONFIG_CPU_IDLE */

/* ------------------------------------------------------------------ */
/* wakeup reason                                                       */
/* ------------------------------------------------------------------ */

#define EXYNOS_EINT_PEND(base, x)	((base) + 0xA00 + (((x) >> 3) * 4))

static void show_wakeup_registers(unsigned long wakeup_stat)
{
	pr_info("PM: WAKEUP_STAT: 0x%08lx\n", wakeup_stat);
	if (eint_base)
		pr_info("PM: EINT_PEND: 0x%02x, 0x%02x, 0x%02x, 0x%02x\n",
			readl_relaxed(EXYNOS_EINT_PEND(eint_base, 0)),
			readl_relaxed(EXYNOS_EINT_PEND(eint_base, 8)),
			readl_relaxed(EXYNOS_EINT_PEND(eint_base, 16)),
			readl_relaxed(EXYNOS_EINT_PEND(eint_base, 24)));
}

static void show_wakeup_reason(bool sleep_abort)
{
	unsigned int wakeup_stat = 0;

	if (sleep_abort)
		pr_info("PM: early wakeup!\n");

	if (pmu_regmap)
		regmap_read(pmu_regmap, EXYNOS_PMU_WAKEUP_STAT, &wakeup_stat);

	show_wakeup_registers(wakeup_stat);

	if (wakeup_stat & WAKEUP_STAT_RTC_ALARM)
		pr_info("PM: Resume caused by RTC alarm\n");
	else if (wakeup_stat & WAKEUP_STAT_INT_MBOX)
		pr_info("PM: Resume caused by CP mailbox\n");
	else if (wakeup_stat & WAKEUP_STAT_EINT)
		pr_info("PM: Resume caused by external interrupt\n");
	else if (wakeup_stat)
		pr_info("PM: Resume caused by wakeup_stat 0x%08x\n",
			wakeup_stat);
}

/* ------------------------------------------------------------------ */
/* idle-IP control                                                     */
/* ------------------------------------------------------------------ */

static int convert_idle_ip_index(int *ip_index)
{
	int reg_index = *ip_index / EXYNOS8890_IDLE_IP_REG_SIZE;

	*ip_index %= EXYNOS8890_IDLE_IP_REG_SIZE;

	return reg_index;
}

int exynos8890_get_idle_ip_index(const char *ip_name)
{
	struct device_node *np;
	int ip_index;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos8890-pm");
	if (!np)
		return -ENODEV;

	ip_index = of_property_match_string(np, "samsung,idle-ip-names", ip_name);
	of_node_put(np);

	if (ip_index < 0)
		return ip_index;

	if (ip_index > EXYNOS8890_IDLE_IP_MAX_CONFIGURABLE) {
		pr_err("PM: %s index %d out of range\n", ip_name, ip_index);
		return -EINVAL;
	}

	return ip_index;
}
EXPORT_SYMBOL_GPL(exynos8890_get_idle_ip_index);

void exynos8890_update_ip_idle_status(int ip_index, int idle)
{
	unsigned long flags;
	int reg_index;

	if (ip_index < 0 || ip_index > EXYNOS8890_IDLE_IP_MAX_CONFIGURABLE ||
	    !pmu_regmap)
		return;

	reg_index = convert_idle_ip_index(&ip_index);

	spin_lock_irqsave(&idle_ip_lock, flags);
	regmap_update_bits(pmu_regmap, EXYNOS_PMU_IDLE_IP(reg_index),
			   BIT(ip_index), idle ? BIT(ip_index) : 0);
	spin_unlock_irqrestore(&idle_ip_lock, flags);
}
EXPORT_SYMBOL_GPL(exynos8890_update_ip_idle_status);

/*
 * Returns 0 when every IP not masked for SYS_SLEEP reports idle.
 * A nonzero return names the first non-idle register index.
 */
int exynos8890_check_idle_ip(void)
{
	unsigned int val, mask;
	int reg;

	if (!pmu_regmap)
		return 0;

	for (reg = 0; reg < NUM_IDLE_IP_REG; reg++) {
		mask = idle_ip_mask[EXYNOS8890_SLEEP_MODE][reg];
		if (regmap_read(pmu_regmap, EXYNOS_PMU_IDLE_IP(reg), &val))
			continue;

		/* bit clear + mask clear = non-idle IP blocking the mode */
		if ((val & ~mask) != ~mask)
			return reg + 1;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_check_idle_ip);

static void exynos_set_idle_ip_mask(int mode)
{
	unsigned long flags;
	int reg;

	spin_lock_irqsave(&idle_ip_lock, flags);
	for (reg = 0; reg < NUM_IDLE_IP_REG; reg++)
		regmap_write(pmu_regmap, EXYNOS_PMU_IDLE_IP_MASK(reg),
			     idle_ip_mask[mode][reg]);
	spin_unlock_irqrestore(&idle_ip_lock, flags);
}

static void parse_idle_ip_masks(struct device_node *np)
{
	struct device_node *mask_np, *child;
	int reg, bit;

	memset(idle_ip_mask, 0xFF, sizeof(idle_ip_mask));

	mask_np = of_get_child_by_name(np, "idle-ip-masks");
	if (!mask_np)
		return;

	for_each_child_of_node(mask_np, child) {
		u32 mode;
		int count, i;

		if (of_property_read_u32(child, "samsung,mode-index", &mode))
			continue;
		if (mode >= EXYNOS8890_NUM_POWER_MODES)
			continue;

		count = of_property_count_u32_elems(child,
						    "samsung,required-idle-ip");
		for (i = 0; i < count; i++) {
			u32 idx;

			if (of_property_read_u32_index(child, "samsung,required-idle-ip",
						       i, &idx))
				continue;
			if (idx > EXYNOS8890_IDLE_IP_MAX_CONFIGURABLE)
				continue;

			bit = idx;
			reg = convert_idle_ip_index(&bit);
			idle_ip_mask[mode][reg] &= ~BIT(bit);
		}
	}

	of_node_put(mask_np);
}

/* ------------------------------------------------------------------ */
/* wakeup masks                                                        */
/* ------------------------------------------------------------------ */

static void parse_wakeup_masks(struct device_node *np)
{
	int mode, ret;

	for (mode = 0; mode < EXYNOS8890_NUM_POWER_MODES; mode++) {
		ret = of_property_read_u32_index(np, "samsung,wakeup-mask",
						 mode,
						 &wakeup_mask[mode][0]);
		ret |= of_property_read_u32_index(np, "samsung,wakeup-mask2",
						  mode,
						  &wakeup_mask[mode][1]);
		ret |= of_property_read_u32_index(np, "samsung,wakeup-mask3",
						  mode,
						  &wakeup_mask[mode][2]);
		if (ret)
			pr_warn("PM: incomplete wakeup_mask DT data for mode %d\n",
				mode);
	}
}

static void exynos_set_wakeupmask(int mode)
{
	if (!pmu_regmap)
		return;

	regmap_write(pmu_regmap, EXYNOS_PMU_WAKEUP_MASK,
		     wakeup_mask[mode][0]);
	regmap_write(pmu_regmap, EXYNOS_PMU_WAKEUP_MASK2,
		     wakeup_mask[mode][1]);
	regmap_write(pmu_regmap, EXYNOS_PMU_WAKEUP_MASK3,
		     wakeup_mask[mode][2]);
}

/* ------------------------------------------------------------------ */
/* suspend                                                             */
/* ------------------------------------------------------------------ */

static int exynos_pm_syscore_suspend(void *data)
{
	int blocked;

	blocked = exynos8890_check_idle_ip();
	if (blocked) {
		pr_info("PM: sleep aborted: IDLE_IP%d reports a busy block\n",
			blocked - 1);
		return -EBUSY;
	}

	exynos_set_idle_ip_mask(EXYNOS8890_SLEEP_MODE);
	exynos_set_wakeupmask(EXYNOS8890_SLEEP_MODE);
	exynos_sys_powerdown_conf(SYS_SLEEP);

	pr_info("PM: Enter sleep mode\n");

	return 0;
}

static void exynos_pm_syscore_resume(void *data)
{
	bool sequence_completed;

	/* Capture the firmware completion bit before the PMU abort repair sets it. */
	sequence_completed = exynos8890_pmu_system_resume();
	WRITE_ONCE(system_sleep_completed,
		   suspend_entered && sequence_completed);
	early_wakeup = !READ_ONCE(system_sleep_completed);

	if (early_wakeup && !sequence_completed)
		pr_info("PM: system sleep did not complete; treating wake as abort\n");

	regmap_update_bits(pmu_mngs_regmap, EXYNOS8890_PMU_MNGS_LPI_MASK,
			   EXYNOS8890_MNGS_LPI_AUD |
			   EXYNOS8890_MNGS_LPI_CAM1,
			   EXYNOS8890_MNGS_LPI_AUD |
			   EXYNOS8890_MNGS_LPI_CAM1);

	show_wakeup_reason(early_wakeup);
}

static const struct syscore_ops exynos_pm_syscore_ops = {
	.suspend = exynos_pm_syscore_suspend,
	.resume	 = exynos_pm_syscore_resume,
};

static struct syscore exynos_pm_syscore __maybe_unused = {
	.ops = &exynos_pm_syscore_ops,
};

static noinstr int exynos8890_suspend_finisher(unsigned long state)
{
	phys_addr_t pa_cpu_resume;

	pa_cpu_resume = __pa_symbol_nodebug((unsigned long)cpu_resume);

	return psci_ops.cpu_suspend((u32)state, pa_cpu_resume);
}

static int exynos_pm_enter(suspend_state_t state)
{
	int ret;

	pm_set_resume_via_firmware();
	suspend_entered = true;
	ret = cpu_suspend(EXYNOS8890_PSCI_SYSTEM_SLEEP_STATE,
			  exynos8890_suspend_finisher);
	if (ret)
		pr_info("PM: firmware rejected system sleep: %d\n", ret);

	return ret;
}

static int exynos_pm_begin(suspend_state_t state)
{
	/* Device-resume callbacks must never consume completion from an old cycle. */
	WRITE_ONCE(system_sleep_completed, false);
	suspend_entered = false;
	early_wakeup = true;
	pm_set_suspend_via_firmware();

	return 0;
}

static int exynos_pm_valid(suspend_state_t state)
{
	/*
	 * Vendor system sleep temporarily enables TOP user muxes and CMU
	 * Q-channels, then disables USE_L2QACTIVE in both CPU clusters.  There is
	 * no complete CCF/domain-owner API for that choreography yet.  Refuse
	 * mem-suspend instead of adding a second raw CMU writer.
	 */
	return 0;
}

static const struct platform_suspend_ops exynos_pm_ops __maybe_unused = {
	.begin	= exynos_pm_begin,
	.enter	= exynos_pm_enter,
	.valid	= suspend_valid_only_mem,
};

static const struct platform_suspend_ops exynos_pm_blocked_ops = {
	.valid	= exynos_pm_valid,
};

/* ------------------------------------------------------------------ */

static int exynos8890_pm_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	if (pdev->dev.parent && pdev->dev.parent->of_node)
		pmu_regmap = syscon_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR_OR_NULL(pmu_regmap))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "failed to get PMU regmap\n");

	pmu_mngs_regmap =
		syscon_regmap_lookup_by_phandle(np, "samsung,pmu-mngs");
	if (IS_ERR(pmu_mngs_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(pmu_mngs_regmap),
				     "failed to get MNGS PMU regmap\n");

	if (!psci_ops.cpu_suspend)
		return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
				     "PSCI CPU_SUSPEND is unavailable\n");

	eint_base = ioremap(EXYNOS8890_PA_GPIO_ALIVE, SZ_8K);
	if (!eint_base) {
		dev_err(&pdev->dev, "failed to map GPIO_ALIVE\n");
		return -ENOMEM;
	}

	parse_wakeup_masks(np);
	parse_idle_ip_masks(np);
	regmap_update_bits(pmu_mngs_regmap, EXYNOS8890_PMU_MNGS_LPI_MASK,
			   EXYNOS8890_MNGS_LPI_AUD |
			   EXYNOS8890_MNGS_LPI_CAM1,
			   EXYNOS8890_MNGS_LPI_AUD |
			   EXYNOS8890_MNGS_LPI_CAM1);

	suspend_set_ops(&exynos_pm_blocked_ops);
	/*
	 * Do not register the SYS_SLEEP syscore sequence while mem-suspend is
	 * blocked.  Syscore hooks also run in other sleep flows, where applying
	 * the Exynos8890 system-sleep PMU table would be incorrect.
	 */

	dev_warn(&pdev->dev,
		 "mem-suspend disabled until CCF/domain owners provide the vendor pre-sleep choreography\n");
	dev_info(&pdev->dev, "Exynos8890 PM coordination registered\n");
	return 0;
}

static const struct of_device_id exynos8890_pm_of_match[] = {
	{ .compatible = "samsung,exynos8890-pm" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_pm_of_match);

static struct platform_driver exynos8890_pm_driver = {
	.probe = exynos8890_pm_probe,
	.driver = {
		.name = "exynos8890-pm",
		.of_match_table = exynos8890_pm_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(exynos8890_pm_driver);
