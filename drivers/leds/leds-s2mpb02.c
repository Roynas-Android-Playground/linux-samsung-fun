// SPDX-License-Identifier: GPL-2.0-only
/* Conservative torch-only support for the Samsung S2MPB02. */

#include <linux/leds.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/s2mpb02.h>
#include <linux/mfd/s2mpb02-private.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define S2MPB02_TORCH_CURRENT_MASK	GENMASK(3, 0)
#define S2MPB02_TORCH_MODE_MASK		GENMASK(7, 6)
#define S2MPB02_TORCH_MODE_ON		GENMASK(7, 6)
#define S2MPB02_TORCH_STEP_UA		20000
#define S2MPB02_TORCH_LIMIT_UA		100000

struct s2mpb02_torch {
	struct led_classdev led;
	struct i2c_client *i2c;
	struct gpio_desc *enable_gpio;
	struct mutex lock;
};

static int s2mpb02_torch_disable(struct s2mpb02_torch *torch)
{
	gpiod_set_value_cansleep(torch->enable_gpio, 0);
	return s2mpb02_update_reg(torch->i2c, S2MPB02_REG_FLED_CTRL1, 0,
				  S2MPB02_TORCH_MODE_MASK);
}

static int s2mpb02_torch_set(struct led_classdev *led,
			     enum led_brightness brightness)
{
	struct s2mpb02_torch *torch = container_of(led, struct s2mpb02_torch,
						      led);
	int ret;

	mutex_lock(&torch->lock);
	ret = s2mpb02_torch_disable(torch);
	if (ret || !brightness)
		goto out;

	brightness = min(brightness, led->max_brightness);
	ret = s2mpb02_update_reg(torch->i2c, S2MPB02_REG_FLED_CUR1,
				 brightness, S2MPB02_TORCH_CURRENT_MASK);
	if (!ret)
		ret = s2mpb02_update_reg(torch->i2c, S2MPB02_REG_FLED_CTRL1,
					 S2MPB02_TORCH_MODE_ON,
					 S2MPB02_TORCH_MODE_MASK);
out:
	mutex_unlock(&torch->lock);
	return ret;
}

static int s2mpb02_torch_probe(struct platform_device *pdev)
{
	struct s2mpb02_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct s2mpb02_torch *torch;
	u32 max_current;
	int ret;

	if (!iodev)
		return -ENODEV;
	ret = device_property_read_u32(&pdev->dev, "max-current-microamp",
				       &max_current);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing maximum torch current\n");
	if (!max_current || max_current > S2MPB02_TORCH_LIMIT_UA ||
	    max_current % S2MPB02_TORCH_STEP_UA)
		return -EINVAL;

	torch = devm_kzalloc(&pdev->dev, sizeof(*torch), GFP_KERNEL);
	if (!torch)
		return -ENOMEM;
	torch->i2c = iodev->i2c;
	torch->enable_gpio = devm_gpiod_get(&pdev->dev, "torch-enable",
					     GPIOD_OUT_LOW);
	if (IS_ERR(torch->enable_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(torch->enable_gpio),
				     "failed to claim torch-enable GPIO\n");
	mutex_init(&torch->lock);
	torch->led.name = "rear:torch";
	torch->led.max_brightness = max_current / S2MPB02_TORCH_STEP_UA;
	torch->led.brightness_set_blocking = s2mpb02_torch_set;

	ret = s2mpb02_torch_disable(torch);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, torch);
	return led_classdev_register(&pdev->dev, &torch->led);
}

static void s2mpb02_torch_shutdown(struct platform_device *pdev)
{
	struct s2mpb02_torch *torch = platform_get_drvdata(pdev);

	if (torch) {
		led_classdev_suspend(&torch->led);
		mutex_lock(&torch->lock);
		s2mpb02_torch_disable(torch);
		mutex_unlock(&torch->lock);
	}
}

static void s2mpb02_torch_remove(struct platform_device *pdev)
{
	struct s2mpb02_torch *torch = platform_get_drvdata(pdev);

	led_classdev_unregister(&torch->led);
	mutex_lock(&torch->lock);
	s2mpb02_torch_disable(torch);
	mutex_unlock(&torch->lock);
}

static const struct of_device_id s2mpb02_torch_of_match[] = {
	{ .compatible = "samsung,s2mpb02-torch" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpb02_torch_of_match);

static struct platform_driver s2mpb02_torch_driver = {
	.probe = s2mpb02_torch_probe,
	.remove = s2mpb02_torch_remove,
	.shutdown = s2mpb02_torch_shutdown,
	.driver = {
		.name = "s2mpb02-led",
		.of_match_table = s2mpb02_torch_of_match,
	},
};
module_platform_driver(s2mpb02_torch_driver);

MODULE_DESCRIPTION("Samsung S2MPB02 conservative torch driver");
MODULE_LICENSE("GPL");
