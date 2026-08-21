// SPDX-License-Identifier: GPL-2.0-only
/*
 * Conservative MAX77854 notification LED support.
 *
 * Only steady, low-current red, green and blue outputs are exposed. Hardware
 * blinking, ramps, the white/service output and vendor pattern interfaces are
 * intentionally omitted until their board policy is understood.
 */

#include <linux/leds.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77843-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define MAX77854_NUM_RGB_LEDS		3
#define MAX77854_FIRST_RGB_CHANNEL	1
#define MAX77854_MAX_BRIGHTNESS	2
#define MAX77854_CHANNEL_MODE_MASK	0x3
#define MAX77854_CHANNEL_MODE_CONSTANT	0x1
#define MAX77854_RGB_ENABLE_MASK	GENMASK(7, 2)

struct max77854_leds;

struct max77854_led {
	struct led_classdev cdev;
	struct max77854_leds *controller;
	unsigned int channel;
};

struct max77854_leds {
	struct regmap *regmap;
	struct mutex lock;
	struct max77854_led leds[MAX77854_NUM_RGB_LEDS];
};

static int max77854_led_set(struct led_classdev *cdev,
			    enum led_brightness brightness)
{
	struct max77854_led *led = container_of(cdev, struct max77854_led, cdev);
	struct max77854_leds *controller = led->controller;
	unsigned int shift = led->channel * 2;
	unsigned int mask = MAX77854_CHANNEL_MODE_MASK << shift;
	int ret;

	brightness = min_t(enum led_brightness, brightness,
			   MAX77854_MAX_BRIGHTNESS);

	mutex_lock(&controller->lock);
	if (brightness == LED_OFF) {
		ret = regmap_update_bits(controller->regmap,
					 MAX77843_LED_REG_LEDEN, mask, 0);
	} else {
		ret = regmap_write(controller->regmap,
				   MAX77843_LED_REG_LED0BRT + led->channel,
				   brightness);
		if (!ret)
			ret = regmap_update_bits(controller->regmap,
						 MAX77843_LED_REG_LEDEN, mask,
						 MAX77854_CHANNEL_MODE_CONSTANT << shift);
	}
	mutex_unlock(&controller->lock);

	return ret;
}

static void max77854_leds_off(void *data)
{
	struct max77854_leds *controller = data;

	mutex_lock(&controller->lock);
	regmap_update_bits(controller->regmap, MAX77843_LED_REG_LEDEN,
			   MAX77854_RGB_ENABLE_MASK, 0);
	mutex_unlock(&controller->lock);
}

static int max77854_led_probe(struct platform_device *pdev)
{
	struct max77693_dev *max77854 = dev_get_drvdata(pdev->dev.parent);
	struct max77854_leds *controller;
	struct device_node *child;
	unsigned long channels = 0;
	unsigned int count = 0;
	int ret;

	controller = devm_kzalloc(&pdev->dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;

	controller->regmap = max77854->regmap;
	mutex_init(&controller->lock);
	platform_set_drvdata(pdev, controller);

	ret = regmap_update_bits(controller->regmap, MAX77843_LED_REG_LEDEN,
				 MAX77854_RGB_ENABLE_MASK, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to disable RGB outputs\n");

	ret = devm_add_action_or_reset(&pdev->dev, max77854_leds_off,
				       controller);
	if (ret)
		return ret;

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		struct led_init_data init_data = { .fwnode = of_fwnode_handle(child) };
		struct max77854_led *led;
		u32 channel;

		ret = of_property_read_u32(child, "reg", &channel);
		if (ret)
			goto err_put_child;
		if (channel < MAX77854_FIRST_RGB_CHANNEL || channel > 3 ||
		    test_and_set_bit(channel, &channels)) {
			ret = -EINVAL;
			goto err_put_child;
		}
		if (count >= MAX77854_NUM_RGB_LEDS) {
			ret = -EINVAL;
			goto err_put_child;
		}

		led = &controller->leds[count++];
		led->controller = controller;
		led->channel = channel;
		led->cdev.max_brightness = MAX77854_MAX_BRIGHTNESS;
		led->cdev.brightness_set_blocking = max77854_led_set;

		ret = devm_led_classdev_register_ext(&pdev->dev, &led->cdev,
						     &init_data);
		if (ret)
			goto err_put_child;
	}

	if (count != MAX77854_NUM_RGB_LEDS)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "expected three RGB LED channels\n");

	return 0;

err_put_child:
	of_node_put(child);
	return dev_err_probe(&pdev->dev, ret, "invalid RGB LED child\n");
}

static void max77854_led_shutdown(struct platform_device *pdev)
{
	max77854_leds_off(platform_get_drvdata(pdev));
}

static const struct of_device_id max77854_led_of_match[] = {
	{ .compatible = "maxim,max77854-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77854_led_of_match);

static struct platform_driver max77854_led_driver = {
	.probe = max77854_led_probe,
	.shutdown = max77854_led_shutdown,
	.driver = {
		.name = "max77854-led",
		.of_match_table = max77854_led_of_match,
	},
};
module_platform_driver(max77854_led_driver);

MODULE_DESCRIPTION("Conservative MAX77854 notification LED driver");
MODULE_LICENSE("GPL");
