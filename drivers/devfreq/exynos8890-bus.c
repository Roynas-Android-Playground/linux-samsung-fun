// SPDX-License-Identifier: GPL-2.0-only
/* Native OPP/devfreq driver for Exynos8890 non-memory bus domains. */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/reboot.h>
#include <linux/soc/samsung/exynos8890-apm.h>
#include <linux/soc/samsung/exynos8890-calibration.h>

struct exynos8890_bus_soc_data {
	enum exynos8890_calib_domain_id id;
	unsigned long ceiling_rate;
};

struct exynos8890_bus {
	struct device *dev;
	struct clk *clk;
	struct devfreq *devfreq;
	struct devfreq_dev_profile profile;
	const struct exynos8890_calib_domain *calib;
	struct notifier_block reboot_nb;
	unsigned long current_rate;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long ceiling_rate;
};

static const char * const exynos8890_bus_regulators[] = { "vdd", NULL };
static const char * const exynos8890_bus_clocks[] = { "bus", NULL };

static void exynos8890_bus_remove_opps(void *data)
{
	dev_pm_opp_remove_all_dynamic(data);
}

static int exynos8890_bus_target(struct device *dev, unsigned long *rate,
				 u32 flags)
{
	struct exynos8890_bus *bus = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int ret;

	*rate = min(*rate, bus->max_rate);
	opp = devfreq_recommended_opp(dev, rate, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	ret = dev_pm_opp_set_rate(dev, *rate);
	if (!ret)
		bus->current_rate = *rate;
	else
		dev_err_ratelimited(dev,
				    "aggregate clock transition to %lu Hz failed: %d\n",
				    *rate, ret);
	return ret;
}

static int exynos8890_bus_get_cur_freq(struct device *dev,
				       unsigned long *rate)
{
	struct exynos8890_bus *bus = dev_get_drvdata(dev);

	*rate = clk_get_rate(bus->clk);
	return *rate ? 0 : -EIO;
}

static int exynos8890_bus_add_opps(struct exynos8890_bus *bus)
{
	struct dev_pm_opp_data opp = { .level = OPP_LEVEL_UNSET };
	unsigned int i;
	int count = 0;
	int ret;

	for (i = 0; i < bus->calib->num_opps; i++) {
		if (!bus->calib->opps[i].enabled)
			continue;
		if (bus->calib->opps[i].rate_hz < bus->calib->min_rate_hz ||
		    bus->calib->opps[i].rate_hz > bus->calib->max_rate_hz)
			continue;
		if (bus->calib->opps[i].rate_hz > bus->ceiling_rate)
			continue;

		opp.freq = bus->calib->opps[i].rate_hz;
		opp.u_volt = bus->calib->opps[i].voltage_uv;
		ret = dev_pm_opp_add_dynamic(bus->dev, &opp);
		if (ret) {
			dev_pm_opp_remove_all_dynamic(bus->dev);
			return ret;
		}
		if (!bus->min_rate || opp.freq < bus->min_rate)
			bus->min_rate = opp.freq;
		bus->max_rate = max(bus->max_rate, opp.freq);
		count++;
	}

	return count ? 0 : -ENODATA;
}

static int exynos8890_bus_reboot(struct notifier_block *nb,
				 unsigned long action, void *unused)
{
	struct exynos8890_bus *bus =
		container_of(nb, struct exynos8890_bus, reboot_nb);
	int ret;

	/* Freeze the governor at the board-validated handoff rate. */
	ret = devfreq_suspend_device(bus->devfreq);
	if (ret)
		goto out_error;
	mutex_lock(&bus->devfreq->lock);
	ret = dev_pm_opp_set_rate(bus->dev, bus->max_rate);
	if (!ret)
		bus->current_rate = bus->max_rate;
	mutex_unlock(&bus->devfreq->lock);
	if (!ret)
		return NOTIFY_OK;

out_error:
	if (ret)
		dev_crit(bus->dev, "failed to set reboot rate %lu Hz: %d\n",
			 bus->max_rate, ret);

	return notifier_from_errno(ret);
}

static void exynos8890_bus_unregister_reboot(void *data)
{
	struct exynos8890_bus *bus = data;

	unregister_reboot_notifier(&bus->reboot_nb);
}

static int exynos8890_bus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dev_pm_opp_config opp_config = {
		.regulator_names = exynos8890_bus_regulators,
		.clk_names = exynos8890_bus_clocks,
	};
	const struct exynos8890_bus_soc_data *soc_data =
		of_device_get_match_data(dev);
	struct exynos8890_bus *bus;
	unsigned long initial_rate;
	unsigned long initial_target;
	int resume_level;
	int ret;

	if (!soc_data)
		return -EINVAL;
	if (!exynos8890_apm_dvfs_ready())
		return -EPROBE_DEFER;

	bus = devm_kzalloc(dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;
	bus->dev = dev;
	bus->ceiling_rate = soc_data->ceiling_rate;
	bus->calib = exynos8890_calib_get_domain(soc_data->id);
	if (IS_ERR(bus->calib))
		return dev_err_probe(dev, PTR_ERR(bus->calib),
				     "calibration data is unavailable\n");

	bus->clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(bus->clk))
		return dev_err_probe(dev, PTR_ERR(bus->clk),
				     "failed to get aggregate clock\n");

	ret = devm_pm_opp_set_config(dev, &opp_config);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to configure OPP core\n");
	ret = exynos8890_bus_add_opps(bus);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add calibrated OPPs\n");
	ret = devm_add_action_or_reset(dev, exynos8890_bus_remove_opps, dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, bus);
	initial_rate = clk_get_rate(bus->clk);
	if (!initial_rate)
		return dev_err_probe(dev, -EIO,
				     "aggregate clock has no live rate\n");
	initial_target = min(initial_rate, bus->max_rate);
	ret = dev_pm_opp_set_rate(dev, initial_target);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set initial OPP\n");
	initial_rate = clk_get_rate(bus->clk);
	if (!initial_rate || initial_rate > bus->max_rate)
		return dev_err_probe(dev, -EIO,
				     "initial rate violates board ceiling\n");
	bus->current_rate = initial_rate;

	bus->profile.initial_freq = initial_rate;
	bus->profile.polling_ms = 0;
	bus->profile.target = exynos8890_bus_target;
	bus->profile.get_cur_freq = exynos8890_bus_get_cur_freq;
	bus->profile.is_cooling_device = true;
	bus->devfreq = devm_devfreq_add_device(dev, &bus->profile,
						 DEVFREQ_GOV_PERFORMANCE, NULL);
	if (IS_ERR(bus->devfreq))
		return dev_err_probe(dev, PTR_ERR(bus->devfreq),
				     "failed to register devfreq\n");

	ret = devm_devfreq_register_opp_notifier(dev, bus->devfreq);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register OPP notifier\n");
	bus->reboot_nb.notifier_call = exynos8890_bus_reboot;
	ret = register_reboot_notifier(&bus->reboot_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register reboot handoff\n");
	ret = devm_add_action_or_reset(dev, exynos8890_bus_unregister_reboot,
				       bus);
	if (ret)
		return ret;

	resume_level = bus->calib->resume_level;
	if (resume_level >= 0 &&
	    (unsigned int)resume_level < bus->calib->num_opps &&
	    bus->calib->opps[resume_level].enabled &&
	    bus->calib->opps[resume_level].rate_hz >= bus->calib->min_rate_hz &&
	    bus->calib->opps[resume_level].rate_hz <= bus->calib->max_rate_hz &&
	    bus->calib->opps[resume_level].rate_hz <= bus->max_rate)
		bus->devfreq->suspend_freq =
			bus->calib->opps[resume_level].rate_hz;
	/*
	 * The driver PM callbacks nest with the global devfreq suspend pass.
	 * The final global resume consumes this value after suspend_count reaches
	 * zero; the performance governor itself has no DEVFREQ_GOV_RESUME action.
	 */
	bus->devfreq->resume_freq = bus->max_rate;

	dev_info(dev, "%s: %lu-%lu Hz, board ceiling %lu Hz, fixed-safe policy\n",
		 bus->calib->name, bus->min_rate, bus->max_rate,
		 bus->ceiling_rate);
	return 0;
}

static int exynos8890_bus_suspend(struct device *dev)
{
	struct exynos8890_bus *bus = dev_get_drvdata(dev);

	return devfreq_suspend_device(bus->devfreq);
}

static int exynos8890_bus_resume(struct device *dev)
{
	struct exynos8890_bus *bus = dev_get_drvdata(dev);

	return devfreq_resume_device(bus->devfreq);
}

static DEFINE_SIMPLE_DEV_PM_OPS(exynos8890_bus_pm_ops,
				exynos8890_bus_suspend, exynos8890_bus_resume);

static const struct exynos8890_bus_soc_data exynos8890_int_data = {
	.id = EXYNOS8890_CALIB_INT,
	.ceiling_rate = 690000000,
};

static const struct exynos8890_bus_soc_data exynos8890_cam_data = {
	.id = EXYNOS8890_CALIB_CAM,
	.ceiling_rate = 600000000,
};

static const struct exynos8890_bus_soc_data exynos8890_disp_data = {
	.id = EXYNOS8890_CALIB_DISP,
	.ceiling_rate = 400000000,
};

static const struct of_device_id exynos8890_bus_of_match[] = {
	{ .compatible = "samsung,exynos8890-int-devfreq",
	  .data = &exynos8890_int_data },
	{ .compatible = "samsung,exynos8890-cam-devfreq",
	  .data = &exynos8890_cam_data },
	{ .compatible = "samsung,exynos8890-disp-devfreq",
	  .data = &exynos8890_disp_data },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_bus_of_match);

static struct platform_driver exynos8890_bus_driver = {
	.probe = exynos8890_bus_probe,
	.driver = {
		.name = "exynos8890-bus-devfreq",
		.of_match_table = exynos8890_bus_of_match,
		.pm = pm_sleep_ptr(&exynos8890_bus_pm_ops),
	},
};
module_platform_driver(exynos8890_bus_driver);

MODULE_DESCRIPTION("Exynos8890 native aggregate bus devfreq driver");
MODULE_LICENSE("GPL");
