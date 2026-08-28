// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native aggregate DVFS clocks for Exynos8890.
 *
 * ECT describes a performance level as an ordered set of PLL, mux, divider
 * and gate values.  Exposing every one of those fields to a policy driver
 * made a complete transition non-atomic and encouraged a second, PWRCAL-owned
 * clock graph.  This file translates the immutable calibration matrix to
 * explicit CCF clock objects once and exposes one writable clock per domain.
 * CCF's prepare lock then covers the complete safe transition.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <dt-bindings/clock/samsung,exynos8890-cmu.h>
#include <linux/soc/samsung/exynos8890-calibration.h>

#include "clk.h"
#include "clk-exynos8890-dvfs.h"

enum exynos8890_dvfs_provider {
	DVFS_PROVIDER_TOP,
	DVFS_PROVIDER_G3D,
	DVFS_PROVIDER_OSC,
};

struct exynos8890_dvfs_clk_ref {
	enum exynos8890_dvfs_provider provider;
	unsigned int id;
};

struct exynos8890_dvfs_member_desc {
	const char *name;
	enum exynos8890_calib_member_type type;
	struct exynos8890_dvfs_clk_ref clock;
	const struct exynos8890_dvfs_clk_ref *parents;
	unsigned int num_parents;
	u64 max_value;
	bool allow_zero;
};

struct exynos8890_dvfs_member {
	enum exynos8890_calib_member_type type;
	struct clk *clk;
	struct clk **parents;
	unsigned int num_parents;
	u64 max_value;
	bool gate_owned;
	bool allow_zero;
};

struct exynos8890_g3d_switch {
	struct clk *source_mux;
	struct clk *source_parent;
	struct clk *source_div;
	struct clk *source_gate;
	struct clk *bus_usermux;
	struct clk *bus_parent;
	struct clk *bus_idle_parent;
	struct clk *main_mux;
	struct clk *main_pll_parent;
	struct clk *main_bus_parent;
	struct clk *pll_usermux;
	struct clk *pll_parent;
	struct clk *core_gate;
	bool gate_owned;
};

struct exynos8890_dvfs_clock {
	struct clk_hw hw;
	struct clk_init_data init;
	const struct exynos8890_calib_domain *calib;
	struct exynos8890_dvfs_member *members;
	struct exynos8890_g3d_switch *g3d;
	unsigned long current_rate;
	bool faulted;
	bool registered;
};

#define to_exynos8890_dvfs(_hw) \
	container_of(_hw, struct exynos8890_dvfs_clock, hw)

static struct clk_hw **top_hws;
static struct clk *top_oscclk;

#define REF_TOP(_id) { .provider = DVFS_PROVIDER_TOP, .id = (_id) }
#define REF_G3D(_id) { .provider = DVFS_PROVIDER_G3D, .id = (_id) }
#define REF_OSC { .provider = DVFS_PROVIDER_OSC }

static const struct exynos8890_dvfs_clk_ref int_bus0123_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS2_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS3_PLL),
};

static const struct exynos8890_dvfs_clk_ref int_bus0123_mfc_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS2_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS3_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_MFC_PLL),
};

static const struct exynos8890_dvfs_clk_ref int_bus0123_isp_mfc_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS2_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS3_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_ISP_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_MFC_PLL),
};

static const struct exynos8890_dvfs_clk_ref int_cam1_arm_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS2_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS3_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_ISP_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_MFC_PLL),
	REF_TOP(CLK_MOUT_TOP_BUS2_PLL),
};

static const struct exynos8890_dvfs_clk_ref int_bus02_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS2_PLL),
};

static const struct exynos8890_dvfs_clk_ref int_bus01_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
};

/* Raw selectors 2 and 3 are both wired to the oscillator. */
static const struct exynos8890_dvfs_clk_ref int_bus01_osc_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS1_PLL),
	REF_OSC,
	REF_OSC,
};

static const struct exynos8890_dvfs_clk_ref int_bus0_osc_parents[] = {
	REF_TOP(CLK_MOUT_TOP_SCLK_BUS0_PLL),
	REF_OSC,
	REF_OSC,
	REF_OSC,
};

#define MEMBER_PLL(_name, _provider, _id) { \
	.name = #_name, \
	.type = EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ, \
	.clock = REF_##_provider(_id), \
	.max_value = ULONG_MAX, \
}

#define MEMBER_PLL_OFF(_name, _provider, _id) { \
	.name = #_name, \
	.type = EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ, \
	.clock = REF_##_provider(_id), \
	.max_value = ULONG_MAX, \
	.allow_zero = true, \
}

#define MEMBER_MUX(_name, _provider, _id, _parents) { \
	.name = #_name, \
	.type = EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR, \
	.clock = REF_##_provider(_id), \
	.parents = (_parents), \
	.num_parents = ARRAY_SIZE(_parents), \
	.max_value = ARRAY_SIZE(_parents) - 1, \
}

#define MEMBER_DIV(_name, _provider, _id) { \
	.name = #_name, \
	.type = EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE, \
	.clock = REF_##_provider(_id), \
	.max_value = U32_MAX, \
}

#define MEMBER_DIV_MAX(_name, _provider, _id, _max) { \
	.name = #_name, \
	.type = EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE, \
	.clock = REF_##_provider(_id), \
	.max_value = (_max), \
}

