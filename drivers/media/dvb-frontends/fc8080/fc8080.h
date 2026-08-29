/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FCI FC8080 T-DMB tuner + baseband: definitions shared by the chip code.
 *
 * The chip driver is self-contained apart from tdmb_store_data(), which is
 * how a demodulated MSC stream leaves it. That is the seam a DVB bridge
 * attaches to.
 */

#ifndef __FC8080_H__
#define __FC8080_H__

#include <linux/kernel.h>
#include <linux/types.h>

#define DPRINTK(fmt, ...)	pr_debug("fc8080: " fmt, ##__VA_ARGS__)

bool tdmb_store_data(unsigned char *data, unsigned long len);

#endif /* __FC8080_H__ */
