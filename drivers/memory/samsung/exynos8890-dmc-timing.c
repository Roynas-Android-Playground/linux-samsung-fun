// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 LPDDR4 timing and PHY programming
 *
 * The register sequence and bit layouts originate from Samsung's Exynos8890
 * PWRCAL implementation.  This port deliberately contains no PWRCAL clock,
 * DFS, or register-access dependency: all MMIO is resolved through the single
 * DMC owner in exynos8890-dmc.c and all characterized data comes from the
 * immutable calibration provider.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/soc/samsung/exynos8890-calibration.h>

#include "exynos8890-dmc-internal.h"

#ifndef MHZ
#define MHZ		1000000ULL
#endif


#define __SMC_ALL				(DMC_MISC_CCORE_BASE + 0x8000)
#define __PHY_ALL				(DMC_MISC_CCORE_BASE + 0x4000)
#define __DMC_MISC_ALL			(DMC_MISC_CCORE_BASE + 0x0000)

#define ModeRegAddr				(__SMC_ALL + 0x0000)
#define MprMrCtl				(__SMC_ALL + 0x0004)
#define ModeRegWrData			(__SMC_ALL + 0x0008)

#define DramTiming0_0			(__SMC_ALL + 0x0058)
#define DramTiming1_0			(__SMC_ALL + 0x005C)
#define DramTiming2_0			(__SMC_ALL + 0x0060)
#define DramTiming3_0			(__SMC_ALL + 0x0064)
#define DramTiming4_0			(__SMC_ALL + 0x0068)
#define DramTiming5_0			(__SMC_ALL + 0x006C)
#define DramTiming6_0			(__SMC_ALL + 0x0070)
#define DramTiming7_0			(__SMC_ALL + 0x0074)
#define DramTiming8_0			(__SMC_ALL + 0x0078)
#define DramTiming9_0			(__SMC_ALL + 0x007C)
#define DramDerateTiming0_0		(__SMC_ALL + 0x0088)
#define DramDerateTiming1_0		(__SMC_ALL + 0x008C)
#define Dimm0AutoRefTiming1_0	(__SMC_ALL + 0x009C)
#define Dimm1AutoRefTiming1_0	(__SMC_ALL + 0x00A4)
#define AutoRefTiming2_0		(__SMC_ALL + 0x00A8)
#define PwrMgmtTiming0_0		(__SMC_ALL + 0x00B8)
#define PwrMgmtTiming1_0		(__SMC_ALL + 0x00BC)
#define PwrMgmtTiming2_0		(__SMC_ALL + 0x00C0)
#define PwrMgmtTiming3_0		(__SMC_ALL + 0x00C4)
#define TmrTrnInterval_0		(__SMC_ALL + 0x00C8)
#define DvfsTrnCtl_0			(__SMC_ALL + 0x00CC)
#define TrnTiming0_0			(__SMC_ALL + 0x00D0)
#define TrnTiming1_0			(__SMC_ALL + 0x00D4)
#define TrnTiming2_0			(__SMC_ALL + 0x00D8)
#define DFIDelay1_0				(__SMC_ALL + 0x00E0)
#define DFIDelay2_0				(__SMC_ALL + 0x00E4)

#define DramTiming0_1			(__SMC_ALL + 0x0108)
#define DramTiming1_1			(__SMC_ALL + 0x010C)
#define DramTiming2_1			(__SMC_ALL + 0x0110)
#define DramTiming3_1			(__SMC_ALL + 0x0114)
#define DramTiming4_1			(__SMC_ALL + 0x0118)
#define DramTiming5_1			(__SMC_ALL + 0x011C)
#define DramTiming6_1			(__SMC_ALL + 0x0120)
#define DramTiming7_1			(__SMC_ALL + 0x0124)
#define DramTiming8_1			(__SMC_ALL + 0x0128)
#define DramTiming9_1			(__SMC_ALL + 0x012C)
#define DramDerateTiming0_1		(__SMC_ALL + 0x0138)
#define DramDerateTiming1_1		(__SMC_ALL + 0x013C)
#define Dimm0AutoRefTiming1_1	(__SMC_ALL + 0x014C)
#define Dimm1AutoRefTiming1_1	(__SMC_ALL + 0x0154)
#define AutoRefTiming2_1		(__SMC_ALL + 0x0158)
#define PwrMgmtTiming0_1		(__SMC_ALL + 0x0168)
#define PwrMgmtTiming1_1		(__SMC_ALL + 0x016C)
#define PwrMgmtTiming2_1		(__SMC_ALL + 0x0170)
#define PwrMgmtTiming3_1		(__SMC_ALL + 0x0174)
#define TmrTrnInterval_1		(__SMC_ALL + 0x0178)
#define DvfsTrnCtl_1			(__SMC_ALL + 0x017C)
#define TrnTiming0_1			(__SMC_ALL + 0x0180)
#define TrnTiming1_1			(__SMC_ALL + 0x0184)
#define TrnTiming2_1			(__SMC_ALL + 0x0188)
#define DFIDelay1_1				(__SMC_ALL + 0x0190)
#define DFIDelay2_1				(__SMC_ALL + 0x0194)

#define SMC_TmrTrnCtl			(__SMC_ALL + 0x01D0)
#define SMC_TmrTrnCtl_CH0		(SMC0_BASE + 0x01D0)
#define SMC_TmrTrnCtl_CH1		(SMC1_BASE + 0x01D0)
#define SMC_TmrTrnCtl_CH2		(SMC2_BASE + 0x01D0)
#define SMC_TmrTrnCtl_CH3		(SMC3_BASE + 0x01D0)
#define SMC_TrnStatus_CH0		(SMC0_BASE + 0x01D8)
#define SMC_TrnStatus_CH1		(SMC1_BASE + 0x01D8)
#define SMC_TrnStatus_CH2		(SMC2_BASE + 0x01D8)
#define SMC_TrnStatus_CH3		(SMC3_BASE + 0x01D8)
#define SMC_PwrMgmtMode_CH0		(SMC0_BASE + 0x0238)
#define SMC_PwrMgmtMode_CH1		(SMC1_BASE + 0x0238)
#define SMC_PwrMgmtMode_CH2		(SMC2_BASE + 0x0238)
#define SMC_PwrMgmtMode_CH3		(SMC3_BASE + 0x0238)


#define DMC_MISC_CON0			(__DMC_MISC_ALL + 0x0014)
#define DMC_MISC_CON1			(__DMC_MISC_ALL + 0x003C)
#define DMC_MISC_CON1_CH0		(DMC_MISC0_BASE + 0x003C)
#define DMC_MISC_CON1_CH1		(DMC_MISC1_BASE + 0x003C)
#define DMC_MISC_CON1_CH2		(DMC_MISC2_BASE + 0x003C)
#define DMC_MISC_CON1_CH3		(DMC_MISC3_BASE + 0x003C)
#define MRS_DATA1				(__DMC_MISC_ALL + 0x0054)
#define MRS_DATA1_CH0			(DMC_MISC0_BASE + 0x0054)
#define MRS_DATA1_CH1			(DMC_MISC1_BASE + 0x0054)
#define MRS_DATA1_CH2			(DMC_MISC2_BASE + 0x0054)
#define MRS_DATA1_CH3			(DMC_MISC3_BASE + 0x0054)

#define CG_CTRL_VAL_DDRPHY0		(CMU_MIF0_BASE + 0x0A08)
#define CG_CTRL_VAL_DDRPHY1		(CMU_MIF1_BASE + 0x0A08)
#define CG_CTRL_VAL_DDRPHY2		(CMU_MIF2_BASE + 0x0A08)
#define CG_CTRL_VAL_DDRPHY3		(CMU_MIF3_BASE + 0x0A08)

#define CG_CTRL_MAN_DDRPHY0		(CMU_MIF0_BASE + 0x1A08)
#define CG_CTRL_MAN_DDRPHY1		(CMU_MIF1_BASE + 0x1A08)
#define CG_CTRL_MAN_DDRPHY2		(CMU_MIF2_BASE + 0x1A08)
#define CG_CTRL_MAN_DDRPHY3		(CMU_MIF3_BASE + 0x1A08)

#define PMU_DREX_CALIBRATION1	(PMU_ALIVE_BASE + 0x09a4)
#define PMU_DREX_CALIBRATION2	(PMU_ALIVE_BASE + 0x09a8)
#define PMU_DREX_CALIBRATION3	(PMU_ALIVE_BASE + 0x09ac)

enum mif_timing_set_idx {
	MIF_TIMING_SET_0,
	MIF_TIMING_SET_1
};

enum smc_dram_mode_register {
	DRAM_MR0,
	DRAM_MR1,
	DRAM_MR2,
	DRAM_MR3,
	DRAM_MR4,
	DRAM_MR5,
	DRAM_MR6,
	DRAM_MR7,
	DRAM_MR8,
	DRAM_MR9,
	DRAM_MR10,
	DRAM_MR11,
	DRAM_MR12,
	DRAM_MR13,
	DRAM_MR14,
	DRAM_MR15,
	DRAM_MR16,
	DRAM_MR17,
	DRAM_MR18,
	DRAM_MR19,
	DRAM_MR20,
	DRAM_MR21,
	DRAM_MR22,
	DRAM_MR23,
	DRAM_MR24,
	DRAM_MR25,
	DRAM_MR32 = 32,
	DRAM_MR40 = 40,
};