static const struct exynos8890_dvfs_member_desc int_members[] = {
	/*
	 * Vendor PWRCAL exposes the shared physical MFC PLL through a separate
	 * DVFS policy alias. CCF models that hardware once as fout_mfc_pll;
	 * zero-valued rows turn it off only after every characterized mux has
	 * moved to another source.
	 */
	MEMBER_PLL_OFF(MFC_PLL_DVFS, TOP, CLK_FOUT_MFC_PLL),
	MEMBER_MUX(TOP_MUX_ACLK_BUS0_528, TOP, CLK_MOUT_TOP_ACLK_BUS0_528,
		   int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_BUS1_528, TOP, CLK_MOUT_TOP_ACLK_BUS1_528,
		   int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_BUS0_200, TOP, CLK_MOUT_TOP_ACLK_BUS0_200,
		   int_bus01_osc_parents),
	MEMBER_MUX(TOP_MUX_PCLK_BUS0_132, TOP, CLK_MOUT_TOP_PCLK_BUS0_132,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_PCLK_BUS1_132, TOP, CLK_MOUT_TOP_PCLK_BUS1_132,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_IMEM_266, TOP, CLK_MOUT_TOP_ACLK_IMEM_266,
		   int_bus01_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_IMEM_200, TOP, CLK_MOUT_TOP_ACLK_IMEM_200,
		   int_bus01_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_IMEM_100, TOP, CLK_MOUT_TOP_ACLK_IMEM_100,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_MFC_600, TOP, CLK_MOUT_TOP_ACLK_MFC_600,
		   int_bus0123_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_MSCL0_528, TOP, CLK_MOUT_TOP_ACLK_MSCL0_528,
		   int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_MSCL1_528, TOP, CLK_MOUT_TOP_ACLK_MSCL1_528,
		   int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_PERIS_66, TOP, CLK_MOUT_TOP_ACLK_PERIS_66,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_FSYS0_200, TOP, CLK_MOUT_TOP_ACLK_FSYS0_200,
		   int_bus01_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_FSYS1_200, TOP, CLK_MOUT_TOP_ACLK_FSYS1_200,
		   int_bus01_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_PERIC0_66, TOP, CLK_MOUT_TOP_ACLK_PERIC0_66,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_PERIC1_66, TOP, CLK_MOUT_TOP_ACLK_PERIC1_66,
		   int_bus0_osc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_ISP0_TREX_528, TOP,
		   CLK_MOUT_TOP_ACLK_ISP0_TREX_528,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_ISP0_ISP0_528, TOP,
		   CLK_MOUT_TOP_ACLK_ISP0_ISP0_528,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_ISP0_TPU_400, TOP,
		   CLK_MOUT_TOP_ACLK_ISP0_TPU_400,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_ISP0_PXL_ASBS_IS_C_FROM_IS_D, TOP,
		   CLK_MOUT_TOP_ACLK_ISP0_PXL_ASBS_IS_C_FROM_IS_D,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_ISP1_ISP1_468, TOP,
		   CLK_MOUT_TOP_ACLK_ISP1_ISP1_468,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_ARM_672, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_ARM_672, int_cam1_arm_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_TREX_VRA_528, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_TREX_VRA_528,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_TREX_B_528, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_TREX_B_528,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_BUS_264, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_BUS_264,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_PERI_84, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_PERI_84, int_bus02_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_CSIS2_414, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_CSIS2_414,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_CSIS3_132, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_CSIS3_132, int_bus01_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM1_SCL_566, TOP,
		   CLK_MOUT_TOP_ACLK_CAM1_SCL_566,
		   int_bus0123_isp_mfc_parents),

	MEMBER_DIV(TOP_DIV_ACLK_BUS0_528, TOP, CLK_DOUT_TOP_ACLK_BUS0_528),
	MEMBER_DIV(TOP_DIV_ACLK_BUS1_528, TOP, CLK_DOUT_TOP_ACLK_BUS1_528),
	MEMBER_DIV(TOP_DIV_ACLK_BUS0_200, TOP, CLK_DOUT_TOP_ACLK_BUS0_200),
	MEMBER_DIV(TOP_DIV_PCLK_BUS0_132, TOP, CLK_DOUT_TOP_PCLK_BUS0_132),
	MEMBER_DIV(TOP_DIV_PCLK_BUS1_132, TOP, CLK_DOUT_TOP_PCLK_BUS1_132),
	MEMBER_DIV(TOP_DIV_ACLK_IMEM_266, TOP, CLK_DOUT_TOP_ACLK_IMEM_266),
	MEMBER_DIV(TOP_DIV_ACLK_IMEM_200, TOP, CLK_DOUT_TOP_ACLK_IMEM_200),
	MEMBER_DIV(TOP_DIV_ACLK_IMEM_100, TOP, CLK_DOUT_TOP_ACLK_IMEM_100),
	MEMBER_DIV(TOP_DIV_ACLK_MFC_600, TOP, CLK_DOUT_TOP_ACLK_MFC_600),
	MEMBER_DIV(TOP_DIV_ACLK_MSCL0_528, TOP, CLK_DOUT_TOP_ACLK_MSCL0_528),
	MEMBER_DIV(TOP_DIV_ACLK_MSCL1_528, TOP, CLK_DOUT_TOP_ACLK_MSCL1_528),
	MEMBER_DIV(TOP_DIV_ACLK_PERIS_66, TOP, CLK_DOUT_TOP_ACLK_PERIS_66),
	MEMBER_DIV(TOP_DIV_ACLK_FSYS0_200, TOP, CLK_DOUT_TOP_ACLK_FSYS0_200),
	MEMBER_DIV(TOP_DIV_ACLK_FSYS1_200, TOP, CLK_DOUT_TOP_ACLK_FSYS1_200),
	MEMBER_DIV(TOP_DIV_ACLK_PERIC0_66, TOP, CLK_DOUT_TOP_ACLK_PERIC0_66),
	MEMBER_DIV(TOP_DIV_ACLK_PERIC1_66, TOP, CLK_DOUT_TOP_ACLK_PERIC1_66),
	MEMBER_DIV(TOP_DIV_ACLK_ISP0_TREX_528, TOP,
		   CLK_DOUT_TOP_ACLK_ISP0_TREX_528),
	MEMBER_DIV(TOP_DIV_ACLK_ISP0_ISP0_528, TOP,
		   CLK_DOUT_TOP_ACLK_ISP0_ISP0_528),
	MEMBER_DIV(TOP_DIV_ACLK_ISP0_TPU_400, TOP,
		   CLK_DOUT_TOP_ACLK_ISP0_TPU_400),
	MEMBER_DIV(TOP_DIV_ACLK_ISP0_PXL_ASBS_IS_C_FROM_IS_D, TOP,
		   CLK_DOUT_TOP_ACLK_ISP0_PXL_ASBS_IS_C_FROM_IS_D),
	MEMBER_DIV(TOP_DIV_ACLK_ISP1_ISP1_468, TOP,
		   CLK_DOUT_TOP_ACLK_ISP1_ISP1_468),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_ARM_672, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_ARM_672),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_TREX_VRA_528, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_TREX_VRA_528),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_TREX_B_528, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_TREX_B_528),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_BUS_264, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_BUS_264),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_PERI_84, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_PERI_84),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_CSIS2_414, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_CSIS2_414),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_CSIS3_132, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_CSIS3_132),
	MEMBER_DIV(TOP_DIV_ACLK_CAM1_SCL_566, TOP,
		   CLK_DOUT_TOP_ACLK_CAM1_SCL_566),
};

static const struct exynos8890_dvfs_member_desc cam_members[] = {
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_TREX_528, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_TREX_528,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_CSIS0_414, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_CSIS0_414,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_CSIS1_168, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_CSIS1_168, int_bus02_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_CSIS2_234, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_CSIS2_234,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_3AA0_414, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_3AA0_414,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_3AA1_414, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_3AA1_414,
		   int_bus0123_isp_mfc_parents),
	MEMBER_MUX(TOP_MUX_ACLK_CAM0_CSIS3_132, TOP,
		   CLK_MOUT_TOP_ACLK_CAM0_CSIS3_132, int_bus01_parents),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_TREX_528, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_TREX_528),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_CSIS0_414, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_CSIS0_414),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_CSIS1_168, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_CSIS1_168),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_CSIS2_234, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_CSIS2_234),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_3AA0_414, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_3AA0_414),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_3AA1_414, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_3AA1_414),
	MEMBER_DIV(TOP_DIV_ACLK_CAM0_CSIS3_132, TOP,
		   CLK_DOUT_TOP_ACLK_CAM0_CSIS3_132),
};

