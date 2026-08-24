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
 * power domain. [VENDOR] drivers/soc/samsung/pwrcal/S5E8890/S5E8890-pmusfr.h
 * CORTEXM3_APM_CONFIGURATION/STATUS (PMU_ALIVE_BASE + 0x2500/0x2504).
 */

#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos8890-apm.h>

#define EXYNOS8890_APM_CONFIGURATION	0x2500
#define EXYNOS8890_APM_STATUS		0x2504
#define EXYNOS8890_APM_STATUS_ON	BIT(0)
#define EXYNOS8890_APM_POWER_OFF	0x0
#define EXYNOS8890_APM_TIMEOUT_US	50000

struct exynos8890_apm {
	struct device	*dev;
	struct regmap	*pmu;
	struct mutex	lock;
	bool		claimed;
};

static int exynos8890_apm_power_off(struct exynos8890_apm *apm)
{
	u32 status;
	int ret;

	ret = regmap_write(apm->pmu, EXYNOS8890_APM_CONFIGURATION,
			   EXYNOS8890_APM_POWER_OFF);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(apm->pmu, EXYNOS8890_APM_STATUS,
					status, !(status & EXYNOS8890_APM_STATUS_ON),
					1000, EXYNOS8890_APM_TIMEOUT_US);
}

/*
 * exynos8890_apm_dvfs_claim() - claim (or release) exclusive DVFS ownership.
 *
 * @dev is this driver's own platform device, reached via the "samsung,apm"
 * phandle a DVFS consumer (exynos8890-cpufreq) resolves and binds to before
 * calling in. Only claim=true does anything: it forces the M3 into (and
 * confirms) the powered-off state so it can never contest Linux's clock/
 * regulator writes. Releasing does not power it back on - nothing in this
 * port would ever load firmware for it, so leaving it off is strictly
 * safer than waking a core with nothing to run.
 */
int exynos8890_apm_dvfs_claim(struct device *dev, bool claim)
{
	struct exynos8890_apm *apm = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&apm->lock);
	if (claim && !apm->claimed) {
		ret = exynos8890_apm_power_off(apm);
		if (!ret)
			apm->claimed = true;
	} else if (!claim) {
		apm->claimed = false;
	}
	mutex_unlock(&apm->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_apm_dvfs_claim);

static int exynos8890_apm_probe(struct platform_device *pdev)
{
	struct exynos8890_apm *apm;

	apm = devm_kzalloc(&pdev->dev, sizeof(*apm), GFP_KERNEL);
	if (!apm)
		return -ENOMEM;

	apm->dev = &pdev->dev;
	mutex_init(&apm->lock);
	apm->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "samsung,pmu");
	if (IS_ERR(apm->pmu))
		return dev_err_probe(&pdev->dev, PTR_ERR(apm->pmu),
				     "failed to get PMU syscon\n");

	platform_set_drvdata(pdev, apm);
	return 0;
}

static const struct of_device_id exynos8890_apm_of_match[] = {
	{ .compatible = "samsung,exynos8890-apm" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_apm_of_match);

static struct platform_driver exynos8890_apm_driver = {
	.probe = exynos8890_apm_probe,
	.driver = {
		.name = "exynos8890-apm",
		.of_match_table = exynos8890_apm_of_match,
	},
};
module_platform_driver(exynos8890_apm_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 APM DVFS ownership provider");
MODULE_LICENSE("GPL");
