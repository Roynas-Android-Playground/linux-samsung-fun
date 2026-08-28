// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 APM (Cortex-M3) DVFS ownership provider.
 *
 * The real APM subsystem is a Cortex-M3 running closed-loop DVFS firmware,
 * fed over a mailbox (see vendor drivers/mailbox/samsung/apm-exynos8890.c and
 * the apm_8890_*.h firmware blobs) - none of which this port loads or speaks
 * to. exynos8890-cpufreq.c already refuses to probe unless PMU
 * CORTEXM3_APM_STATUS shows the M3 is off, so the only thing this driver
 * needs to guarantee is that it *stays* off for as long as Linux is doing
 * DVFS itself: hold it in the standard Exynos PMU LOCAL_PWR_CFG/STATUS
 * power-down handshake, the same pattern used for every other Exynos PMU
 * power domain. The vendor PMU description locates the APM configuration and
 * status registers at PMU_ALIVE offsets 0x2500 and 0x2504.
 */

#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos8890-apm.h>

#define EXYNOS8890_APM_CONFIGURATION	0x2500
#define EXYNOS8890_APM_STATUS		0x2504
#define EXYNOS8890_APM_RUN		BIT(0)
#define EXYNOS8890_APM_TIMEOUT_US	50000

struct exynos8890_apm {
	struct device	*dev;
	struct regmap	*pmu;
	struct mutex	lock;
	bool		claimed;
};

static struct exynos8890_apm *exynos8890_apm_owner;

static int exynos8890_apm_power_off(struct exynos8890_apm *apm)
{
	u32 status;
	int ret;

	ret = regmap_update_bits(apm->pmu, EXYNOS8890_APM_CONFIGURATION,
				 EXYNOS8890_APM_RUN, 0);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(apm->pmu, EXYNOS8890_APM_STATUS,
					status,
					!(status & EXYNOS8890_APM_RUN),
					1000, EXYNOS8890_APM_TIMEOUT_US);
}

/*
 * exynos8890_apm_dvfs_claim() - confirm exclusive Linux DVFS ownership.
 *
 * @dev is this driver's own platform device, reached via the "samsung,apm"
 * phandle a DVFS consumer (exynos8890-cpufreq) resolves and binds to before
 * calling in. claim=true forces the M3 into (and confirms) the powered-off
 * state so it can never contest Linux's clock/regulator writes. claim=false
 * is deliberately a no-op: nothing in this port can reload APM firmware, so
 * leaving the core off is the only safe release behavior.
 */
int exynos8890_apm_dvfs_claim(struct device *dev, bool claim)
{
	struct exynos8890_apm *apm = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&apm->lock);
	if (claim) {
		ret = exynos8890_apm_power_off(apm);
		if (!ret)
			apm->claimed = true;
	}
	mutex_unlock(&apm->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_apm_dvfs_claim);

bool exynos8890_apm_dvfs_ready(void)
{
	struct exynos8890_apm *apm = READ_ONCE(exynos8890_apm_owner);
	u32 status;
	bool ready;

	if (!apm)
		return false;
	mutex_lock(&apm->lock);
	ready = apm->claimed &&
		!regmap_read(apm->pmu, EXYNOS8890_APM_STATUS, &status) &&
		!(status & EXYNOS8890_APM_RUN);
	mutex_unlock(&apm->lock);
	return ready;
}
EXPORT_SYMBOL_GPL(exynos8890_apm_dvfs_ready);

static int exynos8890_apm_probe(struct platform_device *pdev)
{
	struct exynos8890_apm *apm;
	int ret;

	apm = devm_kzalloc(&pdev->dev, sizeof(*apm), GFP_KERNEL);
	if (!apm)
		return -ENOMEM;

	apm->dev = &pdev->dev;
	mutex_init(&apm->lock);
	if (pdev->dev.parent && pdev->dev.parent->of_node)
		apm->pmu = syscon_node_to_regmap(pdev->dev.parent->of_node);
	else
		apm->pmu = ERR_PTR(-ENODEV);
	if (IS_ERR(apm->pmu))
		apm->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							   "samsung,pmu");
	if (IS_ERR(apm->pmu))
		return dev_err_probe(&pdev->dev, PTR_ERR(apm->pmu),
				     "failed to get PMU syscon\n");

	platform_set_drvdata(pdev, apm);
	ret = exynos8890_apm_power_off(apm);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to claim DVFS ownership\n");
	apm->claimed = true;
	/* Publish only after PMU status proves the firmware core is off. */
	WRITE_ONCE(exynos8890_apm_owner, apm);
	return 0;
}

static const struct of_device_id exynos8890_apm_of_match[] = {
	{ .compatible = "samsung,exynos8890-apm" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_apm_of_match);

static int exynos8890_apm_resume(struct device *dev)
{
	return exynos8890_apm_dvfs_claim(dev, true);
}

static const struct dev_pm_ops exynos8890_apm_pm_ops = {
	.resume = exynos8890_apm_resume,
	.restore = exynos8890_apm_resume,
};

static struct platform_driver exynos8890_apm_driver = {
	.probe = exynos8890_apm_probe,
	.driver = {
		.name = "exynos8890-apm",
		.of_match_table = exynos8890_apm_of_match,
		.pm = pm_ptr(&exynos8890_apm_pm_ops),
		.suppress_bind_attrs = true,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
	},
};

static int __init exynos8890_apm_init(void)
{
	return platform_driver_register(&exynos8890_apm_driver);
}
postcore_initcall(exynos8890_apm_init);

MODULE_DESCRIPTION("Samsung Exynos8890 APM DVFS ownership provider");
MODULE_LICENSE("GPL");
