/* SPDX-License-Identifier: GPL-2.0 */
/*
 * of_get_named_gpio() was dropped from mainline (<linux/of_gpio.h> is gone)
 * in favour of the gpiod consumer API. Vendor's decon_8890/dsim code still
 * calls it by the DT property's exact name (no "-gpios"/"-gpio" suffix
 * convention), so a plain fwnode_gpiod_get_index() with @propname as con_id
 * would look for the wrong property. This keeps the exact-name lookup vendor
 * code expects, translated to a legacy int GPIO number the way the real
 * of_get_named_gpio() used to - callers still use gpio_is_valid()/
 * gpio_to_irq() on the result unchanged. DT nodes for this driver must name
 * these properties "<propname>-gpios" to match.
 */
#ifndef __DECON8890_OF_GPIO_COMPAT_H__
#define __DECON8890_OF_GPIO_COMPAT_H__

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>

static inline int of_get_named_gpio(struct device_node *np,
				    const char *propname, int index)
{
	struct gpio_desc *desc;

	desc = fwnode_gpiod_get_index(of_fwnode_handle(np), propname, index,
				      GPIOD_ASIS, propname);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	return desc_to_gpio(desc);
}

/*
 * of_get_gpio() looked up the node's plain, unnamed "gpios" property (no
 * prefix) by index - matched here by passing con_id=NULL, which makes
 * fwnode_gpiod_get_index() build the property name as just "gpios"
 * (gpiolib.h's for_each_gpio_property_name() skips the "%s-" prefix when
 * con_id is NULL, same rule as of_get_named_gpio() above relies on).
 */
static inline int of_get_gpio(struct device_node *np, int index)
{
	struct gpio_desc *desc;

	desc = fwnode_gpiod_get_index(of_fwnode_handle(np), NULL, index,
				      GPIOD_ASIS, NULL);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	return desc_to_gpio(desc);
}

/*
 * Legacy raw-number gpio_get_value() was dropped from mainline along with
 * <linux/gpio.h>. gpio_to_desc() is still exported for exactly this kind
 * of int-gpio-number compat. Raw (not gpiod_get_value()'s polarity-aware)
 * to match what the old gpio_get_value() actually did.
 */
static inline int gpio_get_value(unsigned int gpio)
{
	return gpiod_get_raw_value(gpio_to_desc(gpio));
}

static inline void gpio_set_value(unsigned int gpio, int value)
{
	gpiod_set_raw_value(gpio_to_desc(gpio), value);
}

/*
 * gpio_is_valid()/gpio_to_irq() likewise dropped with <linux/gpio.h>.
 * gpio_is_valid() used to also bound-check against ARCH_NR_GPIOS, which
 * doesn't exist in a descriptor-based gpiolib build - a negative number
 * (our of_get_named_gpio() compat above returns -ERRNO on failure, same
 * as the real one used to) is the only failure case left to check for.
 */
static inline bool gpio_is_valid(int gpio)
{
	return gpio >= 0;
}

static inline int gpio_to_irq(unsigned int gpio)
{
	return gpiod_to_irq(gpio_to_desc(gpio));
}

/*
 * gpio_request_one()/gpio_free() also dropped with <linux/gpio.h>. Unlike
 * the real legacy ones, our of_get_named_gpio() above already claims the
 * descriptor (via fwnode_gpiod_get_index()) at DT-lookup time, so there's
 * no second claim to take here - gpio_request_one() just applies the
 * requested initial direction/level to the already-owned descriptor via
 * gpio_to_desc(), and gpio_free() is a no-op (the descriptor's lifetime
 * belongs to whoever first looked it up via of_get_named_gpio(), which is
 * usually still using it elsewhere - e.g. gpio_get_value()/gpio_to_irq()
 * on the same pin after this request/free pair).
 */
#define GPIOF_DIR_OUT		(0 << 0)
#define GPIOF_DIR_IN		(1 << 0)
#define GPIOF_INIT_LOW		(0 << 1)
#define GPIOF_INIT_HIGH		(1 << 1)
#define GPIOF_IN		GPIOF_DIR_IN
#define GPIOF_OUT_INIT_LOW	(GPIOF_DIR_OUT | GPIOF_INIT_LOW)
#define GPIOF_OUT_INIT_HIGH	(GPIOF_DIR_OUT | GPIOF_INIT_HIGH)

static inline int gpio_request_one(unsigned int gpio, unsigned long flags,
				   const char *label)
{
	struct gpio_desc *desc = gpio_to_desc(gpio);

	if (!desc)
		return -EINVAL;

	if (flags & GPIOF_DIR_IN)
		return gpiod_direction_input(desc);

	return gpiod_direction_output_raw(desc,
			(flags & GPIOF_INIT_HIGH) ? 1 : 0);
}

static inline void gpio_free(unsigned int gpio)
{
}

#endif /* __DECON8890_OF_GPIO_COMPAT_H__ */
