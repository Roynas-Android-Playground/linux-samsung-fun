/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_REGULATOR_S2MPS16_H
#define __LINUX_REGULATOR_S2MPS16_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct regulator;

#if IS_REACHABLE(CONFIG_REGULATOR_S2MPS11)
int s2mps16_regulator_set_vth_offset(struct regulator *regulator, bool high);
#else
static inline int
s2mps16_regulator_set_vth_offset(struct regulator *regulator, bool high)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __LINUX_REGULATOR_S2MPS16_H */
