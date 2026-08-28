/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Validity/Synaptics VFS7XXX fingerprint sensor - userspace ioctl ABI.
 *
 * Trimmed from vendor's vfs7xxx.h to the non-secure command set: this port
 * never defines ENABLE_SENSORS_FPRINT_SECURE (see vfs7xxx.c banner comment),
 * so the TEE-routed commands vendor gates behind it are dropped entirely
 * rather than kept as dead ifdef branches.
 */

#ifndef VFS7XXX_H_
#define VFS7XXX_H_

#include <linux/types.h>

#define VFSSPI_IOCTL_MAGIC	'k'

/* Transmit data to the device and retrieve data from it simultaneously. */
#define VFSSPI_IOCTL_RW_SPI_MESSAGE	_IOWR(VFSSPI_IOCTL_MAGIC, 1, unsigned int)
/* Hard reset the device. */
#define VFSSPI_IOCTL_DEVICE_RESET	_IO(VFSSPI_IOCTL_MAGIC, 2)
/* Set the baud rate of SPI master clock. */
#define VFSSPI_IOCTL_SET_CLK		_IOW(VFSSPI_IOCTL_MAGIC, 3, unsigned int)
/* Get level state of DRDY GPIO. */
#define VFSSPI_IOCTL_CHECK_DRDY		_IO(VFSSPI_IOCTL_MAGIC, 4)
/* Register DRDY signal - SPI driver signals the host when DRDY asserts. */
#define VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL \
	_IOW(VFSSPI_IOCTL_MAGIC, 5, unsigned int)
/* Enable/disable DRDY interrupt handling in the SPI driver. */
#define VFSSPI_IOCTL_SET_DRDY_INT	_IOW(VFSSPI_IOCTL_MAGIC, 8, unsigned int)
/* Put device in suspend state. */
#define VFSSPI_IOCTL_DEVICE_SUSPEND	_IO(VFSSPI_IOCTL_MAGIC, 9)
/* Turn on/off the power to the sensor. */
#define VFSSPI_IOCTL_POWER_ON		_IO(VFSSPI_IOCTL_MAGIC, 13)
#define VFSSPI_IOCTL_POWER_OFF		_IO(VFSSPI_IOCTL_MAGIC, 14)
/* Get sensor orientation. */
#define VFSSPI_IOCTL_GET_SENSOR_ORIENT	_IOR(VFSSPI_IOCTL_MAGIC, 18, unsigned int)

/*
 * Used by VFSSPI_IOCTL_RW_SPI_MESSAGE.
 * @rx_buffer: pointer to retrieved data
 * @tx_buffer: pointer to transmitted data
 * @len: transmitted/retrieved data size
 */
struct vfsspi_ioctl_transfer {
	unsigned char *rx_buffer;
	unsigned char *tx_buffer;
	unsigned int len;
};

/*
 * Used by VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL.
 * @user_pid: process ID the driver sends the DRDY signal to
 * @signal_id: signal number to use
 */
struct vfsspi_ioctl_register_signal {
	int user_pid;
	int signal_id;
};

#endif /* VFS7XXX_H_ */