enum timing_parameter_column {
	DramTiming0,
	DramTiming1,
	DramTiming2,
	DramTiming3,
	DramTiming4,
	DramTiming5,
	DramTiming6,
	DramTiming7,
	DramTiming8,
	DramTiming9,
	DramDerateTiming0,
	DramDerateTiming1,
	Dimm0AutoRefTiming1,
	Dimm1AutoRefTiming1,
	AutoRefTiming2,
	PwrMgmtTiming0,
	PwrMgmtTiming1,
	PwrMgmtTiming2,
	PwrMgmtTiming3,
	TmrTrnInterval,
	DFIDelay1,
	DFIDelay2,
	DvfsTrnCtl,
	TrnTiming0,
	TrnTiming1,
	TrnTiming2,
	DVFSn_CON0,
	DVFSn_CON1,
	DVFSn_CON2,
	DVFSn_CON3,
	DVFSn_CON4,
	DirectCmd_MR1,
	DirectCmd_MR2,
	DirectCmd_MR3,
	DirectCmd_MR11,
	DirectCmd_MR12,
	DirectCmd_MR14,
	DirectCmd_MR22,

	num_of_g_smc_dfs_table_column = TrnTiming2 - DramTiming0 + 1,
	num_of_g_phy_dfs_table_column = DVFSn_CON4 - DVFSn_CON0 + 1,
	num_of_g_dram_dfs_table_column = DirectCmd_MR22 - DirectCmd_MR1 + 1,
	num_of_dram_parameter = num_of_g_smc_dfs_table_column + num_of_g_phy_dfs_table_column + num_of_g_dram_dfs_table_column,
};

/******************************************************************************
 *
 *
 *
 *****************************************************************************/
typedef volatile unsigned int rw_bf_t;
typedef const unsigned int ro_bf_t;
typedef const unsigned int wo_bf_t;

typedef union {
	volatile unsigned int data;
	struct {
		rw_bf_t rdlvl_periodic_incr_adj   :	(6 - 0 + 1);
		rw_bf_t glvl_periodic_incr_adj    :	(13 - 7 + 1);
		rw_bf_t glvl_start_adj            :	(15 - 14 + 1);
		rw_bf_t rdlvl_pass_adj            :	(19 - 16 + 1);
		rw_bf_t rdlvl_incr_adj            :	(26 - 20 + 1);
		ro_bf_t reserved_27               :	(27 - 27 + 1);
		rw_bf_t rdlvl_start_adj           :	(31 - 28 + 1);
	} bitfield;
} phy_cal_con1_t;                    //0x0008

typedef union {
	volatile unsigned int data;
	struct {
		rw_bf_t dvfs0_readadj             : (3 - 0 + 1);
		rw_bf_t dvfs0_readduradj          : (7 - 4 + 1);
		rw_bf_t dvfs0_gateadj             : (11 - 8 + 1);
		rw_bf_t dvfs0_gateduradj          : (15 - 12 + 1);
		rw_bf_t dvfs0_shgate              : (16 - 16 + 1);
		rw_bf_t dvfs0_dgatelvl_en         : (17 - 17 + 1);
		ro_bf_t reserved_18_23            : (23 - 18 + 1);
		rw_bf_t dvfs0_pulld_dqs           : (25 - 24 + 1);
		ro_bf_t reserved_26_28            : (28 - 26 + 1);
		rw_bf_t dvfs0_rodt_disable        : (29 - 29 + 1);
		rw_bf_t dvfs0_dfdqs               : (30 - 30 + 1);
		rw_bf_t dvfs0_cmosrcv             : (31 - 31 + 1);
	} bitfield;
} phy_dvfs0_con2_t;                  //0x00CC

typedef union {
	volatile unsigned int data;
	struct {
		rw_bf_t dvfs0_zq_ds0_pdds         :	(2 - 0 + 1);
		rw_bf_t dvfs0_zq_ds0_dds          :	(5 - 3 + 1);
		rw_bf_t dvfs0_zq_ds1_pdds         :	(8 - 6 + 1);
		rw_bf_t dvfs0_zq_ds1_dds          : (11 - 9 + 1);
		rw_bf_t dvfs0_zq_mode_pdds        :	(14 - 12 + 1);
		rw_bf_t dvfs0_zq_mode_dds         :	(17 - 15 + 1);
		ro_bf_t reserved_18_19            :	(19 - 18 + 1);
		rw_bf_t dvfs0_ds0_vref            :	(25 - 20 + 1);
		rw_bf_t dvfs0_ds1_vref            :	(31 - 26 + 1);
	} bitfield;
} phy_dvfs0_con3_t;

typedef union {
	volatile unsigned int data;
	struct {
		rw_bf_t dvfs0_phyupd_req_cycle    :	(5 - 0 + 1);
		ro_bf_t reserved_6_7              :	(7 - 6 + 1);
		rw_bf_t dvfs0_mcupd_req_cycle     :	(13 - 8 + 1);
		ro_bf_t reserved_14_15            :	(15 - 14 + 1);
		rw_bf_t dvfs0_zq_ds0_term         :	(18 - 16 + 1);
		rw_bf_t dvfs0_zq_ds1_term         :	(21 - 19 + 1);
		ro_bf_t reserved_22_31			  :	(31 - 22 + 1);
	} bitfield;
} phy_dvfs0_con4_t;

typedef union {
	volatile unsigned int data;
	struct {
		ro_bf_t reserved_0                :	(0 - 0 + 1);
		rw_bf_t zq_manual_str             :	(1 - 1 + 1);
		rw_bf_t zq_manual_mode            :	(3 - 2 + 1);
		rw_bf_t zq_udt_dly                :	(11 - 4 + 1);
		rw_bf_t zq_force_impp             :	(14 - 12 + 1);
		rw_bf_t zq_force_impn             :	(17 - 15 + 1);
		rw_bf_t zq_clk_div_en             :	(18 - 18 + 1);
		rw_bf_t zq_mode_noterm            :	(19 - 19 + 1);
		rw_bf_t zq_rgddr3                 :	(20 - 20 + 1);
		rw_bf_t zq_mode_term              :	(23 - 21 + 1);
		rw_bf_t zq_mode_dds               :	(26 - 24 + 1);
		rw_bf_t zq_clk_en                 :	(27 - 27 + 1);
		rw_bf_t zq_mode_pdds              :	(30 - 28 + 1);
		rw_bf_t zq_mode_lp4               :	(31 - 31 + 1);
	} bitfield;
} phy_zq_con0_t;

/******************************************************************************
 *
 * ECT related resource
 *
 *****************************************************************************/
struct smc_dfs_table {
	unsigned int DramTiming0;
	unsigned int DramTiming1;
	unsigned int DramTiming2;
	unsigned int DramTiming3;
	unsigned int DramTiming4;
	unsigned int DramTiming5;
	unsigned int DramTiming6;
	unsigned int DramTiming7;
	unsigned int DramTiming8;
	unsigned int DramTiming9;
	unsigned int DramDerateTiming0;
	unsigned int DramDerateTiming1;
	unsigned int Dimm0AutoRefTiming1;
	unsigned int Dimm1AutoRefTiming1;
	unsigned int AutoRefTiming2;
	unsigned int PwrMgmtTiming0;
	unsigned int PwrMgmtTiming1;
	unsigned int PwrMgmtTiming2;
	unsigned int PwrMgmtTiming3;
	unsigned int TmrTrnInterval;
	unsigned int DFIDelay1;
	unsigned int DFIDelay2;
	unsigned int DvfsTrnCtl;
	unsigned int TrnTiming0;
	unsigned int TrnTiming1;
	unsigned int TrnTiming2;
};

struct phy_dfs_table {
	unsigned int DVFSn_CON0;
	unsigned int DVFSn_CON1;
	unsigned int DVFSn_CON2;
	unsigned int DVFSn_CON3;
	unsigned int DVFSn_CON4;
};

struct dram_dfs_table {
	unsigned int DirectCmd_MR1;
	unsigned int DirectCmd_MR2;
	unsigned int DirectCmd_MR3;
	unsigned int DirectCmd_MR11;
	unsigned int DirectCmd_MR12;
	unsigned int DirectCmd_MR14;
	unsigned int DirectCmd_MR22;
};

/******************************************************************************
 *
 * SMC related resource
 *
 *****************************************************************************/
typedef enum {
	SMC_BYTE_0,
	SMC_BYTE_1,
	SMC_BYTE_ALL,
} smc_byte_t;

typedef enum {
	SMC_CH_0,
	SMC_CH_1,
	SMC_CH_2,
	SMC_CH_3,
	SMC_CH_ALL,
} smc_ch_t;

struct smc_timing_params_t {
	unsigned int DramTiming0;
	unsigned int DramTiming1;
	unsigned int DramTiming2;
	unsigned int DramTiming3;
	unsigned int DramTiming4;
	unsigned int DramTiming5;
	unsigned int DramTiming6;
	unsigned int DramTiming7;
	unsigned int DramTiming8;
	unsigned int DramTiming9;
	unsigned int DramDerateTiming0;
	unsigned int DramDerateTiming1;
	unsigned int Dimm0AutoRefTiming1;
	unsigned int Dimm1AutoRefTiming1;
	unsigned int AutoRefTiming2;
	unsigned int PwrMgmtTiming0;
	unsigned int PwrMgmtTiming1;
	unsigned int PwrMgmtTiming2;
	unsigned int PwrMgmtTiming3;
	unsigned int TmrTrnInterval;
	unsigned int DFIDelay1;
	unsigned int DFIDelay2;
	unsigned int DvfsTrnCtl;
	unsigned int TrnTiming0;
	unsigned int TrnTiming1;
	unsigned int TrnTiming2;
};