static const struct exynos8890_dvfs_member_desc disp_members[] = {
	MEMBER_MUX(TOP_MUX_ACLK_DISP0_0_400, TOP,
		   CLK_MOUT_TOP_ACLK_DISP0_0_400, int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_DISP0_1_400, TOP,
		   CLK_MOUT_TOP_ACLK_DISP0_1_400, int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_DISP1_0_400, TOP,
		   CLK_MOUT_TOP_ACLK_DISP1_0_400, int_bus0123_parents),
	MEMBER_MUX(TOP_MUX_ACLK_DISP1_1_400, TOP,
		   CLK_MOUT_TOP_ACLK_DISP1_1_400, int_bus0123_parents),
	MEMBER_DIV(TOP_DIV_ACLK_DISP0_0_400, TOP,
		   CLK_DOUT_TOP_ACLK_DISP0_0_400),
	MEMBER_DIV(TOP_DIV_ACLK_DISP0_1_400, TOP,
		   CLK_DOUT_TOP_ACLK_DISP0_1_400),
	MEMBER_DIV(TOP_DIV_ACLK_DISP1_0_400, TOP,
		   CLK_DOUT_TOP_ACLK_DISP1_0_400),
	MEMBER_DIV(TOP_DIV_ACLK_DISP1_1_400, TOP,
		   CLK_DOUT_TOP_ACLK_DISP1_1_400),
};

static const struct exynos8890_dvfs_member_desc g3d_members[] = {
	MEMBER_PLL(G3D_PLL, TOP, CLK_FOUT_G3D_PLL),
	MEMBER_DIV_MAX(G3D_DIV_ACLK_G3D, G3D, CLK_DOUT_G3D_ACLK_G3D, 7),
	MEMBER_DIV_MAX(G3D_DIV_PCLK_G3D, G3D, CLK_DOUT_G3D_PCLK_G3D, 7),
	MEMBER_DIV_MAX(G3D_DIV_SCLK_HPM_G3D, G3D,
		       CLK_DOUT_G3D_SCLK_HPM_G3D, 3),
	MEMBER_DIV_MAX(G3D_DIV_SCLK_ATE_G3D, G3D,
		       CLK_DOUT_G3D_SCLK_ATE_G3D, 15),
};

static const struct exynos8890_dvfs_member_desc *
exynos8890_find_member_desc(enum exynos8890_calib_domain_id id,
			    const char *name)
{
	const struct exynos8890_dvfs_member_desc *table;
	unsigned int count;
	unsigned int i;

	switch (id) {
	case EXYNOS8890_CALIB_INT:
		table = int_members;
		count = ARRAY_SIZE(int_members);
		break;
	case EXYNOS8890_CALIB_CAM:
		table = cam_members;
		count = ARRAY_SIZE(cam_members);
		break;
	case EXYNOS8890_CALIB_DISP:
		table = disp_members;
		count = ARRAY_SIZE(disp_members);
		break;
	case EXYNOS8890_CALIB_G3D:
		table = g3d_members;
		count = ARRAY_SIZE(g3d_members);
		break;
	default:
		return NULL;
	}

	for (i = 0; i < count; i++)
		if (!strcmp(table[i].name, name))
			return &table[i];

	return NULL;
}

static struct clk *exynos8890_dvfs_get_clk(
		const struct exynos8890_dvfs_clk_ref *ref,
		struct clk_hw **g3d_hws)
{
	struct clk_hw *hw;

	switch (ref->provider) {
	case DVFS_PROVIDER_TOP:
		if (!top_hws)
			return ERR_PTR(-EPROBE_DEFER);
		hw = top_hws[ref->id];
		break;
	case DVFS_PROVIDER_G3D:
		if (!g3d_hws)
			return ERR_PTR(-EPROBE_DEFER);
		hw = g3d_hws[ref->id];
		break;
	case DVFS_PROVIDER_OSC:
		if (!top_oscclk)
			return ERR_PTR(-EPROBE_DEFER);
		hw = __clk_get_hw(top_oscclk);
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR_OR_NULL(hw))
		return ERR_PTR(-ENOENT);

	return clk_hw_get_clk(hw, NULL);
}

static bool exynos8890_dvfs_member_matches(
		const struct exynos8890_dvfs_member *member, u64 value)
{
	struct clk_hw *hw;
	struct clk *parent;
	unsigned long parent_rate;
	unsigned long rate;
	u64 ratio;

	switch (member->type) {
	case EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ:
		hw = __clk_get_hw(member->clk);
		if (member->allow_zero) {
			if (!value)
				return hw && !clk_hw_is_enabled(hw);
			return hw && clk_hw_is_enabled(hw) &&
			       clk_get_rate(member->clk) == value;
		}
		return clk_get_rate(member->clk) == value;
	case EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR:
		if (value >= member->num_parents)
			return false;
		parent = clk_get_parent(member->clk);
		return parent && clk_is_match(parent, member->parents[value]);
	case EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE:
		parent = clk_get_parent(member->clk);
		if (!parent)
			return false;
		parent_rate = clk_get_rate(parent);
		rate = clk_get_rate(member->clk);
		if (!rate)
			return false;
		ratio = DIV_ROUND_CLOSEST_ULL(parent_rate, rate);
		return ratio && ratio - 1 == value;
	case EXYNOS8890_CALIB_MEMBER_GATE_STATE:
		return !!__clk_is_enabled(member->clk) == !!value;
	default:
		return false;
	}
}

