// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM4773 GNSS/SSP SPI transport
 *
 * The BCM4773 used by Exynos8890 Galaxy S7 devices multiplexes GNSS and the
 * Samsung Sensor Platform over Broadcom's BBD protocol on top of a small SSI
 * framing layer.  This first-stage driver deliberately stops at the transport
 * boundary: it exposes the raw SSI/BBD byte stream through the Linux GNSS core
 * as /dev/gnssX.  BBD demultiplexing, sensor RPC handling and firmware patch
 * loading belong in follow-up work.
 *
 * The SSI transaction format and GPIO handshake are based on Samsung's
 * GPL-licensed bcm_gps_spi driver for BCM4773.
 */

#include <linux/delay.h>
#include <linux/gnss.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#define BCM4773_SSI_READ_HD		0x20
#define BCM4773_SSI_WRITE_HD		0x00

/*
 * Samsung limited PIO reads to 254 payload bytes so status + length + payload
 * stays at 256 bytes and avoids the old driver's DMA corner case.
 */
#define BCM4773_MAX_PAYLOAD		254
#define BCM4773_HELLO_RETRIES		100
#define BCM4773_MAX_DRAIN_FRAMES	128

struct bcm4773 {
	struct spi_device *spi;
	struct gnss_device *gdev;

	struct gpio_desc *enable;
	struct gpio_desc *host_req;
	struct gpio_desc *mcu_req;
	struct gpio_desc *mcu_resp;

	struct mutex io_lock;
	int irq;
	bool irq_enabled;
};

static int bcm4773_spi_xfer(struct bcm4773 *bcm, const void *tx, void *rx,
			    size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.bits_per_word = 8,
	};

	return spi_sync_transfer(bcm->spi, &xfer, 1);
}

static int bcm4773_hello(struct bcm4773 *bcm)
{
	unsigned int count;
	unsigned int retries = 0;

	gpiod_set_value_cansleep(bcm->mcu_req, 1);

	for (count = 0; count < BCM4773_HELLO_RETRIES; count++) {
		if (gpiod_get_value_cansleep(bcm->mcu_resp))
			return 0;

		usleep_range(1000, 1500);

		/* Match the stock driver's three wake-request retries. */
		if (count && !(count % 20) && retries++ < 3) {
			gpiod_set_value_cansleep(bcm->mcu_req, 0);
			usleep_range(1000, 1500);
			gpiod_set_value_cansleep(bcm->mcu_req, 1);
			usleep_range(1000, 1500);
		}
	}

	gpiod_set_value_cansleep(bcm->mcu_req, 0);
	return -ETIMEDOUT;
}

static void bcm4773_bye(struct bcm4773 *bcm)
{
	gpiod_set_value_cansleep(bcm->mcu_req, 0);
}

static int bcm4773_ssi_read(struct bcm4773 *bcm, u8 *payload, size_t *len)
{
	u8 tx[BCM4773_MAX_PAYLOAD + 2] = { 0 };
	u8 rx[BCM4773_MAX_PAYLOAD + 2] = { 0 };
	size_t count;
	int ret;

	tx[0] = BCM4773_SSI_READ_HD;

	/* First transaction returns SSI status and pending payload length. */
	ret = bcm4773_spi_xfer(bcm, tx, rx, 2);
	if (ret)
		return ret;

	if (rx[0])
		return -EIO;

	count = rx[1] ? rx[1] : BCM4773_MAX_PAYLOAD;
	count = min_t(size_t, count, BCM4773_MAX_PAYLOAD);

	memset(tx, 0, count + 2);
	memset(rx, 0, count + 2);
	tx[0] = BCM4773_SSI_READ_HD;

	/* Second transaction returns status, length and the payload itself. */
	ret = bcm4773_spi_xfer(bcm, tx, rx, count + 2);
	if (ret)
		return ret;

	if (rx[0])
		return -EIO;

	if (rx[1] < count)
		count = rx[1];

	memcpy(payload, &rx[2], count);
	*len = count;

	return 0;
}

static int bcm4773_ssi_write(struct bcm4773 *bcm, const u8 *payload,
			     size_t len)
{
	u8 tx[BCM4773_MAX_PAYLOAD + 1] = { 0 };
	u8 rx[BCM4773_MAX_PAYLOAD + 1] = { 0 };

	if (len > BCM4773_MAX_PAYLOAD)
		return -EMSGSIZE;

	tx[0] = BCM4773_SSI_WRITE_HD;
	memcpy(&tx[1], payload, len);

	return bcm4773_spi_xfer(bcm, tx, rx, len + 1);
}

static int bcm4773_drain_locked(struct bcm4773 *bcm)
{
	unsigned int frames;
	int ret = 0;

	for (frames = 0; frames < BCM4773_MAX_DRAIN_FRAMES; frames++) {
		u8 payload[BCM4773_MAX_PAYLOAD];
		size_t len;
		int inserted;

		if (!gpiod_get_value_cansleep(bcm->host_req))
			break;

		ret = bcm4773_ssi_read(bcm, payload, &len);
		if (ret)
			break;

		if (!len)
			continue;

		/*
		 * This is intentionally the raw BBD stream for now.  It can
		 * contain both GNSS traffic and SSP sensor RPC packets.
		 */
		inserted = gnss_insert_raw(bcm->gdev, payload, len);
		if (inserted != len)
			dev_warn_ratelimited(&bcm->spi->dev,
					     "GNSS FIFO overflow: kept %d/%zu bytes\n",
					     inserted, len);
	}

	if (frames == BCM4773_MAX_DRAIN_FRAMES &&
	    gpiod_get_value_cansleep(bcm->host_req))
		dev_warn_ratelimited(&bcm->spi->dev,
				     "HOST_REQ stayed asserted after %u frames\n",
				     frames);

	return ret;
}