struct smc_qos_params_t {
	unsigned int Scheduler[5];
	unsigned int ReadToken;
	unsigned int WriteToken;
	unsigned int HurryReadToken[3];
	unsigned int HurryWriteToken[3];
};

struct smc_page_policy_params_t {
	unsigned char autopchgdis;
	unsigned char level3pchgtoadaptdis;
	unsigned char level3pchgdis;
	unsigned char level2pchgdis;
};

struct smc_pm_params_t {
	unsigned char pd_idle_threshold;
	unsigned char flush_wr_sre;
	unsigned char hw_sre;
	unsigned char pchg_pd;
	unsigned char global_clk_gate;
	unsigned char q_channel;
	unsigned char phy_cg;
};

struct smc_dfi_params_t {
	unsigned char freq_ratio;
	unsigned char t_dram_clk_enable;
	unsigned char t_dram_clk_disable;
};

struct smc_thermal_params_t {
	unsigned char rate;
	unsigned char poll;
	unsigned char range;
	unsigned char derate;
};

struct smc_aref_params_t {
	unsigned char t_refi;
	unsigned char pb_aref;
	unsigned char payback_sref;
	unsigned char staggered_aref;
};

struct smc_trn_lpi_t {
	unsigned int	enable;
	unsigned char	time;
};

struct smc_trn_periodic_t {
	unsigned int	enable;
	unsigned char	offset;
};

struct smc_trn_params_t {
	struct smc_trn_lpi_t			lpi;
	struct smc_trn_periodic_t		periodic;
};

struct smc_config_t {
	unsigned int					magic;
	unsigned char					LP4CaSwizzleEn;
	unsigned char					LP4DqSwizzleEn;
	unsigned char					cmd_cancel;
	unsigned char					TrrdRankToRankEn;
	unsigned char					DbiEn;
	unsigned char					DataMaskEn;
	unsigned char					t_zqcal;
	struct smc_trn_params_t			trn;
	struct smc_aref_params_t		aref;
	struct smc_thermal_params_t	thermal;
	struct smc_dfi_params_t			dfi;
	struct smc_pm_params_t			pm;
	struct smc_page_policy_params_t page_policy;
	struct smc_qos_params_t			qos;
	struct smc_timing_params_t		timing;
};

/******************************************************************************
 *
 * PHY related resource
 *
 *****************************************************************************/
#define NUM_OF_TRN_OFFSET_INFO			(2)
#define NUM_OF_TRN_DLL_INFO				(1)
#define NUM_OF_TRN_GATE_INFO			(4)
#define NUM_OF_TRN_RD_DESKEW_INFO		(9)
#define NUM_OF_TRN_RD_DESKEWQ_INFO		(2)
#define NUM_OF_TRN_WR_DESKEW_INFO		(9)

#define PHY_OFFSET_CONFIG_MAGIC			(0x74664f40)	/* @Ofst */
#define PHY_CAL_CON1_OFFSET				(0x0008)
#define PHY_OFFSETC_CON1_OFFSET			(0x0040)


#define PHY_DVFS_CON					(__PHY_ALL + 0x00B8)
#define PHY_DVFS_CON_CH0				(LPDDR4_PHY0_BASE + 0x00B8)
#define PHY_DVFS0_CON0_CH0				(LPDDR4_PHY0_BASE + 0x00BC)
#define PHY_DVFS0_CON1_CH0				(LPDDR4_PHY0_BASE + 0x00C4)
#define PHY_DVFS0_CON2_CH0				(LPDDR4_PHY0_BASE + 0x00CC)
#define PHY_DVFS0_CON3_CH0				(LPDDR4_PHY0_BASE + 0x00D4)
#define PHY_DVFS0_CON4_CH0				(LPDDR4_PHY0_BASE + 0x00DC)

#define PHY_DVFS_CON_CH1				(LPDDR4_PHY1_BASE + 0x00B8)
#define PHY_DVFS0_CON0_CH1				(LPDDR4_PHY1_BASE + 0x00BC)
#define PHY_DVFS0_CON1_CH1				(LPDDR4_PHY1_BASE + 0x00C4)
#define PHY_DVFS0_CON2_CH1				(LPDDR4_PHY1_BASE + 0x00CC)
#define PHY_DVFS0_CON3_CH1				(LPDDR4_PHY1_BASE + 0x00D4)
#define PHY_DVFS0_CON4_CH1				(LPDDR4_PHY1_BASE + 0x00DC)

#define PHY_DVFS_CON_CH2				(LPDDR4_PHY2_BASE + 0x00B8)
#define PHY_DVFS0_CON0_CH2				(LPDDR4_PHY2_BASE + 0x00BC)
#define PHY_DVFS0_CON1_CH2				(LPDDR4_PHY2_BASE + 0x00C4)
#define PHY_DVFS0_CON2_CH2				(LPDDR4_PHY2_BASE + 0x00CC)
#define PHY_DVFS0_CON3_CH2				(LPDDR4_PHY2_BASE + 0x00D4)
#define PHY_DVFS0_CON4_CH2				(LPDDR4_PHY2_BASE + 0x00DC)

#define PHY_DVFS_CON_CH3				(LPDDR4_PHY3_BASE + 0x00B8)
#define PHY_DVFS0_CON0_CH3				(LPDDR4_PHY3_BASE + 0x00BC)
#define PHY_DVFS0_CON1_CH3				(LPDDR4_PHY3_BASE + 0x00C4)
#define PHY_DVFS0_CON2_CH3				(LPDDR4_PHY3_BASE + 0x00CC)
#define PHY_DVFS0_CON3_CH3				(LPDDR4_PHY3_BASE + 0x00D4)
#define PHY_DVFS0_CON4_CH3				(LPDDR4_PHY3_BASE + 0x00DC)

#define PHY_DVFS1_CON0_CH0				(LPDDR4_PHY0_BASE + 0x00C0)
#define PHY_DVFS1_CON1_CH0				(LPDDR4_PHY0_BASE + 0x00C8)
#define PHY_DVFS1_CON2_CH0				(LPDDR4_PHY0_BASE + 0x00D0)
#define PHY_DVFS1_CON3_CH0				(LPDDR4_PHY0_BASE + 0x00D8)
#define PHY_DVFS1_CON4_CH0				(LPDDR4_PHY0_BASE + 0x00E0)

#define PHY_DVFS1_CON0_CH1				(LPDDR4_PHY1_BASE + 0x00C0)
#define PHY_DVFS1_CON1_CH1				(LPDDR4_PHY1_BASE + 0x00C8)
#define PHY_DVFS1_CON2_CH1				(LPDDR4_PHY1_BASE + 0x00D0)
#define PHY_DVFS1_CON3_CH1				(LPDDR4_PHY1_BASE + 0x00D8)
#define PHY_DVFS1_CON4_CH1				(LPDDR4_PHY1_BASE + 0x00E0)

#define PHY_DVFS1_CON0_CH2				(LPDDR4_PHY2_BASE + 0x00C0)
#define PHY_DVFS1_CON1_CH2				(LPDDR4_PHY2_BASE + 0x00C8)
#define PHY_DVFS1_CON2_CH2				(LPDDR4_PHY2_BASE + 0x00D0)
#define PHY_DVFS1_CON3_CH2				(LPDDR4_PHY2_BASE + 0x00D8)
#define PHY_DVFS1_CON4_CH2				(LPDDR4_PHY2_BASE + 0x00E0)

#define PHY_DVFS1_CON0_CH3				(LPDDR4_PHY3_BASE + 0x00C0)
#define PHY_DVFS1_CON1_CH3				(LPDDR4_PHY3_BASE + 0x00C8)
#define PHY_DVFS1_CON2_CH3				(LPDDR4_PHY3_BASE + 0x00D0)
#define PHY_DVFS1_CON3_CH3				(LPDDR4_PHY3_BASE + 0x00D8)
#define PHY_DVFS1_CON4_CH3				(LPDDR4_PHY3_BASE + 0x00E0)

typedef enum {
	PHY_BYTE_0,
	PHY_BYTE_1,
	PHY_BYTE_ALL,
} phy_byte_t;

typedef enum {
	PHY_CH_0,
	PHY_CH_1,
	PHY_CH_2,
	PHY_CH_3,
	PHY_CH_ALL,
} phy_ch_t;

typedef enum {
	PHY_RANK_0,
	PHY_RANK_1,
	PHY_RANK_ALL,
} phy_rank_t;

typedef unsigned short phy_trn_dll_size_t;
typedef unsigned char phy_trn_info_size_t;

struct phy_trn_gate_info_t {
	phy_trn_info_size_t				cycle[PHY_BYTE_ALL];
	phy_trn_info_size_t				center[PHY_BYTE_ALL];
};

struct phy_trn_read_dqs_info_t {
	phy_trn_info_size_t				left[PHY_BYTE_ALL];
	phy_trn_info_size_t				center[PHY_BYTE_ALL];
};

