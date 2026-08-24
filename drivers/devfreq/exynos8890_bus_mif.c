/* linux/drivers/devfreq/exynos/exynos8890_bus_mif.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung EXYNOS8890 SoC MIF devfreq driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/workqueue.h>

#include <linux/regulator/consumer.h>

#include <linux/soc/samsung/exynos8890-devfreq.h>
/*
 * apm-exynos.h and asv-exynos.h are both unused here (this file's only
 * ASV-table call, cal_dfs_get_rate_asv_table(), comes from pwrcal below,
 * not asv-exynos.h) - vendor's real build likely just pulled these in
 * platform-wide.
 */
#include <linux/mcu_ipc.h>
/*
 * mfd/samsung/core.h isn't ported (the real MFD Samsung PMIC core, not
 * this driver's concern) - s2m_set_vth() is the one thing from it this
 * file calls; see the no-op note at its call sites below for why.
 */

#include <linux/soc/samsung/exynos8890-pwrcal.h>
#include "../../drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-vclk.h"
#if 0
#include "exynos8890_ppmu.h"
#endif

#define DEVFREQ_MIF_REBOOT_FREQ	(1014000)
#define DEVFREQ_MIF_DIFF_FREQ		(962000)
#define DEVFREQ_MIF_CMOS_FREQ		(468000)
#define DEVFREQ_MIF_SWITCH_FREQ_HI	(936000)
#define DEVFREQ_MIF_SWITCH_FREQ 	(528000)
#define DEVFREQ_MIF_BUS3_PLL_THRESHOLD	(845000)
#define DEVFREQ_MIF_BUCK_CTRL		(1539000)
#define SWITCH_CMOS_VOLT_OFFSET		(56250)

/* definition for EVS mode */
#define MBOX_EVS_MODE			(16)
#define MBOX_UMTS_MODE			(18)
#define DEVFREQ_MIF_EVS_FREQ		(1144000)
#define DEVFREQ_MIF_UMTS_FREQ		(676000)

static struct pm_qos_request __maybe_unused int_pm_qos_from_mif;

/* kept for when int_pm_qos_from_mif gets real dev_pm_qos wiring */
static u32 __maybe_unused int_min_table[] = {
	400000,		/* MIF L0  1794MHz, INT L11 */
	336000,		/* MIF L1  1716MHz, INT L12 */
	336000,		/* MIF L2  1539MHz, INT L12 */
	255000,		/* MIF L3  1352MHz, INT L13 */
	255000,		/* MIF L4  1144MHz, INT L13 */
	200000,		/* MIF L5  1014MHz, INT L14 */
	200000,		/* MIF L6   845MHz, INT L14 */
	200000,		/* MIF L7   676MHz, INT L14 */
	200000,		/* MIF L8   546MHz, INT L14 */
	200000,		/* MIF L9   421MHz, INT L14 */
	200000,		/* MIF L10  286MHz, INT L16 */
	200000		/* MIF L11  208MHz, INT L16 */
};

u32 sw_volt_table[2];

struct mif_private {
	struct regulator		*vdd;
};

struct mif_private mif_private_data;

int is_dll_on(void)
{
	return cal_dfs_ext_ctrl(dvfs_mif, cal_dfs_mif_is_dll_on, 0);
}
EXPORT_SYMBOL_GPL(is_dll_on);

static struct exynos_devfreq_data *mif_data;

static int exynos8890_devfreq_mif_cmu_dump(struct device *dev,
					struct exynos_devfreq_data *data)
{
	mutex_lock(&data->devfreq->lock);
	cal_vclk_dbg_info(dvfs_mif);
	mutex_unlock(&data->devfreq->lock);

	return 0;
}

static int exynos8890_devfreq_mif_reboot(struct device *dev,
					struct exynos_devfreq_data *data)
{
	u32 freq = DEVFREQ_MIF_REBOOT_FREQ;