static irqreturn_t bcm4773_irq_thread(int irq, void *data)
{
	struct bcm4773 *bcm = data;
	int ret;

	mutex_lock(&bcm->io_lock);

	ret = bcm4773_hello(bcm);
	if (!ret)
		ret = bcm4773_drain_locked(bcm);

	bcm4773_bye(bcm);
	mutex_unlock(&bcm->io_lock);

	if (ret)
		dev_err_ratelimited(&bcm->spi->dev,
				    "receive transaction failed: %d\n", ret);

	return IRQ_HANDLED;
}

static int bcm4773_gnss_open(struct gnss_device *gdev)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);

	mutex_lock(&bcm->io_lock);

	gpiod_set_value_cansleep(bcm->enable, 1);

	bcm->irq_enabled = true;
	enable_irq(bcm->irq);

	mutex_unlock(&bcm->io_lock);

	return 0;
}

static void bcm4773_gnss_close(struct gnss_device *gdev)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);

	if (bcm->irq_enabled) {
		bcm->irq_enabled = false;
		disable_irq(bcm->irq);
	}

	mutex_lock(&bcm->io_lock);
	bcm4773_bye(bcm);
	gpiod_set_value_cansleep(bcm->enable, 0);
	mutex_unlock(&bcm->io_lock);
}

static int bcm4773_gnss_write_raw(struct gnss_device *gdev,
				  const unsigned char *buf, size_t count)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);
	size_t len = min_t(size_t, count, BCM4773_MAX_PAYLOAD);
	int ret;

	mutex_lock(&bcm->io_lock);

	ret = bcm4773_hello(bcm);
	if (ret)
		goto out;

	/* Stock transport services pending RX before sending new data. */
	ret = bcm4773_drain_locked(bcm);
	if (ret)
		goto out_bye;

	ret = bcm4773_ssi_write(bcm, buf, len);
	if (!ret)
		ret = len;

out_bye:
	bcm4773_bye(bcm);
out:
	mutex_unlock(&bcm->io_lock);

	return ret;
}

static const struct gnss_operations bcm4773_gnss_ops = {
	.open = bcm4773_gnss_open,
	.close = bcm4773_gnss_close,
	.write_raw = bcm4773_gnss_write_raw,
};

static int bcm4773_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct bcm4773 *bcm;
	struct gnss_device *gdev;
	int ret;

	bcm = devm_kzalloc(dev, sizeof(*bcm), GFP_KERNEL);
	if (!bcm)
		return -ENOMEM;

	bcm->spi = spi;
	mutex_init(&bcm->io_lock);

	bcm->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(bcm->enable))
		return dev_err_probe(dev, PTR_ERR(bcm->enable),
				     "failed to get GPS enable GPIO\n");

	bcm->host_req = devm_gpiod_get(dev, "host-request", GPIOD_IN);
	if (IS_ERR(bcm->host_req))
		return dev_err_probe(dev, PTR_ERR(bcm->host_req),
				     "failed to get HOST_REQ GPIO\n");

	bcm->mcu_req = devm_gpiod_get(dev, "mcu-request", GPIOD_OUT_LOW);
	if (IS_ERR(bcm->mcu_req))
		return dev_err_probe(dev, PTR_ERR(bcm->mcu_req),
				     "failed to get MCU_REQ GPIO\n");

	bcm->mcu_resp = devm_gpiod_get(dev, "mcu-response", GPIOD_IN);
	if (IS_ERR(bcm->mcu_resp))
		return dev_err_probe(dev, PTR_ERR(bcm->mcu_resp),
				     "failed to get MCU_RESP GPIO\n");

	bcm->irq = gpiod_to_irq(bcm->host_req);
	if (bcm->irq < 0)
		return dev_err_probe(dev, bcm->irq,
				     "failed to map HOST_REQ IRQ\n");

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "failed to setup SPI\n");

	ret = devm_request_threaded_irq(dev, bcm->irq, NULL,
					bcm4773_irq_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), bcm);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request HOST_REQ IRQ\n");

	/* Keep the transport quiescent until userspace opens /dev/gnssX. */
	bcm->irq_enabled = false;

	gdev = gnss_allocate_device(dev);
	if (!gdev)
		return -ENOMEM;

	bcm->gdev = gdev;
	gdev->ops = &bcm4773_gnss_ops;
	gdev->type = GNSS_TYPE_BCM4773;
	gnss_set_drvdata(gdev, bcm);
	spi_set_drvdata(spi, bcm);

	ret = gnss_register_device(gdev);
	if (ret) {
		gnss_put_device(gdev);
		return ret;
	}

	dev_info(dev, "BCM4773 raw BBD transport registered as %s\n",
		 dev_name(&gdev->dev));

	return 0;
}

static void bcm4773_remove(struct spi_device *spi)
{
	struct bcm4773 *bcm = spi_get_drvdata(spi);

	gnss_deregister_device(bcm->gdev);
	gnss_put_device(bcm->gdev);
}

static const struct of_device_id bcm4773_of_match[] = {
	{ .compatible = "brcm,bcm4773" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm4773_of_match);

static struct spi_driver bcm4773_driver = {
	.probe = bcm4773_probe,
	.remove = bcm4773_remove,
	.driver = {
		.name = "gnss-bcm4773",
		.of_match_table = bcm4773_of_match,
	},
};
module_spi_driver(bcm4773_driver);

MODULE_DESCRIPTION("Broadcom BCM4773 GNSS/SSP SPI transport");
MODULE_LICENSE("GPL");
