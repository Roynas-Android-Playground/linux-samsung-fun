// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX86900 / MAX86907 optical heart-rate (PPG) sensors
 *
 * MAX86900 support originates in Samsung's drivers/optics/max86900.c.
 * MAX86907 register, FLEX slot and sample sequences come from Samsung's
 * universal8890 drivers/sensorhub/brcm/max86902.{c,h}. The compatible
 * selects the variant: no address probing, OTP access or factory tests.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
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

/* MAX86907 uses the MAX86902 register map, not the MAX86900 map. */
#define MAX86907_INT_STATUS_2		0x01
#define MAX86907_INT_ENABLE		0x02
#define MAX86907_INT_ENABLE_2		0x03
#define MAX86907_FIFO_WR_PTR		0x04
#define MAX86907_OVF_COUNTER		0x05
#define MAX86907_FIFO_RD_PTR		0x06
#define MAX86907_FIFO_DATA		0x07
#define MAX86907_FIFO_CONFIG		0x08
#define MAX86907_MODE_CONFIG		0x09
#define MAX86907_SPO2_CONFIG		0x0a
#define MAX86907_LED1_PA			0x0c
#define MAX86907_LED2_PA			0x0d
#define MAX86907_FLEX_CONTROL_1		0x11
#define MAX86907_FLEX_CONTROL_2		0x12
#define MAX86907_TEMP_INTEGER		0x1f
#define MAX86907_TEMP_CONFIG		0x21
#define MAX86907_REV_ID			0xfe
#define MAX86907_PART_ID			0xff

static const bool max86900_variant;
static const bool max86907_variant = true;

struct max86900_data {
	struct i2c_client *client;
	struct regmap *regmap;
	struct input_dev *input;
	/* Serializes requested state, PM, power and IRQ transitions. */
	struct mutex lock;
	int irq;
	bool enabled;
	bool is_max86907;
	bool powered;
	bool running;
	bool suspended;
	bool removing;
	struct regulator_bulk_data supplies[2];
	int temp;
	/* IRQ-owned while running; transitions clear these with IRQ disabled. */
	bool temp_read_pending;
	bool temp_rearm_pending;
};

static int max86900_read_temperature(struct max86900_data *max)
{
	u8 buf[2];
	int ret;

	ret = regmap_raw_read(max->regmap, max->is_max86907 ?
			      MAX86907_TEMP_INTEGER : MAX86900_TEMP_INTEGER,
			      buf, sizeof(buf));
	if (ret)
		return ret;

	max->temp = (s8)buf[0] * 16 + buf[1];
	return 0;
}

static irqreturn_t max86900_irq_thread(int irq, void *data)
{
	struct max86900_data *max = data;
	u8 buf[6];
	unsigned int first, second, status, status2;
	int ret;

	if (max->is_max86907) {
		/* Both status registers are clear-on-read. */
		ret = regmap_read(max->regmap, MAX86900_INT_STATUS, &status);
		if (ret)
			return IRQ_NONE;
		ret = regmap_read(max->regmap, MAX86907_INT_STATUS_2, &status2);
		/* Failed optional status reads must not skip PPG or pending work. */
		/* Preserve the obligation after the clear-on-read ready bit is gone. */
		if (!ret && (status2 & 0x02))
			max->temp_read_pending = true;
		if (max->temp_read_pending) {
			ret = max86900_read_temperature(max);
			if (!ret) {
				max->temp_read_pending = false;
				max->temp_rearm_pending = true;
			}
		}
		if (max->temp_rearm_pending && !max->temp_read_pending) {
			ret = regmap_write(max->regmap, MAX86907_TEMP_CONFIG, 0x01);
			if (!ret)
				max->temp_rearm_pending = false;
		}
		/* Temperature is optional: errors must not discard a ready PPG. */
		if (!(status & 0x40))
			return IRQ_HANDLED;

		/* Two active FLEX slots, each an 18-bit value in three bytes. */
		ret = regmap_raw_read(max->regmap, MAX86907_FIFO_DATA, buf, 6);
		if (ret)
			return IRQ_NONE;
		first = ((buf[0] & 0x03) << 16) | (buf[1] << 8) | buf[2];
		second = ((buf[3] & 0x03) << 16) | (buf[4] << 8) | buf[5];
	} else {
		ret = regmap_raw_read(max->regmap, MAX86900_FIFO_DATA, buf, 4);
		if (ret)
			return IRQ_NONE;
		first = (buf[0] << 8) | buf[1];
		second = (buf[2] << 8) | buf[3];
	}

	/* Keep Samsung's slot order and the existing +1 REL ABI. */
	input_report_rel(max->input, REL_X, first + 1);
	input_report_rel(max->input, REL_Y, second + 1);
	input_report_rel(max->input, REL_Z, max->temp + 1);
	input_sync(max->input);

	return IRQ_HANDLED;
}