	data->max_freq = freq;

	mutex_lock(&data->devfreq->lock);
	update_devfreq(data->devfreq);
	mutex_unlock(&data->devfreq->lock);

	return 0;
}

/*
 * This picked the frequency devfreq forces at suspend (lower for
 * UMTS/EVS modem mailbox modes, matching modem power needs) via
 * struct devfreq_dev_profile's old suspend_freq field - mainline's
 * devfreq_dev_profile has no such field any more (see include/linux/
 * devfreq.h), so there's nothing left to adjust here. No-op.
 */
static int exynos8890_devfreq_mif_pm_suspend_prepare(struct device *dev,
					struct exynos_devfreq_data *data)
{
	return 0;
}

static int exynos8890_devfreq_cl_dvfs_start(struct exynos_devfreq_data *data)
{
	int ret = 0;

#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	ret = exynos_cl_dvfs_start(ID_MIF);
#endif

	return ret;
}

static int exynos8890_devfreq_cl_dvfs_stop(u32 target_idx,
					struct exynos_devfreq_data *data)
{
	int ret = 0;

#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	ret = exynos_cl_dvfs_stop(ID_MIF, target_idx);
#endif

	return ret;
}

static void exynos8890_devfreq_mif_set_voltage_prepare(struct exynos_devfreq_data *data)
{
	/*
	 * The Clock Gate Control should be disable before voltage change.
	 */
	if (cal_dfs_ext_ctrl(dvfs_mif, cal_dfs_ctrl_clk_gate, 0)) {
		dev_err(data->dev, "failed CG control disable\n");
		BUG_ON(1);
	}
}

static void exynos8890_devfreq_mif_set_voltage_post(struct exynos_devfreq_data *data)
{
	/*
	 * The Clock Gate Control should be enable before voltage change,
	 * if the HWACG is enabled.
	 */
	if (cal_dfs_ext_ctrl(dvfs_mif, cal_dfs_ctrl_clk_gate, 1)) {
		dev_err(data->dev, "failed CG control enable\n");
		BUG_ON(1);
	}
}

static int exynos8890_devfreq_mif_get_switch_freq(u32 cur_freq, u32 new_freq,
						u32 *switch_freq)
{
	*switch_freq = DEVFREQ_MIF_SWITCH_FREQ;

	if (cur_freq > DEVFREQ_MIF_SWITCH_FREQ_HI ||
		new_freq > DEVFREQ_MIF_SWITCH_FREQ_HI)
		*switch_freq = DEVFREQ_MIF_SWITCH_FREQ_HI;

	if (cur_freq < DEVFREQ_MIF_BUS3_PLL_THRESHOLD ||
		new_freq < DEVFREQ_MIF_BUS3_PLL_THRESHOLD)
		*switch_freq = DEVFREQ_MIF_SWITCH_FREQ;

	return 0;
}

static int exynos8890_devfreq_mif_get_switch_voltage(u32 cur_freq, u32 new_freq,
						struct exynos_devfreq_data *data)
{
	/* if switching PLL is CMOS I/O mode level, it should be add a voltage offset */
	if (cur_freq <= DEVFREQ_MIF_CMOS_FREQ || new_freq <= DEVFREQ_MIF_CMOS_FREQ) {
		data->switch_volt = sw_volt_table[1] + SWITCH_CMOS_VOLT_OFFSET;
		goto out;
	}

	if (cur_freq > DEVFREQ_MIF_DIFF_FREQ && new_freq > DEVFREQ_MIF_DIFF_FREQ) {
		data->switch_volt = 0;
		goto out;
	}

	if (cur_freq >= new_freq)
		data->switch_volt = data->old_volt;
	else if (cur_freq < new_freq)
		data->switch_volt = data->new_volt;

out:
	//pr_info("Selected switching voltage: %uuV\n", data->switch_volt);
	return 0;
}

