// SPDX-License-Identifier: GPL-2.0-only
/*
 * UFS PHY driver data for Samsung Exynos8890 SoC
 *
 * The Exynos8890 embedded UFS PHY is very close to Exynos7, but uses
 * a different PMU isolation bit, CDR lock status register and calibration
 * data. UFS protector handling belongs to the Exynos8890 host match data.
 */

#include "phy-samsung-ufs.h"

#define EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL		0x724
#define EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL_MASK	0x1
#define EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL_EN		BIT(0)

#define EXYNOS8890_EMBEDDED_COMBO_PHY_CDR_LOCK_STATUS	0x6e


/* Calibration for the Exynos8890 embedded UFS PHY. */
static const struct samsung_ufs_phy_cfg exynos8890_pre_init_cfg[] = {
	PHY_COMN_REG_CFG(0x00f, 0xfa, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x010, 0x82, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x011, 0x1e, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x012, 0x80, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x017, 0x94, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x034, 0x31, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x40, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x038, 0x3f, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x049, 0x00, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x04a, 0x10, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x04c, 0x5b, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x05c, 0x14, PWR_MODE_ANY),
	END_UFS_PHY_CFG
};

/* Calibration immediately before the negotiated HS power-mode change. */
static const struct samsung_ufs_phy_cfg exynos8890_pre_pwr_hs_cfg[] = {
	PHY_TRSV_REG_CFG(0x035, 0x40, PWR_MODE_HS_G1_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x40, PWR_MODE_HS_G2_ANY),
	PHY_TRSV_REG_CFG(0x034, 0x32, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x42, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x037, 0x43, PWR_MODE_HS_G3_ANY),
	END_UFS_PHY_CFG
};

static const struct samsung_ufs_phy_cfg exynos8890_post_pwr_hs_cfg[] = {
	PHY_TRSV_REG_CFG(0x049, 0x02, PWR_MODE_HS_G2_ANY),
	PHY_TRSV_REG_CFG(0x04a, 0x37, PWR_MODE_HS_G2_ANY),
	PHY_TRSV_REG_CFG(0x049, 0x02, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x04a, 0x37, PWR_MODE_HS_G3_ANY),
	END_UFS_PHY_CFG
};

static const struct samsung_ufs_phy_cfg *exynos8890_ufs_phy_cfgs[CFG_TAG_MAX] = {
	[CFG_PRE_INIT]		= exynos8890_pre_init_cfg,
	[CFG_PRE_PWR_HS]	= exynos8890_pre_pwr_hs_cfg,
	[CFG_POST_PWR_HS]	= exynos8890_post_pwr_hs_cfg,
};

static const struct samsung_ufs_phy_cfg exynos8890_pre_h8_exit_cfg[] = {
	PHY_COMN_REG_CFG(0x00f, 0xfa, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x010, 0x82, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x011, 0x1e, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x012, 0x80, PWR_MODE_ANY),
	PHY_COMN_REG_CFG(0x017, 0x94, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x034, 0x31, PWR_MODE_HS_G1_ANY),
	PHY_TRSV_REG_CFG(0x034, 0x31, PWR_MODE_HS_G2_ANY),
	PHY_TRSV_REG_CFG(0x034, 0x32, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x40, PWR_MODE_HS_G1_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x40, PWR_MODE_HS_G2_ANY),
	PHY_TRSV_REG_CFG(0x035, 0x42, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x037, 0x43, PWR_MODE_HS_G3_ANY),
	PHY_TRSV_REG_CFG(0x038, 0x3f, PWR_MODE_ANY),
	PHY_TRSV_REG_CFG(0x049, 0x02, PWR_MODE_HS_ANY),
	PHY_TRSV_REG_CFG(0x04a, 0x37, PWR_MODE_HS_ANY),
	PHY_TRSV_REG_CFG(0x04c, 0x5b, PWR_MODE_HS_ANY),
	PHY_TRSV_REG_CFG(0x05c, 0x14, PWR_MODE_HS_ANY),
	END_UFS_PHY_CFG
};

static const struct samsung_ufs_phy_cfg *exynos8890_ufs_hibern8_cfgs[] = {
	[CFG_PRE_HIBERN8_EXIT] = exynos8890_pre_h8_exit_cfg,
};

static const char * const exynos8890_ufs_phy_clks[] = {
	"ref_clk", "rx0_symbol_clk", "tx0_symbol_clk",
};

static bool exynos8890_ufs_phy_is_cfg_valid(struct samsung_ufs_phy *phy,
					    const struct samsung_ufs_phy_cfg *cfg)
{
	u8 mode = cfg->desc & MD_MASK;
	u8 series = (cfg->desc >> 2) & SR_MASK;
	u8 gear = (cfg->desc >> 4) & GR_MASK;
	u8 active_series;

	if (cfg->desc == PWR_MODE_ANY)
		return true;

	if (phy->mode != PHY_MODE_UFS_HS_A &&
	    phy->mode != PHY_MODE_UFS_HS_B)
		return false;

	if (mode != PWR_DESC_ANY && mode != PWR_DESC_HS)
		return false;

	active_series = phy->mode == PHY_MODE_UFS_HS_A ?
			PWR_DESC_SER_A : PWR_DESC_SER_B;
	if (series != PWR_DESC_ANY && series != active_series)
		return false;

	return gear == PWR_DESC_ANY || gear == phy->submode;
}

const struct samsung_ufs_phy_drvdata exynos8890_ufs_phy = {
	.cfgs = exynos8890_ufs_phy_cfgs,
	.cfgs_hibern8 = exynos8890_ufs_hibern8_cfgs,
	.isol = {
		.offset = EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL,
		.mask = EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL_MASK,
		.en = EXYNOS8890_EMBEDDED_COMBO_PHY_CTRL_EN,
	},
	.clk_list = exynos8890_ufs_phy_clks,
	.num_clks = ARRAY_SIZE(exynos8890_ufs_phy_clks),
	.cdr_lock_status_offset = EXYNOS8890_EMBEDDED_COMBO_PHY_CDR_LOCK_STATUS,
	.is_cfg_valid = exynos8890_ufs_phy_is_cfg_valid,
	.wait_for_cdr = samsung_ufs_phy_wait_for_lock_acq,
};
