// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX86900 optical heart-rate (PPG) sensor
 *
 * Register map and init/enable sequence ported from Samsung's GPL vendor
 * driver (drivers/optics/max86900.c). Variant detection (MAX86900A/B/C at
 * the alternate 0x57 address) and the EOL factory test path aren't ported -
 * this only drives the base MAX86900 at 0x51, which is what's populated on
 * herolte.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define MAX86900_INT_STATUS		0x00
#define MAX86900_INT_ENABLE		0x01
#define MAX86900_FIFO_WR_PTR		0x02
#define MAX86900_OVF_COUNTER		0x03
#define MAX86900_FIFO_RD_PTR		0x04
#define MAX86900_FIFO_DATA		0x05
#define MAX86900_MODE_CONFIG		0x06
#define MAX86900_SPO2_CONFIG		0x07
#define MAX86900_LED_CONFIG		0x09
#define MAX86900_TEMP_INTEGER		0x16
#define MAX86900_TEMP_FRACTION		0x17

#define MAX86900_DEFAULT_LED_CURRENT	0x55

struct max86900_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct input_dev *input;
	struct mutex lock;
	int irq;
	bool enabled;
	int temp;
};

static int max86900_read_temperature(struct max86900_data *max)
{
	u8 buf[2];
	int ret;

	ret = regmap_raw_read(max->regmap, MAX86900_TEMP_INTEGER, buf, sizeof(buf));
	if (ret)
		return ret;

	max->temp = (s8)buf[0] * 16 + buf[1];
	return 0;
}

static irqreturn_t max86900_irq_thread(int irq, void *data)
{
	struct max86900_data *max = data;
	u8 buf[4];
	u16 ir, red;
	int ret;

	ret = regmap_raw_read(max->regmap, MAX86900_FIFO_DATA, buf, sizeof(buf));
	if (ret)
		return IRQ_NONE;

	ir = (buf[0] << 8) | buf[1];
	red = (buf[2] << 8) | buf[3];

	/* +1 so a raw 0 sample still produces a nonzero, always-reported delta */
	input_report_rel(max->input, REL_X, ir + 1);
	input_report_rel(max->input, REL_Y, red + 1);
	input_report_rel(max->input, REL_Z, max->temp + 1);
	input_sync(max->input);

	return IRQ_HANDLED;
}

static int max86900_chip_init(struct max86900_data *max)
{
	int ret;
	unsigned int status;

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x40);
	if (ret)
		return ret;

	/* clear latched interrupt status */
	ret = regmap_read(max->regmap, MAX86900_INT_STATUS, &status);
	if (ret)
		return ret;

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x83);
	if (ret)
		return ret;

	ret = regmap_write(max->regmap, MAX86900_INT_ENABLE, 0x10);
	if (ret)
		return ret;

	/* 400 Hz sample rate, 400us LED pulse width */
	ret = regmap_write(max->regmap, MAX86900_SPO2_CONFIG, 0x51);
	if (ret)
		return ret;

	return regmap_write(max->regmap, MAX86900_LED_CONFIG, 0x00);
}

static int max86900_enable(struct max86900_data *max)
{
	int ret;

	mutex_lock(&max->lock);

	ret = regmap_write(max->regmap, MAX86900_LED_CONFIG, MAX86900_DEFAULT_LED_CURRENT);
	if (ret)
		goto out;

	ret = regmap_write(max->regmap, MAX86900_FIFO_WR_PTR, 0x00);
	if (ret)
		goto out;
	ret = regmap_write(max->regmap, MAX86900_OVF_COUNTER, 0x00);
	if (ret)
		goto out;
	ret = regmap_write(max->regmap, MAX86900_FIFO_RD_PTR, 0x00);
	if (ret)
		goto out;

	ret = max86900_read_temperature(max);
	if (ret)
		goto out;

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x0b);
	if (ret)
		goto out;

	enable_irq(max->irq);
	max->enabled = true;
out:
	mutex_unlock(&max->lock);
	return ret;
}

static int max86900_disable(struct max86900_data *max)
{
	int ret;

	mutex_lock(&max->lock);
	disable_irq(max->irq);

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x40);
	if (!ret)
		ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x80);

	max->enabled = false;
	mutex_unlock(&max->lock);
	return ret;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct max86900_data *max = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", max->enabled);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct max86900_data *max = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	ret = on ? max86900_enable(max) : max86900_disable(max);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enable);

static struct attribute *max86900_attrs[] = {
	&dev_attr_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(max86900);

static const struct regmap_config max86900_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int max86900_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max86900_data *max;
	struct regulator *vdd, *led;
	int ret;

	max = devm_kzalloc(dev, sizeof(*max), GFP_KERNEL);
	if (!max)
		return -ENOMEM;

	max->client = client;
	mutex_init(&max->lock);
	i2c_set_clientdata(client, max);

	max->regmap = devm_regmap_init_i2c(client, &max86900_regmap_config);
	if (IS_ERR(max->regmap))
		return PTR_ERR(max->regmap);

	vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(vdd))
		return dev_err_probe(dev, PTR_ERR(vdd), "failed to get vdd\n");
	led = devm_regulator_get(dev, "led");
	if (IS_ERR(led))
		return dev_err_probe(dev, PTR_ERR(led), "failed to get led\n");

	ret = regulator_enable(vdd);
	if (ret)
		return ret;
	ret = regulator_enable(led);
	if (ret)
		return ret;
	usleep_range(1000, 1100);

	ret = max86900_chip_init(max);
	if (ret) {
		regulator_disable(led);
		regulator_disable(vdd);
		return dev_err_probe(dev, ret, "chip init failed\n");
	}

	max->input = devm_input_allocate_device(dev);
	if (!max->input) {
		ret = -ENOMEM;
		goto fail_regulator;
	}

	max->input->name = "hrm_sensor";
	max->input->id.bustype = BUS_I2C;
	input_set_capability(max->input, EV_REL, REL_X);
	input_set_capability(max->input, EV_REL, REL_Y);
	input_set_capability(max->input, EV_REL, REL_Z);
	input_set_drvdata(max->input, max);

	ret = input_register_device(max->input);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		goto fail_regulator;
	}

	max->irq = client->irq;
	ret = devm_request_threaded_irq(dev, max->irq, NULL, max86900_irq_thread,
					 IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					 "max86900", max);
	if (ret) {
		dev_err(dev, "failed to request IRQ: %d\n", ret);
		goto fail_regulator;
	}
	disable_irq(max->irq);

	return 0;

fail_regulator:
	regulator_disable(led);
	regulator_disable(vdd);
	return ret;
}

static const struct of_device_id max86900_of_match[] = {
	{ .compatible = "maxim,max86900" },
	{ }
};
MODULE_DEVICE_TABLE(of, max86900_of_match);

static struct i2c_driver max86900_driver = {
	.driver = {
		.name = "max86900",
		.of_match_table = max86900_of_match,
		.dev_groups = max86900_groups,
	},
	.probe = max86900_probe,
};
module_i2c_driver(max86900_driver);

MODULE_DESCRIPTION("Maxim MAX86900 heart-rate sensor");
MODULE_LICENSE("GPL");
