// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 Samsung SSP sensor-hub protocol core.
 *
 * Speaks the Samsung "SSP" application-layer command/response protocol to
 * the sensor-hub MCU reachable through the shared BCM4773 GNSS/sensor SPI
 * transport (drivers/gnss/bcm4773.c). This driver owns no hardware of its
 * own: it only calls bcm4773_sensor_send() and receives bytes via a
 * registered callback, per the boundary in
 * Documentation/driver-api/iio/exynos8890-sensorhub.rst.
 *
 * This first milestone only proves the SSP link is alive end to end (the
 * vendor's own WHOAMI bring-up gate) — no sensor enumeration or IIO channel
 * presentation yet.
 */

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/gnss/bcm4773.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/*
 * SSP command header — [VENDOR] ssp.h struct ssp_msg { u8 cmd; u16 length;
 * u16 options; u32 data; } __packed, dumped to the wire in the AP's native
 * (little-endian) byte order.
 */
#define SSP_MSG_HEADER_SIZE		9

/* [VENDOR] ssp.h SSP_SPI_MASK and the transaction-type values it selects */
#define SSP_SPI_MASK			GENMASK(1, 0)
#define SSP_AP2HUB_READ			0
#define SSP_AP2HUB_WRITE		1
#define SSP_HUB2AP_WRITE		2
#define SSP_AP2HUB_READY		3
#define SSP_AP2HUB_RETURN		4

/* [VENDOR] ssp.h MSG2SSP_AP_* command ids */
#define MSG2SSP_AP_WHOAMI		0x0F
#define MSG2SSP_AP_FIRMWARE_REV	0xF0

/* [VENDOR] ssp.h DEVICE_ID — the only valid WHOAMI response byte */
#define SSP_DEVICE_ID			0x55

#define SSP_TRANSACT_TIMEOUT_MS	1000

struct exynos8890_ssp {
	struct device		*dev;
	struct bcm4773		*bcm;

	struct mutex		lock; /* serializes one transaction at a time */
	struct completion	done;
	u8			*rsp_buf;
	size_t			rsp_cap;
	int			rsp_len;
};

/*
 * exynos8890_ssp_recv() — bcm4773_sensor_ops callback.
 *
 * Called from IRQ-thread context with bcm4773's transport lock held (see
 * struct bcm4773_sensor_ops in <linux/gnss/bcm4773.h>): must not block and
 * must not call back into bcm4773_sensor_send(). Storing the response and
 * completing a waiter satisfies both constraints.
 *
 * Phase 1 only: any bytes received while no transaction is outstanding are
 * dropped. Unsolicited HUB2AP_WRITE reports (sensor data streaming) are a
 * future-phase concern, not handled here.
 */
static void exynos8890_ssp_recv(void *priv, const u8 *data, size_t len)
{
	struct exynos8890_ssp *ssp = priv;

	if (completion_done(&ssp->done))
		return;

	ssp->rsp_len = min(len, ssp->rsp_cap);
	memcpy(ssp->rsp_buf, data, ssp->rsp_len);
	complete(&ssp->done);
}

static const struct bcm4773_sensor_ops exynos8890_ssp_ops = {
	.recv = exynos8890_ssp_recv,
};

/*
 * exynos8890_ssp_transact() — send one SSP command, wait for its response.
 *
 * Only one transaction may be outstanding at a time, serialized by @lock —
 * sufficient for probe-time bring-up and matching the vendor driver's own
 * synchronous ssp_spi_sync() usage during initialize_mcu().
 *
 * Returns the response length on success (which may be less than @rsp_cap
 * if the MCU replied with fewer bytes than requested), or a negative errno.
 */
static int exynos8890_ssp_transact(struct exynos8890_ssp *ssp, u8 cmd,
				   u16 options, u8 *rsp, size_t rsp_cap)
{
	u8 req[SSP_MSG_HEADER_SIZE] = { 0 };
	int ret;

	mutex_lock(&ssp->lock);

	reinit_completion(&ssp->done);
	ssp->rsp_buf = rsp;
	ssp->rsp_cap = rsp_cap;
	ssp->rsp_len = 0;

	req[0] = cmd;
	put_unaligned_le16(rsp_cap, &req[1]);
	put_unaligned_le16(options, &req[3]);
	/* req[5..8] ("data") left zero — unused by the commands sent here */

	ret = bcm4773_sensor_send(ssp->bcm, req, sizeof(req));
	if (ret < 0)
		goto out;

	if (!wait_for_completion_timeout(&ssp->done,
			msecs_to_jiffies(SSP_TRANSACT_TIMEOUT_MS))) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = ssp->rsp_len;

out:
	mutex_unlock(&ssp->lock);

	return ret;
}

static int exynos8890_ssp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos8890_ssp *ssp;
	u8 whoami = 0;
	int ret;

	ssp = devm_kzalloc(dev, sizeof(*ssp), GFP_KERNEL);
	if (!ssp)
		return -ENOMEM;

	ssp->dev = dev;
	mutex_init(&ssp->lock);
	init_completion(&ssp->done);

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

	/*
	 * Phase 2 first-probe milestone (see
	 * Documentation/driver-api/iio/exynos8890-sensorhub.rst): the
	 * vendor's own first bring-up gate, reused verbatim. A mismatch or
	 * timeout is diagnostic, not fatal — the sensor-hub MCU may
	 * legitimately be absent or unpowered on early bring-up boards, and
	 * this driver must never be able to wedge or crash the AP.
	 */
	ret = exynos8890_ssp_transact(ssp, MSG2SSP_AP_WHOAMI, SSP_AP2HUB_READ,
				      &whoami, sizeof(whoami));
	if (ret == sizeof(whoami) && whoami == SSP_DEVICE_ID)
		dev_info(dev, "sensor-hub WHOAMI ok (0x%02x)\n", whoami);
	else if (ret >= 0)
		dev_warn(dev, "sensor-hub WHOAMI mismatch: %d byte(s), 0x%02x\n",
			 ret, whoami);
	else
		dev_warn(dev, "sensor-hub WHOAMI failed: %d\n", ret);

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

MODULE_DESCRIPTION("Exynos8890 Samsung SSP sensor-hub protocol core");
MODULE_LICENSE("GPL");