static int exynos8890_dvfs_set_member(struct exynos8890_dvfs_member *member,
				      u64 value)
{
	bool claimed = false;
	struct clk *parent;
	unsigned long rate;
	int ret;

	switch (member->type) {
	case EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ:
		if (!value) {
			if (!member->allow_zero)
				return -EINVAL;
			if (!member->gate_owned) {
				ret = clk_prepare_enable(member->clk);
				if (ret)
					break;
				member->gate_owned = true;
			}
			clk_disable_unprepare(member->clk);
			member->gate_owned = false;
			ret = 0;
			break;
		}
		if (member->allow_zero && !member->gate_owned) {
			ret = clk_prepare_enable(member->clk);
			if (ret)
				break;
			member->gate_owned = true;
			claimed = true;
		}
		ret = clk_set_rate(member->clk, value);
		if (ret && claimed) {
			clk_disable_unprepare(member->clk);
			member->gate_owned = false;
		}
		break;
	case EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR:
		if (value >= member->num_parents)
			return -EINVAL;
		ret = clk_set_parent(member->clk, member->parents[value]);
		break;
	case EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE:
		parent = clk_get_parent(member->clk);
		if (!parent || value == U64_MAX)
			return -EINVAL;
		rate = DIV_ROUND_CLOSEST_ULL(clk_get_rate(parent), value + 1);
		ret = clk_set_rate(member->clk, rate);
		break;
	case EXYNOS8890_CALIB_MEMBER_GATE_STATE:
		if (value && !__clk_is_enabled(member->clk)) {
			ret = clk_prepare_enable(member->clk);
			if (!ret)
				member->gate_owned = true;
		} else if (!value && member->gate_owned) {
			clk_disable_unprepare(member->clk);
			member->gate_owned = false;
			ret = 0;
		} else {
			ret = 0;
		}
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	return exynos8890_dvfs_member_matches(member, value) ? 0 : -EIO;
}

static int exynos8890_dvfs_bind_members(struct exynos8890_dvfs_clock *domain,
					struct clk_hw **g3d_hws)
{
	const struct exynos8890_dvfs_member_desc *desc;
	struct exynos8890_dvfs_member *member;
	unsigned int i, j;

	domain->members = kcalloc(domain->calib->num_members,
				  sizeof(*domain->members), GFP_KERNEL);
	if (!domain->members)
		return -ENOMEM;

	for (i = 0; i < domain->calib->num_members; i++) {
		desc = exynos8890_find_member_desc(domain->calib->id,
						  domain->calib->members[i].name);
		if (!desc || desc->type != domain->calib->members[i].type) {
			pr_err("exynos8890-dvfs: unsupported %s member %s (type %u)\n",
			       domain->calib->name,
			       domain->calib->members[i].name,
			       domain->calib->members[i].type);
			return -EINVAL;
		}

		member = &domain->members[i];
		member->type = desc->type;
		member->max_value = desc->max_value;
		member->allow_zero = desc->allow_zero;
		member->clk = exynos8890_dvfs_get_clk(&desc->clock, g3d_hws);
		if (IS_ERR(member->clk))
			return PTR_ERR(member->clk);

		member->num_parents = desc->num_parents;
		if (!desc->num_parents)
			continue;

		member->parents = kcalloc(desc->num_parents,
					  sizeof(*member->parents), GFP_KERNEL);
		if (!member->parents)
			return -ENOMEM;

		for (j = 0; j < desc->num_parents; j++) {
			member->parents[j] =
				exynos8890_dvfs_get_clk(&desc->parents[j], g3d_hws);
			if (IS_ERR(member->parents[j]))
				return PTR_ERR(member->parents[j]);
		}
	}

	return 0;
}

static int
exynos8890_dvfs_validate_rows(struct exynos8890_dvfs_clock *domain)
{
	const struct exynos8890_calib_domain *calib = domain->calib;
	struct exynos8890_dvfs_member *entry;
	u64 min_rate, max_rate, rate;
	u64 value;
	unsigned int member, level;
	bool found, selectable;

	for (level = 0; level < calib->num_opps; level++) {
		selectable = calib->opps[level].enabled &&
			     calib->opps[level].rate_hz >= calib->min_rate_hz &&
			     calib->opps[level].rate_hz <= calib->max_rate_hz;
		if (!selectable && calib->id != EXYNOS8890_CALIB_G3D)
			continue;
		for (member = 0; member < calib->num_members; member++) {
			entry = &domain->members[member];
			/* Disabled G3D rows supply only mux/div switch values. */
			if (!selectable &&
			    entry->type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ)
				continue;
			value = exynos8890_calib_member_value(calib, level, member);
			switch (entry->type) {
			case EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ:
				if ((!value && entry->allow_zero) ||
				    (value && value <= ULONG_MAX))
					continue;
				break;
			case EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR:
				if (value <= entry->max_value)
					continue;
				break;
			case EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE:
				if (value <= entry->max_value)
					continue;
				break;
			case EXYNOS8890_CALIB_MEMBER_GATE_STATE:
				if (value <= 1)
					continue;
				break;
			default:
				break;
			}
			pr_err("exynos8890-dvfs: invalid %s row %u member %s value %llu\n",
			       calib->name, level, calib->members[member].name,
			       (unsigned long long)value);
			return -EINVAL;
		}
	}

	for (member = 0; member < calib->num_members; member++) {
		if (domain->members[member].type !=
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ)
			continue;

		min_rate = U64_MAX;
		max_rate = 0;
		found = false;
		for (level = 0; level < calib->num_opps; level++) {
			if (!calib->opps[level].enabled ||
			    calib->opps[level].rate_hz < calib->min_rate_hz ||
			    calib->opps[level].rate_hz > calib->max_rate_hz)
				continue;

			rate = exynos8890_calib_member_value(calib, level,
							     member);
			if (!rate && domain->members[member].allow_zero)
				continue;
			if (!rate || rate > ULONG_MAX) {
				pr_err("exynos8890-dvfs: invalid %s PLL row %u for %s\n",
				       calib->members[member].name, level,
				       calib->name);
				return -EINVAL;
			}
			min_rate = min(min_rate, rate);
			max_rate = max(max_rate, rate);
			found = true;
		}
		if (!found)
			return -EINVAL;

		if (calib->id == EXYNOS8890_CALIB_CAM) {
			if (min_rate == max_rate) {
				pr_info("exynos8890-dvfs: %s %s is invariant at %llu Hz\n",
					calib->name, calib->members[member].name,
					(unsigned long long)min_rate);
			} else {
				/*
				 * INT rows can remain parented to ISP_PLL after their
				 * transaction.  Until both aggregates share a dependency
				 * model, allowing CAM to retune it would create two owners.
				 */
				pr_err("exynos8890-dvfs: reject shared %s: CAM rows vary from %llu to %llu Hz\n",
				       calib->members[member].name,
				       (unsigned long long)min_rate,
				       (unsigned long long)max_rate);
				return -EBUSY;
			}
		}
	}

	return 0;
}

static int exynos8890_dvfs_claim_live_plls(
		struct exynos8890_dvfs_clock *domain, unsigned int level)
{
	u64 value;
	unsigned int i;
	int ret;

	for (i = 0; i < domain->calib->num_members; i++) {
		struct exynos8890_dvfs_member *member = &domain->members[i];

		if (member->type != EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ ||
		    !member->allow_zero)
			continue;
		value = exynos8890_calib_member_value(domain->calib, level, i);
		if (!value)
			continue;
		ret = clk_prepare_enable(member->clk);
		if (ret)
			return ret;
		member->gate_owned = true;
		if (!exynos8890_dvfs_member_matches(member, value))
			return -EUCLEAN;
	}

	return 0;
}

static int exynos8890_dvfs_find_level(struct exynos8890_dvfs_clock *domain,
				      unsigned long rate)
{
	unsigned long best_delta = ULONG_MAX;
	unsigned long delta;
	int best = -EINVAL;
	unsigned int i;

	for (i = 0; i < domain->calib->num_opps; i++) {
		if (!domain->calib->opps[i].enabled)
			continue;
		if (domain->calib->opps[i].rate_hz < domain->calib->min_rate_hz ||
		    domain->calib->opps[i].rate_hz > domain->calib->max_rate_hz)
			continue;
		if (domain->calib->opps[i].rate_hz > rate)
			delta = domain->calib->opps[i].rate_hz - rate;
		else
			delta = rate - domain->calib->opps[i].rate_hz;
		if (delta < best_delta) {
			best_delta = delta;
			best = i;
		}
	}

	return best;
}

static int
exynos8890_dvfs_find_floor_level(struct exynos8890_dvfs_clock *domain,
				 unsigned long rate)
{
	unsigned long best_rate = 0;
	int best = -EINVAL;
	unsigned int i;

	for (i = 0; i < domain->calib->num_opps; i++) {
		/* Switch rows are hardware staging data, not selectable OPPs. */
		if (domain->calib->opps[i].rate_hz <= rate &&
		    domain->calib->opps[i].rate_hz > best_rate) {
			best_rate = domain->calib->opps[i].rate_hz;
			best = i;
		}
	}

	return best;
}

static int exynos8890_dvfs_find_live_level(
		struct exynos8890_dvfs_clock *domain)
{
	const struct exynos8890_calib_domain *calib = domain->calib;
	unsigned int level, member;
	bool match;

	for (level = 0; level < calib->num_opps; level++) {
		if (!calib->opps[level].enabled)
			continue;
		if (calib->opps[level].rate_hz < calib->min_rate_hz ||
		    calib->opps[level].rate_hz > calib->max_rate_hz)
			continue;
		match = true;
		for (member = 0; member < calib->num_members; member++) {
			if (!exynos8890_dvfs_member_matches(
				    &domain->members[member],
				    exynos8890_calib_member_value(calib, level,
							  member))) {
				match = false;
				break;
			}
		}
		if (match)
			return level;
	}

	return -EUCLEAN;
}

enum exynos8890_dvfs_stage {
	DVFS_GATE_HIGH,
	DVFS_DIV_HIGH,
	DVFS_PLL_LOW,
	DVFS_PLL_ENABLE,
	DVFS_MUX_DIFF,
	DVFS_PLL_HIGH,
	DVFS_PLL_DISABLE,
	DVFS_PLL_DIFF,
	DVFS_DIV_LOW,
	DVFS_GATE_LOW,
};

static int
exynos8890_dvfs_get_member_value(struct exynos8890_dvfs_member *member,
				 u64 *value)
{
	struct clk_hw *hw;
	struct clk *parent;
	unsigned long parent_rate;
	unsigned long rate;
	u64 ratio;
	unsigned int i;

	switch (member->type) {
	case EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ:
		hw = __clk_get_hw(member->clk);
		if (member->allow_zero && hw && !clk_hw_is_enabled(hw)) {
			*value = 0;
			return 0;
		}
		*value = clk_get_rate(member->clk);
		return *value ? 0 : -EIO;
	case EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR:
		parent = clk_get_parent(member->clk);
		if (!parent)
			return -EIO;
		for (i = 0; i < member->num_parents; i++) {
			if (clk_is_match(parent, member->parents[i])) {
				*value = i;
				return 0;
			}
		}
		return -EUCLEAN;
	case EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE:
		parent = clk_get_parent(member->clk);
		if (!parent)
			return -EIO;
		parent_rate = clk_get_rate(parent);
		rate = clk_get_rate(member->clk);
		if (!parent_rate || !rate)
			return -EIO;
		ratio = DIV_ROUND_CLOSEST_ULL(parent_rate, rate);
		if (!ratio)
			return -EIO;
		*value = ratio - 1;
		return 0;
	case EXYNOS8890_CALIB_MEMBER_GATE_STATE:
		*value = !!__clk_is_enabled(member->clk);
		return 0;
	default:
		return -EINVAL;
	}
}

static bool exynos8890_stage_applies(enum exynos8890_dvfs_stage stage,
				     enum exynos8890_calib_member_type type,
				     u64 live_value, u64 target)
{
	switch (stage) {
	case DVFS_GATE_HIGH:
		return type == EXYNOS8890_CALIB_MEMBER_GATE_STATE &&
		       live_value < target;
	case DVFS_DIV_HIGH:
		return type ==
		       EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE &&
		       live_value < target;
	case DVFS_PLL_LOW:
		return type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		       target && live_value > target;
	case DVFS_PLL_ENABLE:
		return type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		       !live_value && target;
	case DVFS_MUX_DIFF:
		return type == EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR &&
		       live_value != target;
	case DVFS_PLL_HIGH:
		return type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		       live_value && live_value < target;
	case DVFS_PLL_DISABLE:
		return type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		       live_value && !target;
	case DVFS_PLL_DIFF:
		return type == EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		       live_value != target;
	case DVFS_DIV_LOW:
		return type ==
		       EXYNOS8890_CALIB_MEMBER_DIVIDER_RATIO_MINUS_ONE &&
		       live_value > target;
	case DVFS_GATE_LOW:
		return type == EXYNOS8890_CALIB_MEMBER_GATE_STATE &&
		       live_value > target;
	default:
		return false;
	}
}

static int
exynos8890_dvfs_apply_stage(struct exynos8890_dvfs_clock *domain,
			    unsigned int to, enum exynos8890_dvfs_stage stage)
{
	const struct exynos8890_calib_domain *calib = domain->calib;
	struct exynos8890_dvfs_member *member;
	u64 live_value, target;
	unsigned int i;
	int ret;

	for (i = 0; i < calib->num_members; i++) {
		member = &domain->members[i];
		target = exynos8890_calib_member_value(calib, to, i);
		ret = exynos8890_dvfs_get_member_value(member, &live_value);
		/* An unknown mux parent is necessarily different from the row. */
		if (ret && stage == DVFS_MUX_DIFF &&
		    member->type == EXYNOS8890_CALIB_MEMBER_MUX_SELECTOR)
			live_value = U64_MAX;
		else if (ret)
			return ret;

		if (!exynos8890_stage_applies(stage, member->type,
					      live_value, target))
			continue;

		ret = exynos8890_dvfs_set_member(member, target);
		if (ret)
			return ret;
	}

	return 0;
}

static int
exynos8890_dvfs_apply_no_switch(struct exynos8890_dvfs_clock *domain,
				unsigned int to)
{
	static const enum exynos8890_dvfs_stage stages[] = {
		DVFS_GATE_HIGH,
		DVFS_DIV_HIGH,
		DVFS_PLL_LOW,
		DVFS_PLL_ENABLE,
		DVFS_MUX_DIFF,
		DVFS_PLL_HIGH,
		DVFS_PLL_DISABLE,
		DVFS_DIV_LOW,
		DVFS_GATE_LOW,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(stages); i++) {
		ret = exynos8890_dvfs_apply_stage(domain, to, stages[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static bool
exynos8890_dvfs_level_matches(struct exynos8890_dvfs_clock *domain,
			      unsigned int level)
{
	const struct exynos8890_calib_domain *calib = domain->calib;
	u64 value;
	unsigned int member;

	for (member = 0; member < calib->num_members; member++) {
		value = exynos8890_calib_member_value(calib, level, member);
		if (!exynos8890_dvfs_member_matches(&domain->members[member], value))
			return false;
	}

	return true;
}

static void
exynos8890_dvfs_latch_fault(struct exynos8890_dvfs_clock *domain,
			    int transition_error, int recovery_error)
{
	WRITE_ONCE(domain->faulted, true);
	WRITE_ONCE(domain->current_rate, 0);
	pr_crit("exynos8890-dvfs: %s has no recoverable rate (%d, original %d); scaling disabled\n",
		domain->calib->name, recovery_error, transition_error);
}

static int exynos8890_dvfs_transition(struct exynos8890_dvfs_clock *domain,
				      unsigned int from, unsigned int to)
{
	int recovery;
	int ret;

	/*
	 * OPP raises voltage before an upward clock change and lowers it only
	 * after a downward change succeeds.  Recovery happens inside set_rate(),
	 * so either the old or the higher target voltage remains safe throughout
	 * both transactions.
	 */
	ret = exynos8890_dvfs_apply_no_switch(domain, to);
	if (!ret && exynos8890_dvfs_level_matches(domain, to))
		return 0;
	if (!ret)
		ret = -EUCLEAN;

	/* Use the same safe stages against live state after a partial failure. */
	recovery = exynos8890_dvfs_apply_no_switch(domain, from);
	if (!recovery && !exynos8890_dvfs_level_matches(domain, from))
		recovery = -EUCLEAN;
	if (recovery) {
		exynos8890_dvfs_latch_fault(domain, ret, recovery);
		return recovery;
	}

	return ret;
}

static int exynos8890_g3d_set_parent(struct clk *clk, struct clk *parent)
{
	struct clk *live_parent;
	int ret;

	ret = clk_set_parent(clk, parent);
	if (ret)
		return ret;
	live_parent = clk_get_parent(clk);
	if (!live_parent || !clk_is_match(live_parent, parent))
		return -EIO;

	return 0;
}

static int exynos8890_g3d_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;

	if (!rate)
		return -EINVAL;
	ret = clk_set_rate(clk, rate);
	if (ret)
		return ret;

	return clk_get_rate(clk) == rate ? 0 : -EIO;
}

static int
exynos8890_g3d_prepare_switch(struct exynos8890_dvfs_clock *domain,
			      unsigned int divider)
{
	struct exynos8890_g3d_switch *sw = domain->g3d;
	unsigned long switch_rate;
	int ret;

	ret = exynos8890_g3d_set_parent(sw->source_mux, sw->source_parent);
	if (ret)
		return ret;
	switch_rate = DIV_ROUND_CLOSEST(clk_get_rate(sw->source_parent),
					divider + 1);
	ret = exynos8890_g3d_set_rate(sw->source_div, switch_rate);
	if (ret)
		return ret;
	if (!__clk_is_enabled(sw->source_gate)) {
		ret = clk_prepare_enable(sw->source_gate);
		if (ret)
			return ret;
		sw->gate_owned = true;
	}
	if (!__clk_is_enabled(sw->source_gate))
		return -EIO;

	return exynos8890_g3d_set_parent(sw->bus_usermux, sw->bus_parent);
}

static int
exynos8890_g3d_finish_switch(struct exynos8890_dvfs_clock *domain)
{
	struct exynos8890_g3d_switch *sw = domain->g3d;
	struct clk *parent;
	unsigned long source_rate;
	int ret;

	/* Never remove the safe source until the main mux is confirmed on PLL. */
	parent = clk_get_parent(sw->main_mux);
	if (!parent || !clk_is_match(parent, sw->main_pll_parent))
		return -EUCLEAN;

	parent = sw->bus_idle_parent;
	ret = exynos8890_g3d_set_parent(sw->bus_usermux, parent);
	if (ret)
		return ret;
	source_rate = clk_get_rate(sw->source_parent);
	ret = exynos8890_g3d_set_rate(sw->source_div, source_rate);
	if (ret)
		return ret;

	if (sw->gate_owned) {
		clk_disable_unprepare(sw->source_gate);
		sw->gate_owned = false;
	}

	return 0;
}

static void
exynos8890_g3d_keep_switch_powered(struct exynos8890_dvfs_clock *domain)
{
	struct exynos8890_g3d_switch *sw = domain->g3d;

	if (__clk_is_enabled(sw->source_gate))
		return;
	if (!clk_prepare_enable(sw->source_gate))
		sw->gate_owned = true;
}

static int
exynos8890_g3d_apply_switch(struct exynos8890_dvfs_clock *domain,
			    unsigned int switch_level, unsigned int to,
			    unsigned int source_divider)
{
	struct exynos8890_g3d_switch *sw = domain->g3d;
	int ret;

	ret = exynos8890_g3d_prepare_switch(domain, source_divider);
	if (ret)
		return ret;

	/* Vendor sequence: old -> switch divider, then select safe source. */
	ret = exynos8890_dvfs_apply_stage(domain, switch_level, DVFS_DIV_HIGH);
	if (ret)
		return ret;
	ret = exynos8890_g3d_set_parent(sw->main_mux, sw->main_bus_parent);
	if (ret)
		return ret;
	ret = exynos8890_dvfs_apply_stage(domain, switch_level, DVFS_MUX_DIFF);
	if (ret)
		return ret;
	ret = exynos8890_dvfs_apply_stage(domain, switch_level, DVFS_DIV_LOW);
	if (ret)
		return ret;

	/* The main clock is isolated, so the PLL can move in either direction. */
	ret = exynos8890_dvfs_apply_stage(domain, to, DVFS_PLL_DIFF);
	if (ret)
		return ret;
	ret = exynos8890_dvfs_apply_stage(domain, to, DVFS_DIV_HIGH);
	if (ret)
		return ret;
	ret = exynos8890_g3d_set_parent(sw->pll_usermux, sw->pll_parent);
	if (ret)
		return ret;
	ret = exynos8890_g3d_set_parent(sw->main_mux, sw->main_pll_parent);
	if (ret)
		return ret;
	ret = exynos8890_dvfs_apply_stage(domain, to, DVFS_MUX_DIFF);
	if (ret)
		return ret;
	ret = exynos8890_dvfs_apply_stage(domain, to, DVFS_DIV_LOW);
	if (ret)
		return ret;
	if (!exynos8890_dvfs_level_matches(domain, to))
		return -EUCLEAN;

	return exynos8890_g3d_finish_switch(domain);
}

static int
exynos8890_g3d_switch_transition(struct exynos8890_dvfs_clock *domain,
				 unsigned int from, unsigned int to)
{
	static const struct {
		unsigned long threshold;
		unsigned int divider;
	} switches[] = {
		{ 528000000, 0 }, { 264000000, 1 },
		{ 176000000, 2 }, {  88000000, 5 },
	};
	unsigned long from_rate = domain->calib->opps[from].rate_hz;
	unsigned long to_rate = domain->calib->opps[to].rate_hz;
	unsigned long rate_max = max(from_rate, to_rate);
	unsigned int selected = ARRAY_SIZE(switches) - 1;
	int switch_level;
	int recovery;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(switches); i++) {
		if (rate_max >= switches[i].threshold) {
			selected = i;
			break;
		}
	}

	/* Match vendor dfs_get_lv(): use the highest represented row <= source. */
	switch_level = exynos8890_dvfs_find_floor_level(domain,
							switches[selected].threshold);
	if (switch_level < 0)
		return switch_level;

	ret = exynos8890_g3d_apply_switch(domain, switch_level, to,
					  switches[selected].divider);
	if (!ret)
		return 0;

	/* Re-enter the safe path and replay the same stages to the old row. */
	recovery = exynos8890_g3d_apply_switch(domain, switch_level, from,
					       switches[selected].divider);
	if (recovery) {
		exynos8890_g3d_keep_switch_powered(domain);
		exynos8890_dvfs_latch_fault(domain, ret, recovery);
		return recovery;
	}

	return ret;
}

static unsigned long exynos8890_dvfs_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);

	return READ_ONCE(domain->faulted) ? 0 :
	       READ_ONCE(domain->current_rate);
}

static int exynos8890_dvfs_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);
	int level;

	if (READ_ONCE(domain->faulted))
		return -EIO;

	level = exynos8890_dvfs_find_level(domain, req->rate);
	if (level < 0)
		return level;
	req->rate = domain->calib->opps[level].rate_hz;
	return 0;
}

