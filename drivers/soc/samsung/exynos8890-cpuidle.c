// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 local-core C2 diagnostics
 *
 * This deliberately implements only affinity-level-0 CPU power-down. Cluster
 * power-down and system-idle coordination remain unsupported.
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/context_tracking.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/smp.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/smp_plat.h>

#define EXYNOS8890_PMU_CPU_CONFIG_BASE	0x2000
#define EXYNOS8890_PMU_CPU_STRIDE	0x80
#define EXYNOS8890_CPU_LOCAL_PWR_CFG	GENMASK(3, 0)
#define EXYNOS8890_C2_STATE		0x00010000

struct exynos8890_c2_stats {
	u64 attempts;
	u64 before_psci;
	u64 after_psci;
	u64 psci_fail;
	u64 rollback_fail;
	u64 wake_pmu_bad;
	u32 last_before;
	u32 last_after;
	int last_psci_ret;
};

struct exynos8890_cpuidle {
	void __iomem *pmu;
	struct cpuidle_driver driver;
	atomic_t faulted;
};

static DEFINE_PER_CPU(struct exynos8890_c2_stats, exynos8890_c2_stats);
static bool exynos8890_c2_opt_in;

static int __init exynos8890_c2_setup(char *str)
{
	return kstrtobool(str, &exynos8890_c2_opt_in);
}
early_param("exynos8890.c2", exynos8890_c2_setup);

static unsigned int exynos8890_cpu_config_reg(unsigned int cpu)
{
	u64 mpidr = cpu_logical_map(cpu);
	u32 index;

	index = (MPIDR_AFFINITY_LEVEL(mpidr, 1) << 2) |
		MPIDR_AFFINITY_LEVEL(mpidr, 0);
	return EXYNOS8890_PMU_CPU_CONFIG_BASE +
		index * EXYNOS8890_PMU_CPU_STRIDE;
}

static int __cpuidle exynos8890_enter_wfi(struct cpuidle_device *dev,
					 struct cpuidle_driver *drv, int index)
{
	cpu_do_idle();
	return index;
}

static int __cpuidle exynos8890_enter_wfi_rcu(void)
{
	ct_cpuidle_enter();
	cpu_do_idle();
	ct_cpuidle_exit();
	return 0;
}

static int __cpuidle exynos8890_enter_c2(struct cpuidle_device *dev,
					struct cpuidle_driver *drv, int index)
{
	struct exynos8890_cpuidle *idle = container_of(drv,
							struct exynos8890_cpuidle,
							driver);
	struct exynos8890_c2_stats *stats = this_cpu_ptr(&exynos8890_c2_stats);
	unsigned int cpu = smp_processor_id();
	void __iomem *config;
	u32 before, after;
	int ret;

	if (unlikely(atomic_read(&idle->faulted) || cpu != dev->cpu))
		return exynos8890_enter_wfi_rcu();

	stats->attempts++;
	ret = cpu_pm_enter();
	if (ret)
		return -1;

