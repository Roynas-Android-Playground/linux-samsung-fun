/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _EXYNOS8890_VPP_H_
#define _EXYNOS8890_VPP_H_

#include <linux/types.h>

struct device;

/* IDMA channels wired to DECON-F on the DISP0 domain */
#define VPP8890_CH_G0		0
#define VPP8890_CH_G1		1
#define VPP8890_CH_VG0		2
#define VPP8890_CH_VG1		3
#define VPP8890_CH_NR		4

struct exynos8890_vpp_cfg {
	dma_addr_t dma_addr;
	u32 fourcc;
	u32 src_x, src_y;
	u32 src_w, src_h;
	u32 fb_w, fb_h;
};

struct device *exynos8890_vpp_get(struct device_node *np);
int exynos8890_vpp_setup(struct device *dev, unsigned int ch,
			 const struct exynos8890_vpp_cfg *cfg);
void exynos8890_vpp_disable(struct device *dev, unsigned int ch);
int exynos8890_vpp_resume(struct device *dev);
void exynos8890_vpp_suspend(struct device *dev);

/* DECON-F WIN_CONTROL.CHMAP encoding is not the IDMA channel index */
u32 exynos8890_vpp_chmap(unsigned int ch);

#endif /* _EXYNOS8890_VPP_H_ */