static int exynos8890_dvfs_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);
	int from, to, ret;

	if (READ_ONCE(domain->faulted))
		return -EIO;

	from = exynos8890_dvfs_find_level(domain, domain->current_rate);
	to = exynos8890_dvfs_find_level(domain, rate);
	if (from < 0 || to < 0)
		return -EINVAL;
	if (!exynos8890_dvfs_level_matches(domain, from)) {
		exynos8890_dvfs_latch_fault(domain, -EUCLEAN, -EUCLEAN);
		return -EUCLEAN;
	}
	if (from == to)
		return 0;

	if (domain->g3d)
		ret = exynos8890_g3d_switch_transition(domain, from, to);
	else
		ret = exynos8890_dvfs_transition(domain, from, to);
	if (!ret)
		domain->current_rate = domain->calib->opps[to].rate_hz;
	return ret;
}

static int
exynos8890_dvfs_prepare_plls(struct exynos8890_dvfs_clock *domain)
{
	unsigned int i;
	int ret;

	/* Allow-zero PLLs acquire their enable reference only while a row uses them. */
	for (i = 0; i < domain->calib->num_members; i++) {
		if (domain->members[i].type !=
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ ||
		    domain->members[i].allow_zero)
			continue;

		ret = clk_prepare_enable(domain->members[i].clk);
		if (ret)
			goto unwind;
	}

	return 0;

unwind:
	while (i > 0) {
		i--;
		if (domain->members[i].type ==
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		    !domain->members[i].allow_zero)
			clk_disable_unprepare(domain->members[i].clk);
	}
	return ret;
}

