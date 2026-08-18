// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only MAX77854 fuel-gauge support.
 *
 * The initial driver deliberately leaves model programming, learned state,
 * alerts, current sensing and temperature compensation to a later stage.
 */

#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77843-private.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

struct max77854_fg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
};

static enum power_supply_property max77854_fg_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int max77854_fg_read(struct max77854_fg *fg, unsigned int reg,
			    unsigned int *value)
{
	int ret;

	ret = regmap_read(fg->regmap, reg, value);
	if (ret)
		dev_err_ratelimited(fg->dev, "failed to read register %#x: %d\n",
				    reg, ret);

	return ret;
}

static int max77854_fg_read_voltage(struct max77854_fg *fg,
				    unsigned int reg, int *microvolts)
{
	unsigned int raw;
	int ret;

	ret = max77854_fg_read(fg, reg, &raw);
	if (ret)
		return ret;

	*microvolts = raw * 625 / 8;
	return 0;
}

static int max77854_fg_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct max77854_fg *fg = power_supply_get_drvdata(psy);
	unsigned int raw, status;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = max77854_fg_read(fg, MAX77843_FG_REG_STATUS, &status);
		if (ret)
			return ret;
		val->intval = !(status & MAX77843_FG_STATUS_BATTERY_ABSENT);
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return max77854_fg_read_voltage(fg, MAX77843_FG_REG_VCELL,
						&val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		return max77854_fg_read_voltage(fg, MAX77843_FG_REG_AVG_VCELL,
						&val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		return max77854_fg_read_voltage(fg, MAX77843_FG_REG_VFOCV,
						&val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = max77854_fg_read(fg, MAX77843_FG_REG_STATUS, &status);
		if (ret)
			return ret;
		if (status & MAX77843_FG_STATUS_POR)
			return -ENODATA;

		ret = max77854_fg_read(fg, MAX77843_FG_REG_SOCREP, &raw);
		if (ret)
			return ret;
		val->intval = min(raw >> 8, 100U);
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MAX77854";
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Maxim Integrated";
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc max77854_fg_desc = {
	.name = "max77854-fuel-gauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = max77854_fg_properties,
	.num_properties = ARRAY_SIZE(max77854_fg_properties),
	.get_property = max77854_fg_get_property,
};

static int max77854_fg_probe(struct platform_device *pdev)
{
	struct max77693_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct max77854_fg *fg;
	unsigned int status;
	int ret;

	if (!iodev || iodev->type != TYPE_MAX77854 ||
	    IS_ERR_OR_NULL(iodev->regmap_fg))
		return -ENODEV;

	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->dev = &pdev->dev;
	fg->regmap = iodev->regmap_fg;
	platform_set_drvdata(pdev, fg);

	ret = max77854_fg_read(fg, MAX77843_FG_REG_STATUS, &status);
	if (ret)
		return ret;
	if (status & MAX77843_FG_STATUS_POR)
		dev_warn(&pdev->dev,
			 "POR set; capacity unavailable until model initialization\n");

	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);
	fg->psy = devm_power_supply_register(&pdev->dev, &max77854_fg_desc,
					     &psy_cfg);
	return PTR_ERR_OR_ZERO(fg->psy);
}

static const struct of_device_id max77854_fg_of_match[] = {
	{ .compatible = "maxim,max77854-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77854_fg_of_match);

static struct platform_driver max77854_fg_driver = {
	.probe = max77854_fg_probe,
	.driver = {
		.name = "max77854-fuel-gauge",
		.of_match_table = max77854_fg_of_match,
	},
};
module_platform_driver(max77854_fg_driver);

MODULE_DESCRIPTION("Read-only MAX77854 fuel-gauge driver");
MODULE_LICENSE("GPL");
