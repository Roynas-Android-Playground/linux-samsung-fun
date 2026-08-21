// SPDX-License-Identifier: GPL-2.0-only
/*
 * Conservative IDT P9220 wireless-power receiver enumeration.
 *
 * This deliberately does not access receiver registers or implement charging
 * policy. It only reports the physical pad-detect input so userspace and the
 * power-supply core can enumerate the fitted receiver safely.
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>

struct p9220_enum {
	struct gpio_desc *detect;
	struct power_supply *psy;
};

static irqreturn_t p9220_enum_detect_irq(int irq, void *data)
{
	struct p9220_enum *p9220 = data;

	power_supply_changed(p9220->psy);
	return IRQ_HANDLED;
}

static enum power_supply_property p9220_enum_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int p9220_get_property(struct power_supply *psy,
			      enum power_supply_property property,
			      union power_supply_propval *val)
{
	struct p9220_enum *p9220 = power_supply_get_drvdata(psy);

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = gpiod_get_value_cansleep(p9220->detect);
		return val->intval < 0 ? val->intval : 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "IDT";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "P9220";
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc p9220_enum_desc = {
	.name = "p9220-wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = p9220_enum_properties,
	.num_properties = ARRAY_SIZE(p9220_enum_properties),
	.get_property = p9220_get_property,
};

static int p9220_enum_probe(struct i2c_client *client)
{
	struct power_supply_config config = {};
	struct p9220_enum *p9220;
	int irq, ret;

	p9220 = devm_kzalloc(&client->dev, sizeof(*p9220), GFP_KERNEL);
	if (!p9220)
		return -ENOMEM;

	p9220->detect = devm_gpiod_get(&client->dev, "detect", GPIOD_IN);
	if (IS_ERR(p9220->detect))
		return dev_err_probe(&client->dev, PTR_ERR(p9220->detect),
				     "failed to get pad-detect GPIO\n");

	config.drv_data = p9220;
	config.fwnode = dev_fwnode(&client->dev);
	p9220->psy = devm_power_supply_register(&client->dev, &p9220_enum_desc,
						&config);
	if (IS_ERR(p9220->psy))
		return PTR_ERR(p9220->psy);

	irq = gpiod_to_irq(p9220->detect);
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(&client->dev, irq, NULL,
					p9220_enum_detect_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING,
					dev_name(&client->dev), p9220);
	return ret;
}

static const struct of_device_id p9220_enum_of_match[] = {
	{ .compatible = "idt,p9220" },
	{ }
};
MODULE_DEVICE_TABLE(of, p9220_enum_of_match);

static struct i2c_driver p9220_enum_driver = {
	.driver = {
		.name = "p9220-enum",
		.of_match_table = p9220_enum_of_match,
	},
	.probe = p9220_enum_probe,
};
module_i2c_driver(p9220_enum_driver);

MODULE_DESCRIPTION("IDT P9220 wireless receiver enumeration");
MODULE_LICENSE("GPL");
