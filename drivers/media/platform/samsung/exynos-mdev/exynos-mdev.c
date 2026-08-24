// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos media device driver, ported from vendor.
 *
 * decon8890's decon_probe() looks this up by driver name
 * (module_name_to_driver_data(MDEV_MODULE_NAME), see <media/exynos_mc.h>)
 * with no deferred-probe fallback, so this must already be bound before
 * decon8890 probes - the only thing it does is register the shared
 * v4l2_device/media_device that VPP and DECON's subdevs attach to.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/exynos_mc.h>

static int mdev_probe(struct platform_device *pdev)
{
	struct v4l2_device *v4l2_dev;
	struct device *dev = &pdev->dev;
	struct exynos_md *mdev;
	int ret;

	mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;

	if (dev->of_node) {
		ret = of_alias_get_id(dev->of_node, "mdev");
		if (ret < 0)
			ret = 0;
		mdev->id = ret;
		pdev->id = mdev->id;
	} else {
		mdev->id = pdev->id;
	}

	mdev->pdev = pdev;
	spin_lock_init(&mdev->slock);

	snprintf(mdev->media_dev.model, sizeof(mdev->media_dev.model), "%s%d",
		 dev_name(dev), mdev->id);
	mdev->media_dev.dev = dev;

	v4l2_dev = &mdev->v4l2_dev;
	v4l2_dev->mdev = &mdev->media_dev;
	snprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s", dev_name(dev));

	ret = v4l2_device_register(dev, v4l2_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register v4l2_device: %d\n", ret);
		goto err_v4l2_reg;
	}

	ret = media_device_register(&mdev->media_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register media device: %d\n", ret);
		goto err_mdev_reg;
	}

	platform_set_drvdata(pdev, mdev);
	dev_info(dev, "media%d[%p] registered successfully\n", mdev->id, mdev);
	return 0;

err_mdev_reg:
	v4l2_device_unregister(v4l2_dev);
err_v4l2_reg:
	kfree(mdev);
	return ret;
}

static void mdev_remove(struct platform_device *pdev)
{
	struct exynos_md *mdev = platform_get_drvdata(pdev);

	if (!mdev)
		return;
	media_device_unregister(&mdev->media_dev);
	v4l2_device_unregister(&mdev->v4l2_dev);
	kfree(mdev);
}

static const struct of_device_id exynos_mdev_match[] = {
	{ .compatible = "samsung,exynos5-mdev" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_mdev_match);

static struct platform_driver mdev_driver = {
	.probe	= mdev_probe,
	.remove	= mdev_remove,
	.driver	= {
		.name		= MDEV_MODULE_NAME,
		.of_match_table	= exynos_mdev_match,
	},
};
module_platform_driver(mdev_driver);

MODULE_AUTHOR("Hyunwoong Kim <khw0178.kim@samsung.com>");
MODULE_DESCRIPTION("Exynos media device driver");
MODULE_LICENSE("GPL");