static int max86900_chip_init(struct max86900_data *max)
{
	int ret;
	unsigned int status;

	if (max->is_max86907) {
		/* DT selects the OTP subvariant; ordinary IDs only validate it. */
		ret = regmap_read(max->regmap, MAX86907_PART_ID, &status);
		if (ret)
			return ret;
		if (status != 0x15)
			return -ENODEV;
		ret = regmap_read(max->regmap, MAX86907_REV_ID, &status);
		if (ret)
			return ret;
		if (status != 0x03)
			return -ENODEV;

		ret = regmap_write(max->regmap, MAX86907_MODE_CONFIG, 0x40);
		if (ret)
			return ret;
		ret = regmap_read_poll_timeout(max->regmap, MAX86907_MODE_CONFIG,
					       status, !(status & 0x40), 1000, 20000);
		if (ret)
			return ret;
		ret = regmap_read(max->regmap, MAX86900_INT_STATUS, &status);
		if (ret)
			return ret;
		ret = regmap_read(max->regmap, MAX86907_INT_STATUS_2, &status);
		if (ret)
			return ret;

		return regmap_write(max->regmap, MAX86907_MODE_CONFIG, 0x80);
	}

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x40);
	if (ret) {
		dev_err(&max->client->dev, "failed to reset chip: %d\n", ret);
		return ret;
	}

	/* clear latched interrupt status */
	ret = regmap_read(max->regmap, MAX86900_INT_STATUS, &status);
	if (ret) {
		dev_err(&max->client->dev, "failed to read interrupt status: %d\n", ret);
		return ret;
	}

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x83);
	if (ret) {
		dev_err(&max->client->dev, "failed to write mode config: %d\n", ret);
		return ret;
	}

	ret = regmap_write(max->regmap, MAX86900_INT_ENABLE, 0x10);
	if (ret) {
		dev_err(&max->client->dev, "failed to write interrupt enable: %d\n", ret);
		return ret;
	}

	/* 400 Hz sample rate, 400us LED pulse width */
	ret = regmap_write(max->regmap, MAX86900_SPO2_CONFIG, 0x51);
	if (ret) {
		dev_err(&max->client->dev, "failed to write SPO2 config: %d\n", ret);
		return ret;
	}

	ret = regmap_write(max->regmap, MAX86900_LED_CONFIG, 0x00);
	if (ret) {
		dev_err(&max->client->dev, "failed to write LED config: %d\n", ret);
		return ret;
	}

	return 0;
}

