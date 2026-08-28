// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos8890 MIF devfreq glue
 *
 * Frequencies in this file are Hz.  The native DMC owner performs voltage,
 * intermediate clock, PSCDC, DREX and PHY sequencing as one transaction.
 */

#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mcu_ipc.h>
#include <linux/module.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/units.h>

#include <linux/soc/samsung/exynos8890-calibration.h>
#include <linux/soc/samsung/exynos8890-devfreq.h>
#include <linux/soc/samsung/exynos8890-dmc.h>

#define DEVFREQ_MIF_REBOOT_FREQ_HZ	1014000000U

struct exynos8890_mif_private {
	struct exynos8890_dmc *dmc;
};

static void exynos8890_devfreq_mif_put_dmc(void *data)
{
	exynos8890_dmc_put(data);
}

static int exynos8890_devfreq_mif_get_dmc(
		struct device *dev, struct exynos_devfreq_data *data)
{
	struct exynos8890_mif_private *priv = data->private_data;
	int ret;

	if (priv)
		return 0;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dmc = exynos8890_dmc_get(dev);
	if (IS_ERR(priv->dmc))
		return PTR_ERR(priv->dmc);
	ret = devm_add_action_or_reset(dev, exynos8890_devfreq_mif_put_dmc,
				       priv->dmc);
	if (ret)
		return ret;
	data->private_data = priv;
	return 0;
}

static int exynos8890_devfreq_mif_cmu_dump(struct device *dev,
					   struct exynos_devfreq_data *data)
{
	struct exynos8890_mif_private *priv = data->private_data;

	exynos8890_dmc_dump(priv->dmc);
	return 0;
}

static int exynos8890_devfreq_mif_reboot(struct device *dev,
					 struct exynos_devfreq_data *data)
{
	int ret;

	data->max_freq = DEVFREQ_MIF_REBOOT_FREQ_HZ;
	ret = dev_pm_qos_update_request(&data->devfreq->user_max_freq_req,
					DIV_ROUND_UP(DEVFREQ_MIF_REBOOT_FREQ_HZ,
						     HZ_PER_KHZ));
	if (ret < 0)
		return ret;
	mutex_lock(&data->devfreq->lock);
	ret = update_devfreq(data->devfreq);
	mutex_unlock(&data->devfreq->lock);
	return ret;
}

static int exynos8890_devfreq_mif_pm_suspend_prepare(
		struct device *dev, struct exynos_devfreq_data *data)
{
	return 0;
}

static int exynos8890_devfreq_mif_resume(
		struct device *dev, struct exynos_devfreq_data *data)
{
	struct exynos8890_mif_private *priv = data->private_data;
	unsigned long rate;
	u32 voltage;
	s32 level;
	int ret;

	rate = exynos8890_dmc_get_rate(priv->dmc);
	if (!rate || rate > U32_MAX)
		return -EIO;
	level = exynos_devfreq_get_opp_idx(data->opp_list, data->max_state,
					    rate);
	if (level < 0)
		return level;
	ret = exynos8890_dmc_get_voltage(priv->dmc, rate, &voltage);
	if (ret)
		return ret;

	data->old_freq = rate;
	data->new_freq = rate;
	data->old_idx = level;
	data->new_idx = level;
	data->old_volt = voltage;
	data->new_volt = voltage;
	WRITE_ONCE(data->devfreq->previous_freq, rate);
	return 0;
}

static int exynos8890_devfreq_cl_dvfs_start(struct exynos_devfreq_data *data)
{
#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	return exynos_cl_dvfs_start(ID_MIF);
#else
	return 0;
#endif
}

static int exynos8890_devfreq_cl_dvfs_stop(
		u32 target_idx, struct exynos_devfreq_data *data)
{
#ifdef CONFIG_EXYNOS_CL_DVFS_MIF
	return exynos_cl_dvfs_stop(ID_MIF, target_idx);
#else
	return 0;
#endif
}

static int exynos8890_devfreq_mif_get_freq(struct device *dev, u32 *cur_freq,
					   struct exynos_devfreq_data *data)
{
	struct exynos8890_mif_private *priv = data->private_data;
	unsigned long rate = exynos8890_dmc_get_rate(priv->dmc);

	if (!rate || rate > U32_MAX)
		return -EIO;
	*cur_freq = rate;
	return 0;
}

static int exynos8890_devfreq_mif_set_freq(struct device *dev, u32 old_freq,
					   u32 new_freq,
					   struct exynos_devfreq_data *data)
{
	struct exynos8890_mif_private *priv = data->private_data;

	return exynos8890_dmc_set_rate(priv->dmc, new_freq);
}