struct phy_trn_read_info_t {
	phy_trn_info_size_t				deskewc[NUM_OF_TRN_RD_DESKEW_INFO][PHY_BYTE_ALL];
	phy_trn_info_size_t				deskewl[NUM_OF_TRN_RD_DESKEW_INFO][PHY_BYTE_ALL];
	struct phy_trn_read_dqs_info_t		dqs;
};

struct phy_trn_write_info_t {
	phy_trn_info_size_t				deskewc[NUM_OF_TRN_WR_DESKEW_INFO][PHY_BYTE_ALL];
	phy_trn_info_size_t				deskewl[NUM_OF_TRN_WR_DESKEW_INFO][PHY_BYTE_ALL];
};

struct phy_trn_offset_info_t {
	phy_trn_info_size_t				read[PHY_BYTE_ALL];
	phy_trn_info_size_t				write[PHY_BYTE_ALL];
};

struct phy_trn_data_t {
	phy_trn_dll_size_t					dll;
	struct phy_trn_offset_info_t		offset;
	struct phy_trn_gate_info_t			gate[PHY_RANK_ALL];
	struct phy_trn_read_info_t			read;
	struct phy_trn_write_info_t		write[PHY_RANK_ALL];
};

struct phy_vwm_t {
	signed char						left;
	signed char						right;
};

struct phy_vwm_params_t {
	struct phy_vwm_t					read[PHY_CH_ALL][PHY_BYTE_ALL];
	struct phy_vwm_t					write[PHY_CH_ALL][PHY_BYTE_ALL];
	struct phy_vwm_t					command[PHY_CH_ALL];
};

struct phy_adjust_params_t {
	unsigned char						rdlvl_start_adj;
	unsigned char						rdlvl_incr_adj;
	unsigned char						rdlvl_periodic_incr_adj;
	unsigned char						rdlvl_pass_adj;
	unsigned char						glvl_start_adj;
	unsigned char						glvl_periodic_incr_adj;
	unsigned char						gateduradj;
	unsigned char						gateadj;
	unsigned char						readduradj;
	unsigned char						readadj;
	unsigned char						read_width;
};

struct phy_pd_params_t {
	unsigned char						command_io;
	unsigned char						data_io;
	unsigned char						data_clk;
	unsigned char						phy_logic_clk;
	unsigned char						mdll_clk;
	unsigned char						zq_vref[4][2];
};