static void
exynos8890_dvfs_unprepare_plls(struct exynos8890_dvfs_clock *domain)
{
	unsigned int i;

	for (i = domain->calib->num_members; i > 0; i--) {
		if (domain->members[i - 1].type ==
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ &&
		    !domain->members[i - 1].allow_zero)
			clk_disable_unprepare(domain->members[i - 1].clk);
	}
}

static int
exynos8890_dvfs_plls_are_prepared(struct exynos8890_dvfs_clock *domain)
{
	unsigned int i;

	for (i = 0; i < domain->calib->num_members; i++) {
		if (domain->members[i].type !=
		    EXYNOS8890_CALIB_MEMBER_PLL_RATE_HZ ||
		    domain->members[i].allow_zero)
			continue;
		if (!__clk_is_enabled(domain->members[i].clk))
			return 0;
	}

	return 1;
}

static int exynos8890_dvfs_prepare(struct clk_hw *hw)
{
	return exynos8890_dvfs_prepare_plls(to_exynos8890_dvfs(hw));
}

static void exynos8890_dvfs_unprepare(struct clk_hw *hw)
{
	exynos8890_dvfs_unprepare_plls(to_exynos8890_dvfs(hw));
}

static int exynos8890_dvfs_is_prepared(struct clk_hw *hw)
{
	return exynos8890_dvfs_plls_are_prepared(to_exynos8890_dvfs(hw));
}