static int max86907_start(struct max86900_data *max)
{
	/*
	 * Samsung brcm/max86902.c:max86902_hrm_enable(), non-notch path:
	 * LED2 then LED1, 400 Hz with four-sample FIFO averaging (100 Hz).
	 * Keep the vendor slot/current pairing, not its conflicting colour
	 * comments. Only the two enabled slots are read or reported.
	 */
	static const struct {
		u8 reg;
		u8 val;
	} init[] = {
		{ MAX86907_LED1_PA, 0x00 },
		{ MAX86907_LED2_PA, 0xff },
		{ MAX86907_INT_ENABLE, 0x40 },
		{ MAX86907_INT_ENABLE_2, 0x00 },
		{ MAX86907_FLEX_CONTROL_1, 0x12 },
		{ MAX86907_FLEX_CONTROL_2, 0x00 },
		{ MAX86907_SPO2_CONFIG, 0x6e },
		{ MAX86907_FIFO_CONFIG, 0x40 },
		{ MAX86907_FIFO_WR_PTR, 0x00 },
		{ MAX86907_OVF_COUNTER, 0x00 },
		{ MAX86907_FIFO_RD_PTR, 0x00 },
		{ MAX86907_MODE_CONFIG, 0x07 },
		{ MAX86907_TEMP_CONFIG, 0x01 },
	};
	unsigned int i;
	int ret;

	max->temp = 0;
	max->temp_read_pending = false;
	max->temp_rearm_pending = false;
	for (i = 0; i < ARRAY_SIZE(init); i++) {
		ret = regmap_write(max->regmap, init[i].reg, init[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int max86900_start(struct max86900_data *max)
{
	int ret;

	ret = regmap_write(max->regmap, MAX86900_LED_CONFIG, MAX86900_DEFAULT_LED_CURRENT);
	if (ret) {
		dev_err(&max->client->dev, "failed to write LED config: %d\n", ret);
		goto out;
	}

	ret = regmap_write(max->regmap, MAX86900_FIFO_WR_PTR, 0x00);
	if (ret) {
		dev_err(&max->client->dev, "failed to write FIFO write pointer: %d\n", ret);
		goto out;
	}
	ret = regmap_write(max->regmap, MAX86900_OVF_COUNTER, 0x00);
	if (ret) {
		dev_err(&max->client->dev, "failed to write overflow counter: %d\n", ret);
		goto out;
	}
	ret = regmap_write(max->regmap, MAX86900_FIFO_RD_PTR, 0x00);
	if (ret) {
		dev_err(&max->client->dev, "failed to write FIFO read pointer: %d\n", ret);
		goto out;
	}

	ret = max86900_read_temperature(max);
	if (ret) {
		dev_err(&max->client->dev, "failed to read temperature: %d\n", ret);
		goto out;
	}

	ret = regmap_write(max->regmap, MAX86900_MODE_CONFIG, 0x0b);
	if (ret) {
		dev_err(&max->client->dev, "failed to write mode config: %d\n", ret);
		goto out;
	}

out:
	return ret;
}

static int max86900_power_on(struct max86900_data *max)
{
	int ret;

	if (max->powered)
		return 0;
	ret = regulator_bulk_enable(ARRAY_SIZE(max->supplies), max->supplies);
	if (ret)
		return ret;
	max->powered = true;
	usleep_range(1000, 1100);
	return 0;
}

static int max86900_power_off(struct max86900_data *max)
{
	int ret;

	if (!max->powered)
		return 0;
	ret = regulator_bulk_disable(ARRAY_SIZE(max->supplies), max->supplies);
	if (!ret)
		max->powered = false;
	return ret;
}

/* Caller holds lock. IRQ never takes it; disable_irq() drains the thread. */
static int max86900_stop_locked(struct max86900_data *max)
{
	unsigned int mode = max->is_max86907 ? MAX86907_MODE_CONFIG :
					     MAX86900_MODE_CONFIG;
	unsigned int status;
	int ret, err;

	if (max->running) {
		disable_irq(max->irq);
		max->running = false;
	}
	max->temp_read_pending = false;
	max->temp_rearm_pending = false;
	if (!max->powered)
		return 0;

	ret = regmap_write(max->regmap, mode, 0x40);
	if (!ret && max->is_max86907)
		ret = regmap_read_poll_timeout(max->regmap, mode, status,
					       !(status & 0x40), 1000, 20000);
	/* Attempt shutdown and rail release even when reset fails. */
	err = regmap_write(max->regmap, mode, 0x80);
	if (!ret)
		ret = err;
	err = max86900_power_off(max);
	return ret ? ret : err;
}

static int max86900_start_locked(struct max86900_data *max)
{
	int ret, err;

	if (max->running)
		return 0;
	ret = max86900_power_on(max);
	if (ret)
		return ret;
	/* Reset/stop and power loss both destroy the configuration. */
	ret = max86900_chip_init(max);
	if (ret)
		goto fail;
	ret = max->is_max86907 ? max86907_start(max) : max86900_start(max);
	if (ret)
		goto fail;
	max->running = true;
	enable_irq(max->irq);
	return 0;

fail:
	err = max86900_stop_locked(max);
	if (err)
		dev_err(&max->client->dev, "start cleanup failed: %d\n", err);
	return ret;
}

static int max86900_enable(struct max86900_data *max)
{
	int ret;

	mutex_lock(&max->lock);
	if (max->suspended || max->removing) {
		ret = -EBUSY;
		goto out;
	}
	ret = max86900_start_locked(max);
	if (!ret)
		max->enabled = true;
out:
	mutex_unlock(&max->lock);
	return ret;
}

static int max86900_disable(struct max86900_data *max)
{
	int ret;

	mutex_lock(&max->lock);
	ret = max86900_stop_locked(max);
	max->enabled = false;
	mutex_unlock(&max->lock);
	return ret;
}

static int max86900_suspend(struct device *dev)
{
	struct max86900_data *max = dev_get_drvdata(dev);
	int ret, err;

	mutex_lock(&max->lock);
	ret = max86900_stop_locked(max);
	if (ret) {
		/* A failed suspend must leave the pre-suspend request usable. */
		if (max->enabled) {
			err = max86900_start_locked(max);
			if (err)
				dev_err(dev, "failed to restore sampling: %d\n", err);
		}
	} else {
		max->suspended = true;
	}
	mutex_unlock(&max->lock);
	return ret;
}

static int max86900_resume(struct device *dev)
{
	struct max86900_data *max = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&max->lock);
	max->suspended = false;
	if (max->enabled)
		ret = max86900_start_locked(max);
	mutex_unlock(&max->lock);
	return ret;
}

static void max86900_cleanup(void *data)
{
	struct max86900_data *max = data;
	int ret;

	mutex_lock(&max->lock);
	max->removing = true;
	max->enabled = false;
	ret = max86900_stop_locked(max);
	if (ret)
		dev_err(&max->client->dev, "failed to stop sensor: %d\n", ret);
	mutex_unlock(&max->lock);
}

/* Also covers failures before the IRQ and its quiesce action exist. */
static void max86900_release_supplies(void *data)
{
	struct max86900_data *max = data;
	int ret;

	ret = max86900_power_off(max);
	if (ret)
		dev_err(&max->client->dev, "failed to release supplies: %d\n", ret);
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct max86900_data *max = dev_get_drvdata(dev);
	bool enabled;

	mutex_lock(&max->lock);
	enabled = max->enabled;
	mutex_unlock(&max->lock);
	return sysfs_emit(buf, "%d\n", enabled);
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
	int ret;

	max = devm_kzalloc(dev, sizeof(*max), GFP_KERNEL);
	if (!max)
		return -ENOMEM;

	if (!i2c_get_match_data(client))
		return -EINVAL;
	max->is_max86907 = *(const bool *)i2c_get_match_data(client);
	max->client = client;
	mutex_init(&max->lock);
	i2c_set_clientdata(client, max);

	max->regmap = devm_regmap_init_i2c(client, &max86900_regmap_config);
	if (IS_ERR(max->regmap))
		return PTR_ERR(max->regmap);

	max->supplies[0].supply = "vdd";
	max->supplies[1].supply = "led";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(max->supplies), max->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");
	ret = devm_add_action_or_reset(dev, max86900_release_supplies, max);
	if (ret)
		return ret;
	ret = max86900_power_on(max);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supplies\n");
	ret = max86900_chip_init(max);
	if (ret)
		return dev_err_probe(dev, ret, "chip identification/init failed\n");
	ret = max86900_power_off(max);
	if (ret)
		return dev_err_probe(dev, ret, "failed to disable supplies\n");

	max->input = devm_input_allocate_device(dev);
	if (!max->input)
		return -ENOMEM;

	max->input->name = "hrm_sensor";
	max->input->id.bustype = BUS_I2C;
	input_set_capability(max->input, EV_REL, REL_X);
	input_set_capability(max->input, EV_REL, REL_Y);
	input_set_capability(max->input, EV_REL, REL_Z);
	input_set_drvdata(max->input, max);

	ret = input_register_device(max->input);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	max->irq = client->irq;
	ret = devm_request_threaded_irq(dev, max->irq, NULL, max86900_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"max86900", max);
	if (ret) {
		dev_err(dev, "failed to request IRQ: %d\n", ret);
		return ret;
	}
	/* Runs before IRQ/input devres release, including failed probe. */
	return devm_add_action_or_reset(dev, max86900_cleanup, max);
}

static void max86900_shutdown(struct i2c_client *client)
{
	max86900_cleanup(i2c_get_clientdata(client));
}

static DEFINE_SIMPLE_DEV_PM_OPS(max86900_pm_ops, max86900_suspend, max86900_resume);

static const struct of_device_id max86900_of_match[] = {
	{ .compatible = "maxim,max86900", .data = &max86900_variant },
	{ .compatible = "maxim,max86907", .data = &max86907_variant },
	{ }
};
MODULE_DEVICE_TABLE(of, max86900_of_match);

static const struct i2c_device_id max86900_id[] = {
	{ "max86900", (kernel_ulong_t)&max86900_variant },
	{ "max86907", (kernel_ulong_t)&max86907_variant },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max86900_id);

static struct i2c_driver max86900_driver = {
	.driver = {
		.name = "max86900",
		.of_match_table = max86900_of_match,
		.dev_groups = max86900_groups,
		.pm = pm_sleep_ptr(&max86900_pm_ops),
	},
	.probe = max86900_probe,
	.shutdown = max86900_shutdown,
	.id_table = max86900_id,
};
module_i2c_driver(max86900_driver);

MODULE_DESCRIPTION("Maxim MAX86900/MAX86907 heart-rate sensors");
MODULE_LICENSE("GPL");