struct phy_io_params_t {
	unsigned char						ca_pdds[PHY_CH_ALL];
	unsigned char						ca_dds[PHY_CH_ALL];
	unsigned char						dq_pdds[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						dq_dds[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						reset_dds;
	unsigned char						cke_dds[PHY_RANK_ALL];
	unsigned char						term[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						vref[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						offsetr[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						offsetw[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned int						ext_dds;
};

struct phy_dvfs_params_t {
	unsigned int						DVFSn_CON0;
	unsigned int						DVFSn_CON1;
	unsigned int						DVFSn_CON2;
	unsigned int						DVFSn_CON3;
	unsigned int						DVFSn_CON4;
};

struct phy_offset_config_t {
	unsigned int						magic;
	unsigned short						lock[PHY_CH_ALL];
	signed char							read[PHY_CH_ALL][PHY_BYTE_ALL];
	signed char							write[PHY_CH_ALL][PHY_BYTE_ALL];
	unsigned char						dq[PHY_CH_ALL];
	signed char							gate[PHY_CH_ALL][PHY_BYTE_ALL];
	signed char							command[PHY_CH_ALL];
	signed char							oen[PHY_CH_ALL][PHY_BYTE_ALL];
};

struct phy_config_t {
	unsigned int						magic;
	unsigned short						freq;
	unsigned char						training;
	unsigned char						update_mode;
	unsigned char						update_time;
	unsigned char						update_range;
	unsigned char						update_interval;
	unsigned char						write_postamble;
	unsigned char						otf;
	unsigned char						bl;
	unsigned char						rl;
	unsigned char						gate_read_check;
	unsigned char						dgate;
	unsigned char						freq_offset_calc;
	unsigned char						vtc_enable;
	unsigned char						ca_swap;
	unsigned char						phy_update_request_cycle;
	unsigned char						mc_update_request_cycle;
	unsigned char						dvfs_wait_cycle;
	unsigned char						update_ack_cycle;
	unsigned char						rank_en;
	unsigned char						clkm_cg_en_sw;
	unsigned char						fsbst;
	unsigned char						ctrl_ref;
	struct phy_vwm_params_t				vwm;
	struct phy_pd_params_t				pd;
	struct phy_io_params_t				io;
	struct phy_adjust_params_t			adjust;
	struct phy_dvfs_params_t			dvfs[2];
	struct phy_trn_data_t				trn_data[PHY_CH_ALL];
};

/******************************************************************************
 *
 * LPDDR4 related resource
 *
 *****************************************************************************/
typedef enum {
	LPDDR4_CH_0,
	LPDDR4_CH_1,
	LPDDR4_CH_2,
	LPDDR4_CH_3,
	LPDDR4_CH_ALL,
} lpddr4_ch_t;

typedef enum {
	LPDDR4_RANK_0,
	LPDDR4_RANK_1,
	LPDDR4_RANK_ALL,
} lpddr4_rank_t;

struct read_pattern_t {
	unsigned char data[2];
	unsigned char invert[2];
};

struct write_pattern_t {
	unsigned char data[16];
	unsigned char mask[16];
};

struct mr_params_t {
	unsigned char reg1;
	unsigned char reg2;
	unsigned char reg3;
	unsigned char reg11[LPDDR4_CH_ALL][LPDDR4_RANK_ALL];
	unsigned char reg12[LPDDR4_CH_ALL][LPDDR4_RANK_ALL];
	unsigned char reg14[LPDDR4_CH_ALL][LPDDR4_RANK_ALL];
	unsigned char reg22[LPDDR4_CH_ALL][LPDDR4_RANK_ALL];
};

struct lpddr4_config_t {
	unsigned int magic;
	unsigned short freq;
	unsigned char rro;
	unsigned char dmd;
	unsigned char training;
	unsigned char manufacturer;
	struct mr_params_t mode;
	struct read_pattern_t read_pattern;
	struct write_pattern_t write_pattern;
};

/******************************************************************************
 *
 * VREF related resource
 *
 *****************************************************************************/
#define MAX_NUM_DVFS_LEVEL		(20)
struct vref_search_t {
	unsigned short min_step		:4;
	unsigned short max_step		:4;
	unsigned short search_step	:4;
	unsigned short trial		:4;
};

struct vref_info_t {
	unsigned char			vref;
	unsigned char			center;
	struct vref_search_t	search;
};

struct vref_config_t {
	unsigned char			num_of_level;
	struct vref_info_t		read;
	unsigned char			vref_read[PHY_CH_ALL][MAX_NUM_DVFS_LEVEL];
	struct vref_info_t		write;
	unsigned char			vref_write[PHY_CH_ALL][MAX_NUM_DVFS_LEVEL];
};

/******************************************************************************
 *
 *
 *
 *****************************************************************************/
struct drampara_config_t {
	struct smc_config_t smc;
	struct phy_config_t phy;
	struct lpddr4_config_t lpddr4;
	struct phy_offset_config_t phy_offset;
	struct vref_config_t vref;
};

/******************************************************************************
 *
 *
 *
 *****************************************************************************/
static const uintptr_t PHY_CAL_CON1[] = {
	(LPDDR4_PHY0_BASE + PHY_CAL_CON1_OFFSET),
	(LPDDR4_PHY1_BASE + PHY_CAL_CON1_OFFSET),
	(LPDDR4_PHY2_BASE + PHY_CAL_CON1_OFFSET),
	(LPDDR4_PHY3_BASE + PHY_CAL_CON1_OFFSET),
};

static const uintptr_t PHY_OFFSETC_CON1[] = {
	(LPDDR4_PHY0_BASE + PHY_OFFSETC_CON1_OFFSET),
	(LPDDR4_PHY1_BASE + PHY_OFFSETC_CON1_OFFSET),
	(LPDDR4_PHY2_BASE + PHY_OFFSETC_CON1_OFFSET),
	(LPDDR4_PHY3_BASE + PHY_OFFSETC_CON1_OFFSET),
};

static const uintptr_t PHY_ZQ_CON0[] = {
	(LPDDR4_PHY0_BASE + 0x03C8),
	(LPDDR4_PHY1_BASE + 0x03C8),
	(LPDDR4_PHY2_BASE + 0x03C8),
	(LPDDR4_PHY3_BASE + 0x03C8),
};


static const uintptr_t PHY_DVFS0_CON0[] = {PHY_DVFS0_CON0_CH0, PHY_DVFS0_CON0_CH1, PHY_DVFS0_CON0_CH2, PHY_DVFS0_CON0_CH3};
static const uintptr_t PHY_DVFS0_CON1[] = {PHY_DVFS0_CON1_CH0, PHY_DVFS0_CON1_CH1, PHY_DVFS0_CON1_CH2, PHY_DVFS0_CON1_CH3};
static const uintptr_t PHY_DVFS0_CON2[] = {PHY_DVFS0_CON2_CH0, PHY_DVFS0_CON2_CH1, PHY_DVFS0_CON2_CH2, PHY_DVFS0_CON2_CH3};
static const uintptr_t PHY_DVFS0_CON3[] = {PHY_DVFS0_CON3_CH0, PHY_DVFS0_CON3_CH1, PHY_DVFS0_CON3_CH2, PHY_DVFS0_CON3_CH3};
static const uintptr_t PHY_DVFS0_CON4[] = {PHY_DVFS0_CON4_CH0, PHY_DVFS0_CON4_CH1, PHY_DVFS0_CON4_CH2, PHY_DVFS0_CON4_CH3};

static const uintptr_t PHY_DVFS1_CON0[] = {PHY_DVFS1_CON0_CH0, PHY_DVFS1_CON0_CH1, PHY_DVFS1_CON0_CH2, PHY_DVFS1_CON0_CH3};
static const uintptr_t PHY_DVFS1_CON1[] = {PHY_DVFS1_CON1_CH0, PHY_DVFS1_CON1_CH1, PHY_DVFS1_CON1_CH2, PHY_DVFS1_CON1_CH3};
static const uintptr_t PHY_DVFS1_CON2[] = {PHY_DVFS1_CON2_CH0, PHY_DVFS1_CON2_CH1, PHY_DVFS1_CON2_CH2, PHY_DVFS1_CON2_CH3};
static const uintptr_t PHY_DVFS1_CON3[] = {PHY_DVFS1_CON3_CH0, PHY_DVFS1_CON3_CH1, PHY_DVFS1_CON3_CH2, PHY_DVFS1_CON3_CH3};
static const uintptr_t PHY_DVFS1_CON4[] = {PHY_DVFS1_CON4_CH0, PHY_DVFS1_CON4_CH1, PHY_DVFS1_CON4_CH2, PHY_DVFS1_CON4_CH3};


static struct smc_dfs_table *g_smc_dfs_table;
static struct phy_dfs_table *g_phy_dfs_table;
static struct dram_dfs_table *g_dram_dfs_table;
static unsigned long long *mif_freq_to_level;
static int num_mif_freq_to_level;
static unsigned int num_mif_timing_levels;
static unsigned long long query_key;

static const unsigned long long mif_freq_to_level_switch[] = {
	936 * MHZ,	/* BUS3_PLL SW 936 */
	528 * MHZ,	/* BUS0_PLL SW 528 */
};

static void *config_base;
static struct drampara_config_t *drampara_config;

/******************************************************************************
 *
 * @fn      exynos8890_dmc_get_dram_key
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static unsigned long long __maybe_unused exynos8890_dmc_get_dram_key(void)
{
	return query_key;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_get_dram_tdqs2dq
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static int exynos8890_dmc_get_dram_tdqs2dq(int ch, int rank, int idx,
					   unsigned int *tdqs2dq)
{
	int dq;
	int byte;

	if (drampara_config == NULL)
		return 0;

	dq = idx % 9;
	byte = idx / 9;

	*tdqs2dq = drampara_config->phy.trn_data[ch].write[rank].deskewl[dq][byte];

	return 1;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_get_dram_tdqsck
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static int exynos8890_dmc_get_dram_tdqsck(int ch, int rank, int byte,
					  unsigned int *tdqsck)
{
	if (drampara_config == NULL)
		return 0;

	*tdqsck = drampara_config->phy.trn_data[ch].gate[rank].cycle[byte] * drampara_config->phy.trn_data[ch].dll
			+ drampara_config->phy.trn_data[ch].gate[rank].center[byte];

	return 1;
}

/******************************************************************************
 *
 * @fn      dmc_misc_direct_dmc_enable
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static void __maybe_unused dmc_misc_direct_dmc_enable(int enable)
{
	exynos8890_dmc_write(DMC_MISC_CON0, (enable << 24) | (0x2 << 20));
}

/******************************************************************************
 *
 * @fn      smc_mode_register_write
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static void smc_mode_register_write(int mr, int op)
{
	exynos8890_dmc_write(ModeRegAddr, ((0x3 << 28) | (mr << 20)));
	exynos8890_dmc_write(ModeRegWrData, op);
	exynos8890_dmc_write(MprMrCtl, 0x10);
}

/******************************************************************************
 *
 * @fn      smc_mode_register_write_per_ch
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static void __maybe_unused smc_mode_register_write_per_ch(int ch, int mr,
						  int rank, int op)
{
	unsigned long modereg;
	unsigned long moderegwrdata;
	unsigned long mprmrctl;

	unsigned long base;

	switch (ch) {
	case 0:
		base = SMC0_BASE;
		break;
	case 1:
		base = SMC1_BASE;
		break;
	case 2:
		base = SMC2_BASE;
		break;
	case 3:
		base = SMC3_BASE;
		break;
	}
	modereg = base;
	moderegwrdata = (base + 0x8);
	mprmrctl = (base + 0x4);

	exynos8890_dmc_write(modereg, ((rank & 0x3) << 28) | (mr << 20));
	exynos8890_dmc_write(moderegwrdata, op);
	exynos8890_dmc_write(mprmrctl, 0x10);
}

/******************************************************************************
 *
 * @fn      convert_to_level
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static unsigned int convert_to_level(unsigned long long freq)
{
	int idx;
	int tablesize = num_mif_freq_to_level;

	for (idx = tablesize - 1; idx >= 0; idx--)
		if (freq <= mif_freq_to_level[idx])
			return (unsigned int)idx;

	return 0;
}

/******************************************************************************
 *
 * @fn      convert_to_level_switch
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static unsigned int convert_to_level_switch(unsigned long long freq)
{
	int idx;
	int tablesize = ARRAY_SIZE(mif_freq_to_level_switch);

	for (idx = tablesize - 1; idx >= 0; idx--)
		if (freq <= mif_freq_to_level_switch[idx])
			return (unsigned int)idx;

	return 0;
}

static int exynos8890_dmc_wait_training_idle(unsigned int channel)
{
	uintptr_t status = (uintptr_t)SMC_TrnStatus_CH0 + channel * SZ_1M;
	unsigned int timeout = 20000;
	u32 value;

	while (timeout--) {
		value = exynos8890_dmc_read(status);
		if (value == 0 || value == 8)
			return 0;
		udelay(1);
	}

	pr_err("Exynos8890 DMC channel %u training did not become idle\n",
	       channel);
	return -ETIMEDOUT;
}

static int exynos8890_dmc_quiesce_periodic_training(uintptr_t interval_reg)
{
	u32 interval = exynos8890_dmc_read(interval_reg);
	unsigned int channel;
	int ret;

	/* Stop new work, then prove every channel idle before changing any. */
	exynos8890_dmc_write(interval_reg, 0);
	for (channel = 0; channel < PHY_CH_ALL; channel++) {
		ret = exynos8890_dmc_wait_training_idle(channel);
		if (ret) {
			exynos8890_dmc_write(interval_reg, interval);
			return ret;
		}
	}

	return 0;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_program_timing
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
int exynos8890_dmc_program_timing(unsigned long target_mif_freq,
				  unsigned int timing_set_idx)
{
	int n;
	int byte;

	unsigned int uReg;

	unsigned int target_mif_level_idx, target_mif_level_switch_idx;
	unsigned int mr13;

	int gate_offset_adjust = 0;
	int new_gate_offset;
	unsigned short offsetc;

	phy_cal_con1_t cal_con1;
	phy_dvfs0_con2_t *dvfs0_con2;

	phy_dvfs0_con3_t DVFS_CON3_ECT, DVFS_CON3;
	phy_dvfs0_con4_t DVFS_CON4_ECT, DVFS_CON4;
	phy_zq_con0_t ZQ_CON0;

	if (!g_smc_dfs_table || !g_phy_dfs_table || !g_dram_dfs_table ||
	    !mif_freq_to_level)
		return -ENODEV;
	if (timing_set_idx > MIF_TIMING_SET_1)
		return -EINVAL;

	target_mif_level_idx = convert_to_level(target_mif_freq);

	if (target_mif_freq == 936 * MHZ) {
		target_mif_level_switch_idx = convert_to_level_switch(target_mif_freq);
		target_mif_level_switch_idx += num_mif_freq_to_level;
	} else {
		target_mif_level_switch_idx = convert_to_level(target_mif_freq);
	}
	if (target_mif_level_idx >= num_mif_timing_levels ||
	    target_mif_level_switch_idx >= num_mif_timing_levels)
		return -EINVAL;
	/* 1. Configure parameter */
	if (timing_set_idx == MIF_TIMING_SET_0) {

		/* Bank 1 is live while bank 0 is prepared. */
		if (exynos8890_dmc_quiesce_periodic_training(TmrTrnInterval_1))
			return -ETIMEDOUT;

		/* All channels are idle; update their controls as one phase. */
		for (n = 0; n < PHY_CH_ALL; n++) {
			if (g_smc_dfs_table[target_mif_level_idx].DvfsTrnCtl == 0)
				exynos8890_dmc_write(SMC_TmrTrnCtl_CH0 + n*0x100000, 0x0);
			else
				exynos8890_dmc_write(SMC_TmrTrnCtl_CH0 + n*0x100000, (0x1<<31) | g_smc_dfs_table[target_mif_level_idx].DvfsTrnCtl);
		}

		exynos8890_dmc_write(DMC_MISC_CON1_CH0, 0x0);	//timing_set_sw_r=0x0
		exynos8890_dmc_write(DMC_MISC_CON1_CH1, 0x0);	//timing_set_sw_r=0x0
		exynos8890_dmc_write(DMC_MISC_CON1_CH2, 0x0);	//timing_set_sw_r=0x0
		exynos8890_dmc_write(DMC_MISC_CON1_CH3, 0x0);	//timing_set_sw_r=0x0

		exynos8890_dmc_write(DramTiming0_0, g_smc_dfs_table[target_mif_level_idx].DramTiming0);
		exynos8890_dmc_write(DramTiming1_0, g_smc_dfs_table[target_mif_level_idx].DramTiming1);
		exynos8890_dmc_write(DramTiming2_0, g_smc_dfs_table[target_mif_level_idx].DramTiming2);
		exynos8890_dmc_write(DramTiming3_0, g_smc_dfs_table[target_mif_level_idx].DramTiming3);
		exynos8890_dmc_write(DramTiming4_0, g_smc_dfs_table[target_mif_level_idx].DramTiming4);
		exynos8890_dmc_write(DramTiming5_0, g_smc_dfs_table[target_mif_level_idx].DramTiming5);
		exynos8890_dmc_write(DramTiming6_0, g_smc_dfs_table[target_mif_level_idx].DramTiming6);
		exynos8890_dmc_write(DramTiming7_0, g_smc_dfs_table[target_mif_level_idx].DramTiming7);
		exynos8890_dmc_write(DramTiming8_0, g_smc_dfs_table[target_mif_level_idx].DramTiming8);
		exynos8890_dmc_write(DramTiming9_0, g_smc_dfs_table[target_mif_level_idx].DramTiming9);
		exynos8890_dmc_write(DramDerateTiming0_0, g_smc_dfs_table[target_mif_level_idx].DramDerateTiming0);
		exynos8890_dmc_write(DramDerateTiming1_0, g_smc_dfs_table[target_mif_level_idx].DramDerateTiming1);
		exynos8890_dmc_write(Dimm0AutoRefTiming1_0, g_smc_dfs_table[target_mif_level_idx].Dimm0AutoRefTiming1);
		exynos8890_dmc_write(Dimm1AutoRefTiming1_0, g_smc_dfs_table[target_mif_level_idx].Dimm1AutoRefTiming1);
		exynos8890_dmc_write(AutoRefTiming2_0, g_smc_dfs_table[target_mif_level_idx].AutoRefTiming2);
		exynos8890_dmc_write(PwrMgmtTiming0_0, g_smc_dfs_table[target_mif_level_idx].PwrMgmtTiming0);
		exynos8890_dmc_write(PwrMgmtTiming1_0, g_smc_dfs_table[target_mif_level_idx].PwrMgmtTiming1);
		exynos8890_dmc_write(PwrMgmtTiming2_0, g_smc_dfs_table[target_mif_level_idx].PwrMgmtTiming2);
		exynos8890_dmc_write(PwrMgmtTiming3_0, g_smc_dfs_table[target_mif_level_idx].PwrMgmtTiming3);
		exynos8890_dmc_write(TmrTrnInterval_0, g_smc_dfs_table[target_mif_level_idx].TmrTrnInterval);
		exynos8890_dmc_write(DFIDelay1_0, g_smc_dfs_table[target_mif_level_idx].DFIDelay1);
		exynos8890_dmc_write(DFIDelay2_0, g_smc_dfs_table[target_mif_level_idx].DFIDelay2);
		exynos8890_dmc_write(DvfsTrnCtl_0, g_smc_dfs_table[target_mif_level_idx].DvfsTrnCtl);
		exynos8890_dmc_write(TrnTiming0_0, g_smc_dfs_table[target_mif_level_idx].TrnTiming0);
		exynos8890_dmc_write(TrnTiming1_0, g_smc_dfs_table[target_mif_level_idx].TrnTiming1);
		exynos8890_dmc_write(TrnTiming2_0, g_smc_dfs_table[target_mif_level_idx].TrnTiming2);

		uReg = exynos8890_dmc_read(PHY_DVFS_CON_CH0);
		uReg &= ~(0x3 << 30);
		uReg |= (0x1 << 30);	//0x1 = DVFS 1 mode
		exynos8890_dmc_write(PHY_DVFS_CON, uReg);

		for (n = 0; n < PHY_CH_ALL; n++) {

			if (drampara_config != 0) {

				/* get offsetc information */
				if (drampara_config->phy_offset.magic == PHY_OFFSET_CONFIG_MAGIC) {

					dvfs0_con2 = (phy_dvfs0_con2_t *)&g_phy_dfs_table[target_mif_level_idx].DVFSn_CON2;

					if ((dvfs0_con2->bitfield.dvfs0_gateadj != 0)					\
							&& ((drampara_config->phy_offset.gate[n][0] != 0)			\
								|| (drampara_config->phy_offset.gate[n][1] != 0)))
						gate_offset_adjust = 1;
				}

				offsetc = 0;
				if (gate_offset_adjust == 1) {

					cal_con1.data = exynos8890_dmc_read(PHY_CAL_CON1[n]);
					cal_con1.bitfield.rdlvl_pass_adj	= 6;
					cal_con1.bitfield.glvl_start_adj	= 3;

					exynos8890_dmc_write(PHY_CAL_CON1[n], cal_con1.data);

					for (byte = 0; byte < PHY_BYTE_ALL; byte++) {

						new_gate_offset = (drampara_config->phy_offset.lock[n] >> 1)	\
										  - drampara_config->phy_offset.gate[n][byte];

						if (new_gate_offset < 0)
							offsetc |= ((new_gate_offset & 0x7f) | 0x80) << (8 * byte);
						else
							offsetc |= (new_gate_offset & 0x7f) << (8 * byte);
					}
				} else {
					cal_con1.data = exynos8890_dmc_read(PHY_CAL_CON1[n]);
					cal_con1.bitfield.rdlvl_pass_adj	= drampara_config->phy.adjust.rdlvl_pass_adj;
					cal_con1.bitfield.glvl_start_adj	= drampara_config->phy.adjust.glvl_start_adj;
					exynos8890_dmc_write(PHY_CAL_CON1[n], cal_con1.data);
				}

				exynos8890_dmc_write(PHY_OFFSETC_CON1[n], offsetc);
			}

			DVFS_CON3.data = DVFS_CON3_ECT.data = g_phy_dfs_table[target_mif_level_idx].DVFSn_CON3;
			DVFS_CON4.data = DVFS_CON4_ECT.data = g_phy_dfs_table[target_mif_level_idx].DVFSn_CON4;
			ZQ_CON0.data = exynos8890_dmc_read(PHY_ZQ_CON0[n]);

			/* WA: Ch1 and Ch2 wrong connection SFR and IO control pin */
			if ((n == 1) || (n == 2)) {

				/* ca register => byte1 pad */
				DVFS_CON3.bitfield.dvfs0_zq_mode_dds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_dds;
				DVFS_CON3.bitfield.dvfs0_zq_mode_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_pdds;
				ZQ_CON0.bitfield.zq_mode_term				= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds1_term;

				/* ds0 register => ca pad */
				DVFS_CON3.bitfield.dvfs0_zq_ds0_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds0_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds0_term		= 0x0;

				/* ds1 register => byte0 pad */
				DVFS_CON3.bitfield.dvfs0_zq_ds1_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds1_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds1_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds0_term;

			} else {

				DVFS_CON3.bitfield.dvfs0_zq_mode_dds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_dds;
				DVFS_CON3.bitfield.dvfs0_zq_mode_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_pdds;
				ZQ_CON0.bitfield.zq_mode_term				= 0x0;

				DVFS_CON3.bitfield.dvfs0_zq_ds0_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds0_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds0_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds0_term;

				DVFS_CON3.bitfield.dvfs0_zq_ds1_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds1_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds1_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds1_term;
			}

			/* tied to byte0 */
			DVFS_CON3.bitfield.dvfs0_ds0_vref				= DVFS_CON3_ECT.bitfield.dvfs0_ds0_vref;
			DVFS_CON3.bitfield.dvfs0_ds1_vref				= DVFS_CON3_ECT.bitfield.dvfs0_ds0_vref;

			exynos8890_dmc_write(PHY_DVFS0_CON0[n], g_phy_dfs_table[target_mif_level_idx].DVFSn_CON0);
			exynos8890_dmc_write(PHY_DVFS0_CON1[n], g_phy_dfs_table[target_mif_level_idx].DVFSn_CON1);
			exynos8890_dmc_write(PHY_DVFS0_CON2[n], g_phy_dfs_table[target_mif_level_idx].DVFSn_CON2);
			exynos8890_dmc_write(PHY_DVFS0_CON3[n], DVFS_CON3.data);
			exynos8890_dmc_write(PHY_DVFS0_CON4[n], DVFS_CON4.data);
			exynos8890_dmc_write(PHY_ZQ_CON0[n],	 ZQ_CON0.data);
		}

		mr13 = (0x1 << 7) | (0x0 << 6) | (0x0 << 5) | (0x0 << 3);	//FSP-OP=0x1, FSP-WR=0x0, DMD=0x0, VRCG=0x0
		smc_mode_register_write(DRAM_MR13, mr13);
		smc_mode_register_write(DRAM_MR1, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR1);
		smc_mode_register_write(DRAM_MR2, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR2);
		smc_mode_register_write(DRAM_MR3, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR3);
		smc_mode_register_write(DRAM_MR11, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR11);
		smc_mode_register_write(DRAM_MR12, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR12);
		smc_mode_register_write(DRAM_MR14, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR14);
		smc_mode_register_write(DRAM_MR22, g_dram_dfs_table[target_mif_level_idx].DirectCmd_MR22);

		mr13 &= ~(0x1 << 7);	// clear FSP-OP[7]

		exynos8890_dmc_write(MRS_DATA1_CH0, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH1, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH2, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH3, mr13);

	} else if (timing_set_idx == MIF_TIMING_SET_1) {

		/* Bank 0 is live while bank 1 is prepared. */
		if (exynos8890_dmc_quiesce_periodic_training(TmrTrnInterval_0))
			return -ETIMEDOUT;

		/* All channels are idle; update their controls as one phase. */
		for (n = 0; n < PHY_CH_ALL; n++)
			exynos8890_dmc_write(SMC_TmrTrnCtl_CH0 + n * SZ_1M, 0);

		exynos8890_dmc_write(DMC_MISC_CON1_CH0, 0x1);	//timing_set_sw_r=0x1
		exynos8890_dmc_write(DMC_MISC_CON1_CH1, 0x1);	//timing_set_sw_r=0x1
		exynos8890_dmc_write(DMC_MISC_CON1_CH2, 0x1);	//timing_set_sw_r=0x1
		exynos8890_dmc_write(DMC_MISC_CON1_CH3, 0x1);	//timing_set_sw_r=0x1

		exynos8890_dmc_write(DramTiming0_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming0);
		exynos8890_dmc_write(DramTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming1);
		exynos8890_dmc_write(DramTiming2_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming2);
		exynos8890_dmc_write(DramTiming3_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming3);
		exynos8890_dmc_write(DramTiming4_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming4);
		exynos8890_dmc_write(DramTiming5_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming5);
		exynos8890_dmc_write(DramTiming6_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming6);
		exynos8890_dmc_write(DramTiming7_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming7);
		exynos8890_dmc_write(DramTiming8_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming8);
		exynos8890_dmc_write(DramTiming9_1, g_smc_dfs_table[target_mif_level_switch_idx].DramTiming9);
		exynos8890_dmc_write(DramDerateTiming0_1, g_smc_dfs_table[target_mif_level_switch_idx].DramDerateTiming0);
		exynos8890_dmc_write(DramDerateTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].DramDerateTiming1);
		exynos8890_dmc_write(Dimm0AutoRefTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].Dimm0AutoRefTiming1);
		exynos8890_dmc_write(Dimm1AutoRefTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].Dimm1AutoRefTiming1);
		exynos8890_dmc_write(AutoRefTiming2_1, g_smc_dfs_table[target_mif_level_switch_idx].AutoRefTiming2);
		exynos8890_dmc_write(PwrMgmtTiming0_1, g_smc_dfs_table[target_mif_level_switch_idx].PwrMgmtTiming0);
		exynos8890_dmc_write(PwrMgmtTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].PwrMgmtTiming1);
		exynos8890_dmc_write(PwrMgmtTiming2_1, g_smc_dfs_table[target_mif_level_switch_idx].PwrMgmtTiming2);
		exynos8890_dmc_write(PwrMgmtTiming3_1, g_smc_dfs_table[target_mif_level_switch_idx].PwrMgmtTiming3);
		exynos8890_dmc_write(TmrTrnInterval_1, g_smc_dfs_table[target_mif_level_switch_idx].TmrTrnInterval);
		exynos8890_dmc_write(DFIDelay1_1, g_smc_dfs_table[target_mif_level_switch_idx].DFIDelay1);
		exynos8890_dmc_write(DFIDelay2_1, g_smc_dfs_table[target_mif_level_switch_idx].DFIDelay2);
		exynos8890_dmc_write(DvfsTrnCtl_1, g_smc_dfs_table[target_mif_level_switch_idx].DvfsTrnCtl);
		exynos8890_dmc_write(TrnTiming0_1, g_smc_dfs_table[target_mif_level_switch_idx].TrnTiming0);
		exynos8890_dmc_write(TrnTiming1_1, g_smc_dfs_table[target_mif_level_switch_idx].TrnTiming1);
		exynos8890_dmc_write(TrnTiming2_1, g_smc_dfs_table[target_mif_level_switch_idx].TrnTiming2);

		uReg = exynos8890_dmc_read(PHY_DVFS_CON_CH0);
		uReg &= ~(0x3 << 30);
		uReg |= (0x2 << 30);	//0x2 = DVFS 2 mode
		exynos8890_dmc_write(PHY_DVFS_CON, uReg);

		for (n = 0; n < PHY_CH_ALL; n++) {

			DVFS_CON3.data = DVFS_CON3_ECT.data = g_phy_dfs_table[target_mif_level_switch_idx].DVFSn_CON3;
			DVFS_CON4.data = DVFS_CON4_ECT.data = g_phy_dfs_table[target_mif_level_switch_idx].DVFSn_CON4;
			ZQ_CON0.data = exynos8890_dmc_read(PHY_ZQ_CON0[n]);

			/* WA: Ch1 and Ch2 wrong connection SFR and IO control pin */
			if ((n == 1) || (n == 2)) {

				/* ca register => byte1 pad */
				DVFS_CON3.bitfield.dvfs0_zq_mode_dds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_dds;
				DVFS_CON3.bitfield.dvfs0_zq_mode_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_pdds;
				ZQ_CON0.bitfield.zq_mode_term				= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds1_term;

				/* ds0 register => ca pad */
				DVFS_CON3.bitfield.dvfs0_zq_ds0_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds0_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds0_term		= 0x0;

				/* ds1 register => byte0 pad */
				DVFS_CON3.bitfield.dvfs0_zq_ds1_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds1_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds1_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds0_term;

			} else {

				DVFS_CON3.bitfield.dvfs0_zq_mode_dds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_dds;
				DVFS_CON3.bitfield.dvfs0_zq_mode_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_mode_pdds;
				ZQ_CON0.bitfield.zq_mode_term				= 0x0;

				DVFS_CON3.bitfield.dvfs0_zq_ds0_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds0_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds0_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds0_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds0_term;

				DVFS_CON3.bitfield.dvfs0_zq_ds1_dds			= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_dds;
				DVFS_CON3.bitfield.dvfs0_zq_ds1_pdds		= DVFS_CON3_ECT.bitfield.dvfs0_zq_ds1_pdds;
				DVFS_CON4.bitfield.dvfs0_zq_ds1_term		= DVFS_CON4_ECT.bitfield.dvfs0_zq_ds1_term;
			}

			/* tied to byte0 */
			DVFS_CON3.bitfield.dvfs0_ds0_vref				= DVFS_CON3_ECT.bitfield.dvfs0_ds0_vref;
			DVFS_CON3.bitfield.dvfs0_ds1_vref				= DVFS_CON3_ECT.bitfield.dvfs0_ds0_vref;

			exynos8890_dmc_write(PHY_DVFS1_CON0[n], g_phy_dfs_table[target_mif_level_switch_idx].DVFSn_CON0);
			exynos8890_dmc_write(PHY_DVFS1_CON1[n], g_phy_dfs_table[target_mif_level_switch_idx].DVFSn_CON1);
			exynos8890_dmc_write(PHY_DVFS1_CON2[n], g_phy_dfs_table[target_mif_level_switch_idx].DVFSn_CON2);
			exynos8890_dmc_write(PHY_DVFS1_CON3[n], DVFS_CON3.data);
			exynos8890_dmc_write(PHY_DVFS1_CON4[n], DVFS_CON4.data);
			exynos8890_dmc_write(PHY_ZQ_CON0[n],	 ZQ_CON0.data);
		}


		mr13 = (0x0 << 7) | (0x1 << 6) | (0x0 << 5) | (0x0 << 3);	//FSP-OP=0x0, FSP-WR=0x1, DMD=0x0, VRCG=0x0
		smc_mode_register_write(DRAM_MR13, mr13);
		smc_mode_register_write(DRAM_MR1, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR1);
		smc_mode_register_write(DRAM_MR2, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR2);
		smc_mode_register_write(DRAM_MR3, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR3);
		smc_mode_register_write(DRAM_MR11, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR11);
		smc_mode_register_write(DRAM_MR12, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR12);
		smc_mode_register_write(DRAM_MR14, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR14);
		smc_mode_register_write(DRAM_MR22, g_dram_dfs_table[target_mif_level_switch_idx].DirectCmd_MR22);

		mr13 &= ~(0x1 << 7);	// clear FSP-OP[7]
		mr13 |= (0x1 << 7);	// set FSP-OP[7]=0x1
		exynos8890_dmc_write(MRS_DATA1_CH0, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH1, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH2, mr13);
		exynos8890_dmc_write(MRS_DATA1_CH3, mr13);
	}	 else {
		pr_err("wrong DMC timing set selection on DVFS\n");
		return -EINVAL;
	}

	return 0;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_timing_table_init
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static int exynos8890_dmc_timing_table_init(void)
{
	const struct exynos8890_calib_mif_timing *timing;
	int i;

	query_key = ((u64)exynos8890_dmc_read(PMU_DREX_CALIBRATION2) << 32) |
		exynos8890_dmc_read(PMU_DREX_CALIBRATION3);
	if (!query_key)
		return -ENODATA;

	timing = exynos8890_calib_get_mif_timing(query_key);
	if (IS_ERR(timing))
		return PTR_ERR(timing);
	if (timing->num_parameters != num_of_dram_parameter ||
	    !timing->num_levels)
		return -EINVAL;
	g_smc_dfs_table = kcalloc(timing->num_levels,
				   sizeof(*g_smc_dfs_table), GFP_KERNEL);
	if (!g_smc_dfs_table)
		return -ENOMEM;

	g_phy_dfs_table = kcalloc(timing->num_levels,
				   sizeof(*g_phy_dfs_table), GFP_KERNEL);
	if (!g_phy_dfs_table) {
		kfree(g_smc_dfs_table);
		g_smc_dfs_table = NULL;
		return -ENOMEM;
	}

	g_dram_dfs_table = kcalloc(timing->num_levels,
				    sizeof(*g_dram_dfs_table), GFP_KERNEL);
	if (!g_dram_dfs_table) {
		kfree(g_phy_dfs_table);
		g_phy_dfs_table = NULL;
		kfree(g_smc_dfs_table);
		g_smc_dfs_table = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < timing->num_levels; ++i) {
#define TIMING(_column) timing->values[i * num_of_dram_parameter + (_column)]
		g_smc_dfs_table[i].DramTiming0 = TIMING(DramTiming0);
		g_smc_dfs_table[i].DramTiming1 = TIMING(DramTiming1);
		g_smc_dfs_table[i].DramTiming2 = TIMING(DramTiming2);
		g_smc_dfs_table[i].DramTiming3 = TIMING(DramTiming3);
		g_smc_dfs_table[i].DramTiming4 = TIMING(DramTiming4);
		g_smc_dfs_table[i].DramTiming5 = TIMING(DramTiming5);
		g_smc_dfs_table[i].DramTiming6 = TIMING(DramTiming6);
		g_smc_dfs_table[i].DramTiming7 = TIMING(DramTiming7);
		g_smc_dfs_table[i].DramTiming8 = TIMING(DramTiming8);
		g_smc_dfs_table[i].DramTiming9 = TIMING(DramTiming9);
		g_smc_dfs_table[i].DramDerateTiming0 = TIMING(DramDerateTiming0);
		g_smc_dfs_table[i].DramDerateTiming1 = TIMING(DramDerateTiming1);
		g_smc_dfs_table[i].Dimm0AutoRefTiming1 = TIMING(Dimm0AutoRefTiming1);
		g_smc_dfs_table[i].Dimm1AutoRefTiming1 = TIMING(Dimm1AutoRefTiming1);
		g_smc_dfs_table[i].AutoRefTiming2 = TIMING(AutoRefTiming2);
		g_smc_dfs_table[i].PwrMgmtTiming0 = TIMING(PwrMgmtTiming0);
		g_smc_dfs_table[i].PwrMgmtTiming1 = TIMING(PwrMgmtTiming1);
		g_smc_dfs_table[i].PwrMgmtTiming2 = TIMING(PwrMgmtTiming2);
		g_smc_dfs_table[i].PwrMgmtTiming3 = TIMING(PwrMgmtTiming3);
		g_smc_dfs_table[i].TmrTrnInterval = TIMING(TmrTrnInterval);
		g_smc_dfs_table[i].DFIDelay1 = TIMING(DFIDelay1);
		g_smc_dfs_table[i].DFIDelay2 = TIMING(DFIDelay2);
		g_smc_dfs_table[i].DvfsTrnCtl = TIMING(DvfsTrnCtl);
		g_smc_dfs_table[i].TrnTiming0 = TIMING(TrnTiming0);
		g_smc_dfs_table[i].TrnTiming1 = TIMING(TrnTiming1);
		g_smc_dfs_table[i].TrnTiming2 = TIMING(TrnTiming2);

		g_phy_dfs_table[i].DVFSn_CON0 = TIMING(DVFSn_CON0);
		g_phy_dfs_table[i].DVFSn_CON1 = TIMING(DVFSn_CON1);
		g_phy_dfs_table[i].DVFSn_CON2 = TIMING(DVFSn_CON2);
		g_phy_dfs_table[i].DVFSn_CON3 = TIMING(DVFSn_CON3);
		g_phy_dfs_table[i].DVFSn_CON4 = TIMING(DVFSn_CON4);

		g_dram_dfs_table[i].DirectCmd_MR1 = TIMING(DirectCmd_MR1);
		g_dram_dfs_table[i].DirectCmd_MR2 = TIMING(DirectCmd_MR2);
		g_dram_dfs_table[i].DirectCmd_MR3 = TIMING(DirectCmd_MR3);
		g_dram_dfs_table[i].DirectCmd_MR11 = TIMING(DirectCmd_MR11);
		g_dram_dfs_table[i].DirectCmd_MR12 = TIMING(DirectCmd_MR12);
		g_dram_dfs_table[i].DirectCmd_MR14 = TIMING(DirectCmd_MR14);
		g_dram_dfs_table[i].DirectCmd_MR22 = TIMING(DirectCmd_MR22);
#undef TIMING
	}
	num_mif_timing_levels = timing->num_levels;

	return 0;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_level_init
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static int exynos8890_dmc_level_init(void)
{
	const struct exynos8890_calib_domain *domain;
	int i;

	domain = exynos8890_calib_get_domain(EXYNOS8890_CALIB_MIF);
	if (IS_ERR(domain))
		return PTR_ERR(domain);
	/* The vendor table appends one dedicated 936 MHz switch-timing row. */
	if (num_mif_timing_levels < domain->num_opps + 1)
		return -EINVAL;

	mif_freq_to_level = kcalloc(domain->num_opps,
				    sizeof(*mif_freq_to_level), GFP_KERNEL);
	if (!mif_freq_to_level)
		return -ENOMEM;

	num_mif_freq_to_level = domain->num_opps;

	for (i = 0; i < domain->num_opps; ++i)
		mif_freq_to_level[i] = domain->opps[i].rate_hz;

	return 0;
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_print_info
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
static void __maybe_unused exynos8890_dmc_print_info(void)
{
	int ch;
	int rank;
	int byte;
	int idx;

	unsigned int tdqs2dq;
	unsigned int tdqsck[2];

	for (ch = 0; ch < 4; ch++) {
		for (rank = 0; rank < 2; rank++) {
			for (byte = 0; byte < 2; byte++)
				exynos8890_dmc_get_dram_tdqsck(ch, rank, byte, &tdqsck[byte]);

			pr_info("tdqsck for ch%d, rank%d : %d, %d\n", ch, rank, tdqsck[0], tdqsck[1]);
		}
	}

	for (rank = 0; rank < 2; rank++) {
		for (ch = 0; ch < 4; ch++) {
			pr_info("tdqs2dq for ch%d, rank%d\n", ch, rank);

			for (idx = 0; idx < 18; idx++) {
				exynos8890_dmc_get_dram_tdqs2dq(ch, rank, idx, &tdqs2dq);
				pr_info("tdqs2dq[%d] : %d\n", idx, tdqs2dq);
			}
		}
	}
}

/******************************************************************************
 *
 * @fn      exynos8890_dmc_timing_init_legacy
 *
 * @brief
 *
 * @param
 *
 * @return
 *
 *****************************************************************************/
int exynos8890_dmc_timing_init(struct exynos8890_dmc *dmc)
{
	phys_addr_t calibration_pa;
	int ret;

	if (!dmc)
		return -EINVAL;

	calibration_pa = exynos8890_dmc_read(PMU_DREX_CALIBRATION1);
	if (calibration_pa)
		config_base = memremap(calibration_pa, SZ_2K, MEMREMAP_WB);

	if (config_base)
		drampara_config = config_base;
	else
		pr_warn("Exynos8890 DMC training offsets are unavailable\n");

	ret = exynos8890_dmc_timing_table_init();
	if (ret)
		goto err_unmap;
	ret = exynos8890_dmc_level_init();
	if (ret)
		goto err_tables;

	return 0;

err_tables:
	kfree(g_dram_dfs_table);
	g_dram_dfs_table = NULL;
	kfree(g_phy_dfs_table);
	g_phy_dfs_table = NULL;
	kfree(g_smc_dfs_table);
	g_smc_dfs_table = NULL;
	num_mif_timing_levels = 0;
err_unmap:
	if (config_base)
		memunmap(config_base);
	config_base = NULL;
	drampara_config = NULL;
	return ret;
}

void exynos8890_dmc_timing_exit(struct exynos8890_dmc *dmc)
{
	(void)dmc;
	kfree(mif_freq_to_level);
	mif_freq_to_level = NULL;
	num_mif_freq_to_level = 0;
	kfree(g_dram_dfs_table);
	g_dram_dfs_table = NULL;
	kfree(g_phy_dfs_table);
	g_phy_dfs_table = NULL;
	kfree(g_smc_dfs_table);
	g_smc_dfs_table = NULL;
	num_mif_timing_levels = 0;
	if (config_base)
		memunmap(config_base);
	config_base = NULL;
	drampara_config = NULL;
}
