// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 Samsung SSP sensor-hub transport registration.
 *
 * The BCM4773 firmware lifecycle is owned by userspace: stock registers the
 * BBD/SSP devices first, then lhd downloads the runtime patch and announces
 * ESW:READY before it asks the MCU for its identity.  Kernel probe must not
 * send WHOAMI or sensor commands to an unpatched MCU.
 */

#include <linux/err.h>
#include <linux/gnss/bcm4773.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct exynos8890_ssp {
	struct device *dev;
	struct bcm4773 *bcm;
};

/*
 * Responses are ignored until a userspace-facing firmware/readiness lifecycle
 * exists.  This callback runs with the BCM4773 transport lock held and must
 * not block or call back into the transport.
 */
static void exynos8890_ssp_recv(void *priv, const u8 *data, size_t len)
{
	struct exynos8890_ssp *ssp = priv;

	dev_dbg(ssp->dev,
		"ignoring %zu sensor byte(s) at %p before userspace readiness\n",
		len, data);
}

static const struct bcm4773_sensor_ops exynos8890_ssp_ops = {
	.recv = exynos8890_ssp_recv,
};

static int exynos8890_ssp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos8890_ssp *ssp;
	int ret;

	ssp = devm_kzalloc(dev, sizeof(*ssp), GFP_KERNEL);
	if (!ssp)
		return -ENOMEM;

	ssp->dev = dev;
	ssp->bcm = bcm4773_get(dev);
	if (IS_ERR(ssp->bcm))
		return dev_err_probe(dev, PTR_ERR(ssp->bcm),
				     "failed to get BCM4773 transport\n");

	ret = bcm4773_register_sensor_ops(ssp->bcm, &exynos8890_ssp_ops, ssp);
	if (ret) {
		bcm4773_put(ssp->bcm);
		return dev_err_probe(dev, ret,
				     "failed to register as sensor RPC consumer\n");
	}

	platform_set_drvdata(pdev, ssp);
	dev_info(dev, "registered; awaiting userspace firmware readiness\n");

	return 0;
}

static void exynos8890_ssp_remove(struct platform_device *pdev)
{
	struct exynos8890_ssp *ssp = platform_get_drvdata(pdev);

	bcm4773_unregister_sensor_ops(ssp->bcm);
	bcm4773_put(ssp->bcm);
}

static const struct of_device_id exynos8890_ssp_of_match[] = {
	{ .compatible = "samsung,exynos8890-ssp" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_ssp_of_match);

static struct platform_driver exynos8890_ssp_driver = {
	.probe = exynos8890_ssp_probe,
	.remove = exynos8890_ssp_remove,
	.driver = {
		.name = "exynos8890-ssp",
		.of_match_table = exynos8890_ssp_of_match,
	},
};
module_platform_driver(exynos8890_ssp_driver);

MODULE_DESCRIPTION("Exynos8890 Samsung SSP sensor-hub transport");
MODULE_LICENSE("GPL");
