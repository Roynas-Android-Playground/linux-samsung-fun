/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Exported API for consumers of the shared BCM4773 SPI transport
 * (drivers/gnss/bcm4773.c).
 *
 * BCM4773 multiplexes GNSS traffic and Samsung SSP sensor-hub RPC traffic
 * over one physical SPI link; the GNSS driver is the sole owner of that
 * link and hands decoded sensor-hub bytes to at most one registered
 * consumer, and accepts outbound bytes from it. See
 * Documentation/driver-api/iio/exynos8890-sensorhub.rst for the full
 * boundary rationale.
 */
#ifndef _LINUX_GNSS_BCM4773_H
#define _LINUX_GNSS_BCM4773_H

#include <linux/err.h>
#include <linux/types.h>

struct bcm4773;
struct device;

/**
 * struct bcm4773_sensor_ops - callbacks for a sensor-hub protocol consumer
 * @recv: called with one decoded, length-validated SSP payload every time
 *        an IRpcSensorResponse_Data RPC record arrives. Invoked from
 *        IRQ-thread context with the transport's internal lock held: it
 *        must not block and must not call bcm4773_sensor_send() or any
 *        other bcm4773_*() entry point re-entrantly.
 */
struct bcm4773_sensor_ops {
	void (*recv)(void *priv, const u8 *data, size_t len);
};

#if IS_REACHABLE(CONFIG_GNSS_BCM4773)

struct bcm4773 *bcm4773_get(struct device *consumer);
void bcm4773_put(struct bcm4773 *bcm);

int bcm4773_register_sensor_ops(struct bcm4773 *bcm,
				const struct bcm4773_sensor_ops *ops,
				void *priv);
void bcm4773_unregister_sensor_ops(struct bcm4773 *bcm);

int bcm4773_sensor_send(struct bcm4773 *bcm, const void *data, size_t len);

#else

static inline struct bcm4773 *bcm4773_get(struct device *consumer)
{
	return ERR_PTR(-ENODEV);
}

static inline void bcm4773_put(struct bcm4773 *bcm) { }

static inline int bcm4773_register_sensor_ops(struct bcm4773 *bcm,
					const struct bcm4773_sensor_ops *ops,
					void *priv)
{
	return -ENODEV;
}

static inline void bcm4773_unregister_sensor_ops(struct bcm4773 *bcm) { }

static inline int bcm4773_sensor_send(struct bcm4773 *bcm, const void *data,
				      size_t len)
{
	return -ENODEV;
}

#endif /* IS_REACHABLE(CONFIG_GNSS_BCM4773) */

#endif /* _LINUX_GNSS_BCM4773_H */