static int exynos8890_g3d_prepare(struct clk_hw *hw)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);
	int ret;

	ret = exynos8890_dvfs_prepare_plls(domain);
	if (ret)
		return ret;

	/* Keep the PLL stable before exposing it through the G3D core gate. */
	ret = clk_prepare_enable(domain->g3d->core_gate);
	if (ret)
		exynos8890_dvfs_unprepare_plls(domain);
	return ret;
}

static void exynos8890_g3d_unprepare(struct clk_hw *hw)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);

	clk_disable_unprepare(domain->g3d->core_gate);
	exynos8890_dvfs_unprepare_plls(domain);
}

static int exynos8890_g3d_is_prepared(struct clk_hw *hw)
{
	struct exynos8890_dvfs_clock *domain = to_exynos8890_dvfs(hw);

	return __clk_is_enabled(domain->g3d->core_gate) &&
	       exynos8890_dvfs_plls_are_prepared(domain);
}

static const struct clk_ops exynos8890_dvfs_ops = {
	.prepare = exynos8890_dvfs_prepare,
	.unprepare = exynos8890_dvfs_unprepare,
	.is_prepared = exynos8890_dvfs_is_prepared,
	.recalc_rate = exynos8890_dvfs_recalc_rate,
	.determine_rate = exynos8890_dvfs_determine_rate,
	.set_rate = exynos8890_dvfs_set_rate,
};

static const struct clk_ops exynos8890_g3d_dvfs_ops = {
	.prepare = exynos8890_g3d_prepare,
	.unprepare = exynos8890_g3d_unprepare,
	.is_prepared = exynos8890_g3d_is_prepared,
	.recalc_rate = exynos8890_dvfs_recalc_rate,
	.determine_rate = exynos8890_dvfs_determine_rate,
	.set_rate = exynos8890_dvfs_set_rate,
};

static void __init
exynos8890_dvfs_release(struct exynos8890_dvfs_clock *domain)
{
	struct exynos8890_g3d_switch *sw;
	struct clk *switch_clks[11];
	unsigned int i, j;

	if (!domain)
		return;
	if (domain->registered)
		clk_hw_unregister(&domain->hw);

	for (i = 0; domain->members && i < domain->calib->num_members; i++) {
		if (domain->members[i].gate_owned)
			clk_disable_unprepare(domain->members[i].clk);
		for (j = 0; j < domain->members[i].num_parents; j++) {
			if (!IS_ERR_OR_NULL(domain->members[i].parents[j]))
				clk_put(domain->members[i].parents[j]);
		}
		kfree(domain->members[i].parents);
		if (!IS_ERR_OR_NULL(domain->members[i].clk))
			clk_put(domain->members[i].clk);
	}
	kfree(domain->members);

	sw = domain->g3d;
	if (sw) {
		switch_clks[0] = sw->source_mux;
		switch_clks[1] = sw->source_parent;
		switch_clks[2] = sw->source_div;
		switch_clks[3] = sw->source_gate;
		switch_clks[4] = sw->bus_usermux;
		switch_clks[5] = sw->main_mux;
		switch_clks[6] = sw->main_pll_parent;
		switch_clks[7] = sw->main_bus_parent;
		switch_clks[8] = sw->pll_usermux;
		switch_clks[9] = sw->pll_parent;
		switch_clks[10] = sw->core_gate;
		for (i = 0; i < ARRAY_SIZE(switch_clks); i++) {
			if (!IS_ERR_OR_NULL(switch_clks[i]))
				clk_put(switch_clks[i]);
		}
		kfree(sw);
	}
	kfree(domain);
}

static struct exynos8890_dvfs_clock * __init
exynos8890_dvfs_create(struct clk_hw **g3d_hws,
		       enum exynos8890_calib_domain_id id, const char *name)
{
	const struct exynos8890_calib_domain *calib;
	struct exynos8890_dvfs_clock *domain;
	int level;
	int ret;

	calib = exynos8890_calib_get_domain(id);
	if (IS_ERR(calib))
		return ERR_PTR(PTR_ERR(calib));

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return ERR_PTR(-ENOMEM);
	domain->calib = calib;