static int exynos8890_devfreq_mif_set_freq_post(
		struct device *dev, struct exynos_devfreq_data *data)
{
#ifdef CONFIG_MCU_IPC
	/* Firmware mailbox slot 13 retains its vendor ABI in kHz. */
	mbox_set_value(13, data->new_freq / 1000);
#endif
	return 0;
}

static int exynos8890_devfreq_mif_init_freq_table(
		struct device *dev, struct exynos_devfreq_data *data)
{
	const struct exynos8890_calib_domain *domain;
	struct exynos8890_mif_private *priv = data->private_data;
	u32 current_hz;
	unsigned int i, j;
	bool found;

	domain = exynos8890_calib_get_domain(EXYNOS8890_CALIB_MIF);
	if (IS_ERR(domain))
		return PTR_ERR(domain);
	if (domain->max_rate_hz < data->max_freq)
		data->max_freq = domain->max_rate_hz;
	if (domain->min_rate_hz > data->min_freq)
		data->min_freq = domain->min_rate_hz;

	for (i = 0; i < data->max_state; i++) {
		found = false;
		for (j = 0; j < domain->num_opps; j++)
			if (data->opp_list[i].freq == domain->opps[j].rate_hz) {
				found = true;
				break;
			}
		if (!found) {
			dev_err(dev, "DT MIF OPP %u Hz is not calibrated\n",
				data->opp_list[i].freq);
			return -EINVAL;
		}
		if (data->opp_list[i].freq > data->max_freq ||
		    data->opp_list[i].freq < data->min_freq ||
		    !domain->opps[j].enabled)
			dev_pm_opp_disable(dev, data->opp_list[i].freq);
	}

	current_hz = exynos8890_dmc_get_rate(priv->dmc);
	if (!current_hz)
		return -EIO;
	data->devfreq_profile.initial_freq = current_hz;
	dev_info(dev, "native MIF range %u-%u Hz, current %u Hz\n",
		 data->min_freq, data->max_freq, current_hz);
	return 0;
}

static int exynos8890_devfreq_mif_get_volt_table(
		struct device *dev, u32 *volt_table,
		struct exynos_devfreq_data *data)
{
	unsigned int i;
	struct exynos8890_mif_private *priv;
	int ret;

	ret = exynos8890_devfreq_mif_get_dmc(dev, data);
	if (ret)
		return ret;
	priv = data->private_data;
	for (i = 0; i < data->max_state; i++) {
		ret = exynos8890_dmc_get_voltage(priv->dmc,
						data->opp_list[i].freq,
						&volt_table[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int exynos8890_mif_ppmu_register(struct device *dev,
					struct exynos_devfreq_data *data)
{
	return 0;
}

static int exynos8890_mif_ppmu_unregister(struct device *dev,
					  struct exynos_devfreq_data *data)
{
	return 0;
}

static int exynos8890_devfreq_mif_init(struct device *dev,
				       struct exynos_devfreq_data *data)
{
	return exynos8890_devfreq_mif_get_dmc(dev, data);
}

static int exynos8890_devfreq_mif_exit(struct device *dev,
				       struct exynos_devfreq_data *data)
{
	return 0;
}

static int __init exynos8890_devfreq_mif_init_prepare(
		struct exynos_devfreq_data *data)
{
	/* CPUs keep running during s2idle, so retain the constrained live rate. */
	data->suspend_freq = 0;
	data->ops.init = exynos8890_devfreq_mif_init;
	data->ops.exit = exynos8890_devfreq_mif_exit;
	data->ops.get_volt_table = exynos8890_devfreq_mif_get_volt_table;
	data->ops.ppmu_register = exynos8890_mif_ppmu_register;
	data->ops.ppmu_unregister = exynos8890_mif_ppmu_unregister;
	data->ops.get_freq = exynos8890_devfreq_mif_get_freq;
	data->ops.set_freq = exynos8890_devfreq_mif_set_freq;
	data->ops.set_freq_post = exynos8890_devfreq_mif_set_freq_post;
	data->ops.init_freq_table = exynos8890_devfreq_mif_init_freq_table;
	data->ops.cl_dvfs_start = exynos8890_devfreq_cl_dvfs_start;
	data->ops.cl_dvfs_stop = exynos8890_devfreq_cl_dvfs_stop;
	data->ops.reboot = exynos8890_devfreq_mif_reboot;
	data->ops.resume = exynos8890_devfreq_mif_resume;
	data->ops.pm_suspend_prepare = exynos8890_devfreq_mif_pm_suspend_prepare;
	data->ops.cmu_dump = exynos8890_devfreq_mif_cmu_dump;
	return 0;
}

static int __init exynos8890_devfreq_mif_initcall(void)
{
	return register_exynos_devfreq_init_prepare(
		DEVFREQ_MIF, exynos8890_devfreq_mif_init_prepare);
}
fs_initcall(exynos8890_devfreq_mif_initcall);