	config = idle->pmu + exynos8890_cpu_config_reg(cpu);
	before = readl_relaxed(config);
	stats->last_before = before;
	if (unlikely((before & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		     EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		atomic_set(&idle->faulted, 1);
		ret = -EIO;
		goto out_pm;
	}

	writel_relaxed(before & ~EXYNOS8890_CPU_LOCAL_PWR_CFG, config);
	after = readl_relaxed(config);
	if (unlikely(after & EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		writel_relaxed(before, config);
		dsb(sy);
		atomic_set(&idle->faulted, 1);
		ret = -EIO;
		goto out_pm;
	}

	stats->before_psci++;
	dsb(sy);
	ret = psci_cpu_suspend_enter(EXYNOS8890_C2_STATE);
	stats->last_psci_ret = ret;
	stats->after_psci++;

	after = readl_relaxed(config);
	stats->last_after = after;
	if (ret) {
		stats->psci_fail++;
		writel_relaxed(before, config);
		dsb(sy);
		if ((readl_relaxed(config) & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		    EXYNOS8890_CPU_LOCAL_PWR_CFG) {
			stats->rollback_fail++;
			atomic_set(&idle->faulted, 1);
		}
	} else if (unlikely((after & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
			    EXYNOS8890_CPU_LOCAL_PWR_CFG)) {
		/* Never rewrite CPU CONFIG after a successful firmware wake. */
		stats->wake_pmu_bad++;
		atomic_set(&idle->faulted, 1);
	}

out_pm:
	cpu_pm_exit();
	return ret ? -1 : index;
}

static int exynos8890_cpuidle_probe(struct platform_device *pdev)
{
	struct device_node *pmu_np;
	struct exynos8890_cpuidle *idle;
	u32 state = 0;
	int cpu, ret;

	if (!exynos8890_c2_opt_in) {
		dev_info(&pdev->dev, "C2 not opted in; keeping WFI-only policy\n");
		return 0;
	}

	ret = of_property_read_u32(pdev->dev.of_node,
				   "samsung,psci-suspend-param", &state);
	if (ret || state != EXYNOS8890_C2_STATE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid PSCI C2 state %#x\n", state);

	idle = devm_kzalloc(&pdev->dev, sizeof(*idle), GFP_KERNEL);
	if (!idle)
		return -ENOMEM;
	atomic_set(&idle->faulted, 0);

	pmu_np = of_parse_phandle(pdev->dev.of_node, "samsung,pmu-syscon", 0);
	if (!pmu_np)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing PMU phandle\n");
	idle->pmu = of_iomap(pmu_np, 0);
	of_node_put(pmu_np);
	if (!idle->pmu)
		return dev_err_probe(&pdev->dev, -ENOMEM, "failed to map PMU\n");

	for_each_possible_cpu(cpu) {
		u32 value = readl_relaxed(idle->pmu +
					  exynos8890_cpu_config_reg(cpu));

		if ((value & EXYNOS8890_CPU_LOCAL_PWR_CFG) !=
		    EXYNOS8890_CPU_LOCAL_PWR_CFG) {
			dev_err(&pdev->dev,
				"CPU%d starts with invalid PMU config %#x\n",
				cpu, value);
			ret = -EIO;
			goto err_unmap;
		}
	}

	idle->driver.name = "exynos8890-c2-test";
	idle->driver.owner = THIS_MODULE;
	idle->driver.safe_state_index = 0;
	idle->driver.state_count = 2;
	idle->driver.states[0] = (struct cpuidle_state) {
		.name = "WFI",
		.desc = "ARM WFI",
		.exit_latency_ns = 1000,
		.target_residency_ns = 1000,
		.enter = exynos8890_enter_wfi,
	};
	idle->driver.states[1] = (struct cpuidle_state) {
		.name = "C2",
		.desc = "Exynos8890 local CPU C2",
		.exit_latency_ns = 90000,
		.target_residency_ns = 2000000,
		.flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE |
			 CPUIDLE_FLAG_OFF,
		.enter = exynos8890_enter_c2,
	};

	platform_set_drvdata(pdev, idle);
	ret = cpuidle_register(&idle->driver, NULL);
	if (ret)
		goto err_unmap;

	dev_warn(&pdev->dev,
		 "C2 diagnostic registered disabled; enable one CPU state at a time\n");
	return 0;

err_unmap:
	iounmap(idle->pmu);
	return ret;
}

static void exynos8890_cpuidle_remove(struct platform_device *pdev)
{
	struct exynos8890_cpuidle *idle = platform_get_drvdata(pdev);

	if (!idle)
		return;
	cpuidle_unregister(&idle->driver);
	iounmap(idle->pmu);
}

static const struct of_device_id exynos8890_cpuidle_of_match[] = {
	{ .compatible = "samsung,exynos8890-cpuidle-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpuidle_of_match);

static struct platform_driver exynos8890_cpuidle_driver = {
	.probe = exynos8890_cpuidle_probe,
	.remove = exynos8890_cpuidle_remove,
	.driver = {
		.name = "exynos8890-cpuidle-test",
		.of_match_table = exynos8890_cpuidle_of_match,
	},
};
module_platform_driver(exynos8890_cpuidle_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 opt-in local-core C2 diagnostics");
MODULE_LICENSE("GPL");