	ret = exynos8890_dvfs_bind_members(domain, g3d_hws);
	if (ret)
		goto err_free;
	ret = exynos8890_dvfs_validate_rows(domain);
	if (ret)
		goto err_free;

	level = exynos8890_dvfs_find_live_level(domain);
	if (level < 0) {
		ret = level;
		goto err_free;
	}
	ret = exynos8890_dvfs_claim_live_plls(domain, level);
	if (ret)
		goto err_free;
	domain->current_rate = calib->opps[level].rate_hz;

	domain->init.name = name;
	domain->init.ops = id == EXYNOS8890_CALIB_G3D ?
			   &exynos8890_g3d_dvfs_ops : &exynos8890_dvfs_ops;
	domain->init.flags = CLK_GET_RATE_NOCACHE;
	domain->hw.init = &domain->init;
	return domain;

err_free:
	exynos8890_dvfs_release(domain);
	return ERR_PTR(ret);
}

static int __init
exynos8890_dvfs_publish(struct samsung_clk_provider *ctx,
			struct exynos8890_dvfs_clock *domain,
			unsigned int clk_id)
{
	int ret;

	ret = clk_hw_register(NULL, &domain->hw);
	if (ret)
		return ret;
	domain->registered = true;
	samsung_clk_add_lookup(ctx, &domain->hw, clk_id);
	return 0;
}

int __init exynos8890_dvfs_register_top(struct samsung_clk_provider *ctx,
					struct device_node *np)
{
	static const enum exynos8890_calib_domain_id ids[] = {
		EXYNOS8890_CALIB_INT,
		EXYNOS8890_CALIB_CAM,
		EXYNOS8890_CALIB_DISP,
	};
	static const unsigned int clk_ids[] = {
		CLK_DVFS_INT,
		CLK_DVFS_CAM,
		CLK_DVFS_DISP,
	};
	static const char * const names[] = {
		"exynos8890-dvfs-int",
		"exynos8890-dvfs-cam",
		"exynos8890-dvfs-disp",
	};
	struct exynos8890_dvfs_clock *domains[ARRAY_SIZE(ids)] = { };
	unsigned int created = 0;
	unsigned int published = 0;
	int ret;

	top_hws = ctx->clk_data.hws;
	top_oscclk = of_clk_get_by_name(np, "oscclk");
	if (IS_ERR(top_oscclk)) {
		ret = PTR_ERR(top_oscclk);
		top_oscclk = NULL;
		return ret;
	}

	for (created = 0; created < ARRAY_SIZE(domains); created++) {
		enum exynos8890_calib_domain_id id = ids[created];
		const char *name = names[created];

		domains[created] = exynos8890_dvfs_create(NULL, id, name);
		if (IS_ERR(domains[created])) {
			ret = PTR_ERR(domains[created]);
			domains[created] = NULL;
			goto err_release;
		}
	}

	for (published = 0; published < ARRAY_SIZE(domains); published++) {
		struct exynos8890_dvfs_clock *domain = domains[published];
		unsigned int clk_id = clk_ids[published];

		ret = exynos8890_dvfs_publish(ctx, domain, clk_id);
		if (ret)
			goto err_unpublish;
	}

	return 0;

err_unpublish:
	while (published > 0) {
		published--;
		samsung_clk_add_lookup(ctx, ERR_PTR(-ENOENT),
				       clk_ids[published]);
	}
err_release:
	for (created = 0; created < ARRAY_SIZE(domains); created++)
		exynos8890_dvfs_release(domains[created]);
	clk_put(top_oscclk);
	top_oscclk = NULL;
	top_hws = NULL;
	return ret;
}

static int exynos8890_g3d_get_switch_clks(struct exynos8890_dvfs_clock *domain,
					  struct clk_hw **hws)
{
	struct exynos8890_g3d_switch *sw;
	struct clk_hw *clock_hws[11];
	struct clk **clock_slots[ARRAY_SIZE(clock_hws)];
	unsigned int i;

	if (!top_hws || !top_oscclk)
		return -ENODEV;
	clock_hws[0] = top_hws[CLK_MOUT_TOP_SCLK_BUS_PLL_G3D];
	clock_hws[1] = top_hws[CLK_MOUT_TOP_SCLK_BUS0_PLL];
	clock_hws[2] = top_hws[CLK_DOUT_TOP_SCLK_BUS_PLL_G3D];
	clock_hws[3] = top_hws[CLK_GOUT_TOP_SCLK_BUS_PLL_G3D];
	clock_hws[4] = hws[CLK_MOUT_G3D_BUS_PLL_USER];
	clock_hws[5] = hws[CLK_MOUT_G3D_G3D];
	clock_hws[6] = hws[CLK_MOUT_G3D_G3D_PLL_USER];
	clock_hws[7] = hws[CLK_MOUT_G3D_BUS_PLL_USER];
	clock_hws[8] = hws[CLK_MOUT_G3D_G3D_PLL_USER];
	clock_hws[9] = top_hws[CLK_FOUT_G3D_PLL];
	clock_hws[10] = hws[CLK_GOUT_G3D_ACLK_G3D];

	sw = kzalloc(sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;
	domain->g3d = sw;
	clock_slots[0] = &sw->source_mux;
	clock_slots[1] = &sw->source_parent;
	clock_slots[2] = &sw->source_div;
	clock_slots[3] = &sw->source_gate;
	clock_slots[4] = &sw->bus_usermux;
	clock_slots[5] = &sw->main_mux;
	clock_slots[6] = &sw->main_pll_parent;
	clock_slots[7] = &sw->main_bus_parent;
	clock_slots[8] = &sw->pll_usermux;
	clock_slots[9] = &sw->pll_parent;
	clock_slots[10] = &sw->core_gate;
	for (i = 0; i < ARRAY_SIZE(clock_hws); i++) {
		if (IS_ERR_OR_NULL(clock_hws[i]))
			return clock_hws[i] ? PTR_ERR(clock_hws[i]) : -ENOENT;
		*clock_slots[i] = clk_hw_get_clk(clock_hws[i], NULL);
		if (IS_ERR(*clock_slots[i]))
			return PTR_ERR(*clock_slots[i]);
	}

	sw->bus_parent = sw->source_gate;
	sw->bus_idle_parent = top_oscclk;
	return 0;
}

int __init exynos8890_dvfs_register_g3d(struct samsung_clk_provider *ctx,
					struct device_node *np)
{
	struct exynos8890_dvfs_clock *domain;
	struct clk_hw **hws = ctx->clk_data.hws;
	int ret;

	domain = exynos8890_dvfs_create(hws, EXYNOS8890_CALIB_G3D,
					"exynos8890-dvfs-g3d");
	if (IS_ERR(domain))
		return PTR_ERR(domain);
	ret = exynos8890_g3d_get_switch_clks(domain, hws);
	if (ret)
		goto err_release;
	ret = exynos8890_dvfs_publish(ctx, domain, CLK_DVFS_G3D);
	if (ret)
		goto err_release;
	return 0;

err_release:
	exynos8890_dvfs_release(domain);
	return ret;
}