static int exynos8890_devfreq_mif_get_freq(struct device *dev, u32 *cur_freq,
					struct exynos_devfreq_data *data)
{
	*cur_freq = (u32)clk_get_rate(data->clk);
	if (*cur_freq == 0) {
		dev_err(dev, "failed get frequency from CAL\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * s2m_set_vth(reg, enable) toggles the S2MPS16 PMIC's Vth (buck ramp/
 * mode) setting when MIF crosses DEVFREQ_MIF_BUCK_CTRL - a real
 * regulator-mode optimization, not the voltage level itself (that's
 * still set correctly via data->new_volt/switch_volt through the
 * generic devfreq voltage-scaling path). Its only implementation lives
 * in the S2MPS16 regulator driver (drivers/regulator/s2mps16.c
 * upstream, ~1400 lines, plus the MFD Samsung PMIC core it depends on),
 * neither of which is ported here. No-op until that driver exists.
 */
static int exynos8890_devfreq_mif_change_to_switch_freq(struct device *dev,
					struct exynos_devfreq_data *data)
{
	if (clk_set_rate(data->sw_clk, data->switch_freq)) {
		dev_err(dev, "failed to set switching frequency by CAL (%uKhz for %uKhz)\n",
				data->switch_freq, data->new_freq);
		return -EINVAL;
	}

	return 0;
}

static int exynos8890_devfreq_mif_restore_from_switch_freq(struct device *dev,
					struct exynos_devfreq_data *data)
{
	if (clk_set_rate(data->clk, data->new_freq)) {
		dev_err(dev, "failed to set frequency by CAL (%uKhz)\n",
				data->new_freq);
		return -EINVAL;
	}

	return 0;
}

/*
 * int_pm_qos_from_mif: raises the INT clock domain's floor while MIF is
 * at a level that needs more bus bandwidth (int_min_table), via the
 * same now-removed global PM_QOS_DEVICE_THROUGHPUT class as decon_bts.c's
 * mif_qos/disp_qos/int_qos - see the note on bts_add_request() there for
 * the real fix (dev_pm_qos_add_request() against the INT devfreq device,
 * needs a new DT phandle). No-op for now.
 */
static int exynos8890_devfreq_mif_set_freq_prepare(struct device *dev,
					struct exynos_devfreq_data *data)
{
	return 0;
}

static int exynos8890_devfreq_mif_set_freq_post(struct device *dev,
					struct exynos_devfreq_data *data)
{
#ifdef CONFIG_MCU_IPC
	/* Send information about MIF frequency to mailbox */
	mbox_set_value(13, data->new_freq);
#endif

	return 0;
}

static int exynos8890_devfreq_mif_init_freq_table(struct device *dev,
						struct exynos_devfreq_data *data)
{
	u32 max_freq, min_freq, cur_freq;
	unsigned long tmp_max, tmp_min;
	struct dev_pm_opp *target_opp;
	u32 flags = 0;
	int i, ret;

	ret = cal_clk_enable(dvfs_mif);
	if (ret) {
		dev_err(dev, "failed to enable MIF\n");
		return -EINVAL;
	}

	max_freq = (u32)cal_dfs_get_max_freq(dvfs_mif);
	if (!max_freq) {
		dev_err(dev, "failed get max frequency\n");
		return -EINVAL;
	}

	dev_info(dev, "max_freq: %uKhz, get_max_freq: %uKhz\n",
			data->max_freq, max_freq);

	if (max_freq < data->max_freq) {
		rcu_read_lock();
		flags |= DEVFREQ_FLAG_LEAST_UPPER_BOUND;
		tmp_max = (unsigned long)max_freq;
		target_opp = devfreq_recommended_opp(dev, &tmp_max, flags);
		if (IS_ERR(target_opp)) {
			rcu_read_unlock();
			dev_err(dev, "not found valid OPP for max_freq\n");
			return PTR_ERR(target_opp);
		}

		data->max_freq = dev_pm_opp_get_freq(target_opp);
		rcu_read_unlock();
	}

	min_freq = (u32)cal_dfs_get_min_freq(dvfs_mif);
	if (!min_freq) {
		dev_err(dev, "failed get min frequency\n");
		return -EINVAL;
	}

	dev_info(dev, "min_freq: %uKhz, get_min_freq: %uKhz\n",
			data->min_freq, min_freq);

	if (min_freq > data->min_freq) {
		rcu_read_lock();
		flags &= ~DEVFREQ_FLAG_LEAST_UPPER_BOUND;
		tmp_min = (unsigned long)min_freq;
		target_opp = devfreq_recommended_opp(dev, &tmp_min, flags);
		if (IS_ERR(target_opp)) {
			rcu_read_unlock();
			dev_err(dev, "not found valid OPP for min_freq\n");
			return PTR_ERR(target_opp);
		}

		data->min_freq = dev_pm_opp_get_freq(target_opp);
		rcu_read_unlock();
	}

	dev_info(dev, "min_freq: %uKhz, max_freq: %uKhz\n",
			data->min_freq, data->max_freq);

	cur_freq = clk_get_rate(data->clk);
	dev_info(dev, "current frequency: %uKhz\n", cur_freq);

	for (i = 0; i < data->max_state; i++) {
		if (data->opp_list[i].freq > data->max_freq ||
			data->opp_list[i].freq < data->min_freq)
			dev_pm_opp_disable(dev, (unsigned long)data->opp_list[i].freq);
	}

	return 0;
}

static int exynos8890_devfreq_mif_get_volt_table(struct device *dev, u32 *volt_table,
						struct exynos_devfreq_data *data)
{
	struct dvfs_rate_volt mif_rate_volt[data->max_state];
	int table_size;
	int i;

	table_size = cal_dfs_get_rate_asv_table(dvfs_mif, mif_rate_volt);
	if (!table_size) {
		dev_err(dev, "failed get ASV table\n");
		return -ENODEV;
	}

	if (table_size != data->max_state) {
		dev_err(dev, "ASV table size is not matched\n");
		return -ENODEV;
	}

	for (i = 0; i < data->max_state; i++) {
		if (data->opp_list[i].freq != (u32)(mif_rate_volt[i].rate)) {
			dev_err(dev, "Freq table is not matched(%u:%u)\n",
				data->opp_list[i].freq, (u32)mif_rate_volt[i].rate);
			return -EINVAL;
		}
		volt_table[i] = (u32)mif_rate_volt[i].volt;

		/* Fill switch voltage table */
		if (!sw_volt_table[0] &&
			data->opp_list[i].freq < DEVFREQ_MIF_SWITCH_FREQ_HI)
			sw_volt_table[0] = (u32)mif_rate_volt[i-1].volt;
		if (!sw_volt_table[1] &&
			data->opp_list[i].freq < DEVFREQ_MIF_SWITCH_FREQ)
			sw_volt_table[1] = (u32)mif_rate_volt[i-1].volt;
	}

	dev_info(dev, "SW_volt %uuV in freq %uKhz\n",
			sw_volt_table[0], DEVFREQ_MIF_SWITCH_FREQ_HI);
	dev_info(dev, "SW_volt %uuV in freq %uKhz\n",
			sw_volt_table[1], DEVFREQ_MIF_SWITCH_FREQ);

	return 0;
}

static int exynos8890_mif_ppmu_register(struct device *dev,
					struct exynos_devfreq_data *data)
{
#if 0
	int ret;
	struct devfreq_exynos *ppmu_data = (struct devfreq_exynos *)&data->ppmu_data;

	ret = exynos8890_devfreq_register(ppmu_data);
	if (ret) {
		dev_err(dev, "failed ppmu register\n");
		return ret;
	}

	ret = exynos8890_ppmu_register_notifier(MIF, &data->ppmu_nb->nb);
	if (ret) {
		dev_err(dev, "failed ppmu notifier register\n");
		return ret;
	}
#endif

	return 0;
}

static int exynos8890_mif_ppmu_unregister(struct device *dev,
					struct exynos_devfreq_data *data)
{
#if 0
	exynos8890_ppmu_unregister_notifier(MIF, &data->ppmu_nb->nb);
#endif

	return 0;
}

static int exynos8890_devfreq_mif_init(struct device *dev,
					struct exynos_devfreq_data *data)
{
	data->clk = clk_get(dev, "dvfs_mif");
	if (IS_ERR_OR_NULL(data->clk)) {
		dev_err(dev, "failed get dvfs vclk\n");
		return -ENODEV;
	}

	data->sw_clk = clk_get(dev, "dvfs_mif_sw");
	if (IS_ERR_OR_NULL(data->sw_clk)) {
		dev_err(dev, "failed get dvfs sw vclk\n");
		clk_put(data->clk);
		return -ENODEV;
	}

	/* for pwm mode control */
	mif_private_data.vdd = regulator_get(NULL, "vdd_mem");
	if (IS_ERR(mif_private_data.vdd)) {
		dev_err(data->dev, "failed to get regulator(vdd_mem)\n");
		clk_put(data->clk);
		clk_put(data->sw_clk);
		return -ENODEV;
	}

	data->private_data = (void *)&mif_private_data;

	return 0;
}

static int exynos8890_devfreq_mif_exit(struct device *dev,
					struct exynos_devfreq_data *data)
{
	clk_put(data->sw_clk);
	clk_put(data->clk);

	return 0;
}

static int __init exynos8890_devfreq_mif_init_prepare(struct exynos_devfreq_data *data)
{
	data->ops.init = exynos8890_devfreq_mif_init;
	data->ops.exit = exynos8890_devfreq_mif_exit;
	data->ops.get_volt_table = exynos8890_devfreq_mif_get_volt_table;
	data->ops.ppmu_register = exynos8890_mif_ppmu_register;
	data->ops.ppmu_unregister = exynos8890_mif_ppmu_unregister;
	data->ops.get_switch_freq = exynos8890_devfreq_mif_get_switch_freq;
	data->ops.get_switch_voltage = exynos8890_devfreq_mif_get_switch_voltage;
	data->ops.get_freq = exynos8890_devfreq_mif_get_freq;
	data->ops.change_to_switch_freq = exynos8890_devfreq_mif_change_to_switch_freq;
	data->ops.restore_from_switch_freq = exynos8890_devfreq_mif_restore_from_switch_freq;
	data->ops.set_freq_prepare = exynos8890_devfreq_mif_set_freq_prepare;
	data->ops.set_freq_post = exynos8890_devfreq_mif_set_freq_post;
	data->ops.init_freq_table = exynos8890_devfreq_mif_init_freq_table;
	data->ops.cl_dvfs_start = exynos8890_devfreq_cl_dvfs_start;
	data->ops.cl_dvfs_stop = exynos8890_devfreq_cl_dvfs_stop;
	data->ops.set_voltage_prepare = exynos8890_devfreq_mif_set_voltage_prepare;
	data->ops.set_voltage_post = exynos8890_devfreq_mif_set_voltage_post;
	data->ops.reboot = exynos8890_devfreq_mif_reboot;
	data->ops.pm_suspend_prepare = exynos8890_devfreq_mif_pm_suspend_prepare;
	data->ops.cmu_dump = exynos8890_devfreq_mif_cmu_dump;

	mif_data = data;

	return 0;
}

static int __init exynos8890_devfreq_mif_initcall(void)
{
	if (register_exynos_devfreq_init_prepare(DEVFREQ_MIF,
				exynos8890_devfreq_mif_init_prepare))
		return -EINVAL;

	return 0;
}
fs_initcall(exynos8890_devfreq_mif_initcall);
