// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX77854 charger driver
 *
 * The register layout is closely related to MAX77705, but current and
 * constant-voltage encodings differ. Keep this driver intentionally small:
 * wired CHGIN charging, status reporting and AICL only. Wireless charging,
 * AFC/QC policy and fuel-gauge/thermal integration are follow-up work.
 */

#include <linux/devm-helpers.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77843-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define MAX77854_CHARGER_NAME			"max77854-charger"

#define MAX77854_CHG_WDTEN			BIT(4)
#define MAX77854_CHGPROT_MASK			GENMASK(3, 2)
#define MAX77854_CHGPROT_UNLOCKED		(0x3 << 2)

#define MAX77854_CHGIN_LIM_MASK			GENMASK(6, 0)
#define MAX77854_CHGIN_MIN_UA			100000
/* Conservative bring-up ceiling until thermal/cable policy is present. */
#define MAX77854_CHGIN_MAX_UA			450000
#define MAX77854_AICL_MIN_UA			300000
#define MAX77854_AICL_STEP_CODE			3
#define MAX77854_AICL_DELAY_MS			100

#define MAX77854_WCIN_LIM_MASK			GENMASK(5, 0)
#define MAX77854_WCIN_STEP_UA			30000
/* Matches vendor's own stock default WCIN limit - conservative until thermal policy exists. */
#define MAX77854_WCIN_MAX_UA			480000
#define MAX77854_WCIN_AICL_MIN_UA		120000

#define MAX77854_CHG_CC_MASK			GENMASK(5, 0)
#define MAX77854_CHG_CC_MIN_UA			100000
#define MAX77854_CHG_CC_HW_MAX_UA		3150000
#define MAX77854_CHG_CC_STEP_UA			50000

#define MAX77854_CHG_CV_MASK			GENMASK(5, 0)
#define MAX77854_CHG_CV_MIN_UV			4050000
#define MAX77854_CHG_CV_MAX_UV			4500000
#define MAX77854_CHG_CV_STEP_UV			12500

#define MAX77854_CHGINSEL			BIT(5)
#define MAX77854_VCHGIN_REG_MASK		GENMASK(4, 3)
#define MAX77854_WCIN_REG_MASK			GENMASK(2, 1)
#define MAX77854_DISSKIP			BIT(0)

#define MAX77854_DEFAULT_INPUT_UA		450000
#define MAX77854_DEFAULT_CHARGE_UA		450000

enum max77854_chg_irq {
	MAX77854_IRQ_BYP,
	MAX77854_IRQ_INP_LIMIT,
	MAX77854_IRQ_BATP,
	MAX77854_IRQ_BAT,
	MAX77854_IRQ_CHG,
	MAX77854_IRQ_WCIN,
	MAX77854_IRQ_CHGIN,
	MAX77854_IRQ_AICL,
};

struct max77854_charger {
	struct device *dev;
	struct max77693_dev *max77854;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply_battery_info *bat_info;
	struct work_struct changed_work;
	struct mutex lock;
	int max_charge_current_ua;
	int charge_voltage_uv;
	struct notifier_block wireless_nb;
	bool wireless_active;
};

static const enum power_supply_property max77854_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct regmap_irq max77854_charger_irqs[] = {
	[MAX77854_IRQ_BYP]		= { .mask = BIT(0) },
	[MAX77854_IRQ_INP_LIMIT]	= { .mask = BIT(1) },
	[MAX77854_IRQ_BATP]		= { .mask = BIT(2) },
	[MAX77854_IRQ_BAT]		= { .mask = BIT(3) },
	[MAX77854_IRQ_CHG]		= { .mask = BIT(4) },
	[MAX77854_IRQ_WCIN]		= { .mask = BIT(5) },
	[MAX77854_IRQ_CHGIN]		= { .mask = BIT(6) },
	[MAX77854_IRQ_AICL]		= { .mask = BIT(7) },
};

static const struct regmap_irq_chip max77854_charger_irq_chip = {
	.name		= MAX77854_CHARGER_NAME,
	.status_base	= MAX77843_CHG_REG_CHG_INT,
	.mask_base	= MAX77843_CHG_REG_CHG_INT_MASK,
	.num_regs	= 1,
	.irqs		= max77854_charger_irqs,
	.num_irqs	= ARRAY_SIZE(max77854_charger_irqs),
};

static int max77854_get_enabled(struct max77854_charger *chg, bool *enabled)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_00, &val);
	if (ret)
		return ret;

	*enabled = !!(val & MAX77843_CHG_MASK);
	return 0;
}

static int max77854_get_online(struct max77854_charger *chg, int *val)
{
	unsigned int data;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_INT_OK, &data);
	if (ret)
		return ret;

	*val = !!(data & (MAX77843_CHG_CHGIN_OK | MAX77843_CHG_WCIN_OK));
	return 0;
}

static int max77854_get_present(struct max77854_charger *chg, int *val)
{
	unsigned int int_ok, details;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_INT_OK, &int_ok);
	if (ret)
		return ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_DTLS_00, &details);
	if (ret)
		return ret;

	*val = !!((int_ok & MAX77843_CHG_BATP_OK) ||
		 !(details & MAX77843_CHG_BAT_DTLS));
	return 0;
}

static int max77854_get_status(struct max77854_charger *chg, int *val)
{
	unsigned int data;
	bool enabled;
	int ret;

	ret = max77854_get_enabled(chg, &enabled);
	if (ret)
		return ret;

	if (!enabled) {
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_DTLS_01, &data);
	if (ret)
		return ret;

	switch (data & MAX77843_CHG_DTLS_MASK) {
	case 0x00:
	case 0x01:
	case 0x02:
		*val = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x03:
	case 0x04:
		*val = POWER_SUPPLY_STATUS_FULL;
		break;
	case 0x05:
	case 0x06:
	case 0x07:
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case 0x08:
	case 0x0a:
	case 0x0b:
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		*val = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static int max77854_get_charge_type(struct max77854_charger *chg, int *val)
{
	unsigned int data;
	bool enabled;
	int ret;

	ret = max77854_get_enabled(chg, &enabled);
	if (ret)
		return ret;

	if (!enabled) {
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_DTLS_01, &data);
	if (ret)
		return ret;

	switch (data & MAX77843_CHG_DTLS_MASK) {
	case 0x00:
		*val = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case 0x01:
	case 0x02:
		*val = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	default:
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	}

	return 0;
}

static int max77854_get_vbus_health(struct max77854_charger *chg, int *val)
{
	unsigned int data;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_DTLS_00, &data);
	if (ret)
		return ret;

	switch ((data >> 5) & 0x3) {
	case 0x0:
	case 0x1:
		*val = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
		break;
	case 0x2:
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	case 0x3:
		*val = POWER_SUPPLY_HEALTH_GOOD;
		break;
	}

	return 0;
}

static int max77854_get_battery_health(struct max77854_charger *chg, int *val)
{
	unsigned int data;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_DTLS_01, &data);
	if (ret)
		return ret;

	switch ((data >> 4) & 0x7) {
	case 0x0:
		*val = POWER_SUPPLY_HEALTH_NO_BATTERY;
		break;
	case 0x1:
	case 0x3:
	case 0x4:
		*val = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x2:
		*val = POWER_SUPPLY_HEALTH_DEAD;
		break;
	case 0x5:
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	default:
		*val = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	}

	return 0;
}

static int max77854_get_health(struct max77854_charger *chg, int *val)
{
	int online;
	int ret;

	ret = max77854_get_online(chg, &online);
	if (ret)
		return ret;

	if (online) {
		ret = max77854_get_vbus_health(chg, val);
		if (ret || *val != POWER_SUPPLY_HEALTH_GOOD)
			return ret;
	}

	return max77854_get_battery_health(chg, val);
}

static unsigned int max77854_input_current_to_code(int ua)
{
	int ma, quotient, remainder;
	unsigned int code;

	ua = clamp_val(ua, MAX77854_CHGIN_MIN_UA, MAX77854_CHGIN_MAX_UA);
	ma = ua / 1000;
	quotient = ma / 100;
	remainder = ma % 100;
	code = quotient * 3;

	if (remainder >= 67)
		code += 2;
	else if (remainder >= 33)
		code += 1;

	return min_t(unsigned int, code, 0x78);
}

static int max77854_code_to_input_current(unsigned int code)
{
	unsigned int quotient, remainder;

	code &= MAX77854_CHGIN_LIM_MASK;
	if (code <= 0x3)
		return MAX77854_CHGIN_MIN_UA;
	if (code >= 0x78)
		return 4000000;

	quotient = code / 3;
	remainder = code % 3;

	return quotient * 100000 +
	       (remainder == 1 ? 33000 : remainder == 2 ? 67000 : 0);
}

static unsigned int max77854_wcin_current_to_code(int ua)
{
	ua = clamp_val(ua, MAX77854_WCIN_STEP_UA, MAX77854_WCIN_MAX_UA);
	return min_t(unsigned int, ua / MAX77854_WCIN_STEP_UA, MAX77854_WCIN_LIM_MASK);
}

static int max77854_set_input_current(struct max77854_charger *chg, int ua)
{
	unsigned int reg, mask, code;
	int ret;

	if (chg->wireless_active) {
		reg = MAX77843_CHG_REG_CHG_CNFG_10;
		mask = MAX77854_WCIN_LIM_MASK;
		code = max77854_wcin_current_to_code(ua);
	} else {
		reg = MAX77843_CHG_REG_CHG_CNFG_09;
		mask = MAX77854_CHGIN_LIM_MASK;
		code = max77854_input_current_to_code(ua);
	}

	mutex_lock(&chg->lock);
	ret = regmap_update_bits(chg->regmap, reg, mask, code);
	mutex_unlock(&chg->lock);

	return ret;
}

static int max77854_get_input_current(struct max77854_charger *chg, int *ua)
{
	unsigned int reg, code;
	int ret;

	reg = chg->wireless_active ? MAX77843_CHG_REG_CHG_CNFG_10 :
				      MAX77843_CHG_REG_CHG_CNFG_09;

	ret = regmap_read(chg->regmap, reg, &code);
	if (ret)
		return ret;

	*ua = chg->wireless_active ? (code & MAX77854_WCIN_LIM_MASK) * MAX77854_WCIN_STEP_UA :
				      max77854_code_to_input_current(code);
	return 0;
}

static int max77854_select_input(struct max77854_charger *chg, bool wireless)
{
	int ret;

	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_12,
				 MAX77854_CHGINSEL, wireless ? 0 : MAX77854_CHGINSEL);
	if (ret)
		return ret;

	return max77854_set_input_current(chg, wireless ? MAX77854_WCIN_MAX_UA :
							    MAX77854_DEFAULT_INPUT_UA);
}

static int max77854_set_charge_current(struct max77854_charger *chg, int ua)
{
	unsigned int code;

	ua = clamp_val(ua, MAX77854_CHG_CC_MIN_UA, chg->max_charge_current_ua);
	code = ua / MAX77854_CHG_CC_STEP_UA;
	code = min_t(unsigned int, code, MAX77854_CHG_CC_MASK);

	return regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_02,
				  MAX77854_CHG_CC_MASK, code);
}

static int max77854_get_charge_current(struct max77854_charger *chg, int *ua)
{
	unsigned int code;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_02, &code);
	if (ret)
		return ret;

	*ua = (code & MAX77854_CHG_CC_MASK) * MAX77854_CHG_CC_STEP_UA;
	return 0;
}

static int max77854_set_float_voltage(struct max77854_charger *chg, int uv)
{
	unsigned int code;

	uv = clamp_val(uv, MAX77854_CHG_CV_MIN_UV, MAX77854_CHG_CV_MAX_UV);
	code = (uv - MAX77854_CHG_CV_MIN_UV) / MAX77854_CHG_CV_STEP_UV;

	return regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_04,
				  MAX77854_CHG_CV_MASK, code);
}

static int max77854_get_float_voltage(struct max77854_charger *chg, int *uv)
{
	unsigned int code;
	int ret;

	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_04, &code);
	if (ret)
		return ret;

	code &= MAX77854_CHG_CV_MASK;
	*uv = MAX77854_CHG_CV_MIN_UV + code * MAX77854_CHG_CV_STEP_UV;
	return 0;
}

static int max77854_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct max77854_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		return max77854_get_online(chg, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		return max77854_get_present(chg, &val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		return max77854_get_status(chg, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return max77854_get_charge_type(chg, &val->intval);
	case POWER_SUPPLY_PROP_HEALTH:
		return max77854_get_health(chg, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chg->bat_info->voltage_max_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return max77854_get_float_voltage(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return max77854_get_charge_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return max77854_get_input_current(chg, &val->intval);
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

static int max77854_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct max77854_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return max77854_set_charge_current(chg, val->intval);
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return max77854_set_input_current(chg, val->intval);
	default:
		return -EINVAL;
	}
}

static int max77854_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc max77854_charger_desc = {
	.name			= MAX77854_CHARGER_NAME,
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= max77854_charger_props,
	.num_properties		= ARRAY_SIZE(max77854_charger_props),
	.get_property		= max77854_get_property,
	.set_property		= max77854_set_property,
	.property_is_writeable	= max77854_property_is_writeable,
};

static void max77854_changed_work(struct work_struct *work)
{
	struct max77854_charger *chg =
		container_of(work, struct max77854_charger, changed_work);

	power_supply_changed(chg->psy);
}

static irqreturn_t max77854_changed_irq(int irq, void *data)
{
	struct max77854_charger *chg = data;

	schedule_work(&chg->changed_work);
	return IRQ_HANDLED;
}

static irqreturn_t max77854_aicl_irq(int irq, void *data)
{
	struct max77854_charger *chg = data;
	unsigned int int_ok, code;
	unsigned int reg = chg->wireless_active ? MAX77843_CHG_REG_CHG_CNFG_10 :
						   MAX77843_CHG_REG_CHG_CNFG_09;
	unsigned int mask = chg->wireless_active ? MAX77854_WCIN_LIM_MASK :
						    MAX77854_CHGIN_LIM_MASK;
	unsigned int min_code = chg->wireless_active ?
		max77854_wcin_current_to_code(MAX77854_WCIN_AICL_MIN_UA) :
		max77854_input_current_to_code(MAX77854_AICL_MIN_UA);
	int ret, loops = 0;

	mutex_lock(&chg->lock);

	for (;;) {
		ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_INT_OK,
				  &int_ok);
		if (ret || (int_ok & MAX77843_CHG_AICL_OK))
			break;

		ret = regmap_read(chg->regmap, reg, &code);
		if (ret)
			break;

		code &= mask;
		if (code <= min_code)
			break;

		code = max_t(unsigned int, code - MAX77854_AICL_STEP_CODE,
			     min_code);

		ret = regmap_update_bits(chg->regmap, reg, mask, code);
		if (ret)
			break;

		if (++loops >= 40)
			break;

		msleep(MAX77854_AICL_DELAY_MS);
	}

	mutex_unlock(&chg->lock);
	schedule_work(&chg->changed_work);

	return IRQ_HANDLED;
}

static int max77854_wireless_notifier_call(struct notifier_block *nb,
					    unsigned long event, void *data)
{
	struct max77854_charger *chg =
		container_of(nb, struct max77854_charger, wireless_nb);
	struct power_supply *wireless_psy = data;
	union power_supply_propval online = { 0 };
	unsigned int chgin_ok = 0;
	bool wireless;

	if (event != PSY_EVENT_PROP_CHANGED ||
	    strcmp(wireless_psy->desc->name, "p9220-wireless"))
		return NOTIFY_OK;

	if (power_supply_get_property(wireless_psy, POWER_SUPPLY_PROP_ONLINE, &online))
		return NOTIFY_OK;

	/* Wired CHGIN always wins over wireless when both are present. */
	regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_INT_OK, &chgin_ok);
	wireless = !(chgin_ok & MAX77843_CHG_CHGIN_OK) && !!online.intval;

	if (wireless == chg->wireless_active)
		return NOTIFY_OK;

	chg->wireless_active = wireless;
	max77854_select_input(chg, wireless);
	schedule_work(&chg->changed_work);

	return NOTIFY_OK;
}

static void max77854_unreg_wireless_notifier(void *data)
{
	struct max77854_charger *chg = data;

	power_supply_unreg_notifier(&chg->wireless_nb);
}

static int max77854_charger_enable(struct max77854_charger *chg)
{
	int ret;

	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_00,
				 MAX77843_CHG_MODE_MASK,
				 MAX77843_CHG_ENABLE);
	if (ret)
		return ret;

	return regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_12,
				  MAX77854_DISSKIP, MAX77854_DISSKIP);
}

static void max77854_charger_disable(void *data)
{
	struct max77854_charger *chg = data;

	regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_00,
			   MAX77843_CHG_MASK, 0);
	regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_12,
			   MAX77854_DISSKIP, 0);
}

static int max77854_charger_source_enable(struct max77854_charger *chg)
{
	return regmap_update_bits(chg->max77854->regmap,
				  MAX77843_SYS_REG_INTSRCMASK,
				  MAX77843_INTSRCMASK_CHGR_MASK, 0);
}

static void max77854_charger_source_disable(void *data)
{
	struct max77854_charger *chg = data;

	regmap_update_bits(chg->max77854->regmap,
			   MAX77843_SYS_REG_INTSRCMASK,
			   MAX77843_INTSRCMASK_CHGR_MASK,
			   MAX77843_INTSRCMASK_CHGR_MASK);
}

static void max77854_put_battery_info(void *data)
{
	struct max77854_charger *chg = data;

	power_supply_put_battery_info(chg->psy, chg->bat_info);
}

static int max77854_charger_initialize(struct max77854_charger *chg)
{
	int ret;

	/* Unlock charger configuration writes. */
	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_06,
				 MAX77854_CHGPROT_MASK,
				 MAX77854_CHGPROT_UNLOCKED);
	if (ret)
		return ret;

	/*
	 * Match Samsung's herolte defaults: fast-charge timer disabled,
	 * restart threshold disabled and 2 MHz switching.
	 */
	ret = regmap_write(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_01, 0x38);
	if (ret)
		return ret;

	/* Preserve CC bits while selecting the stock 900 mA OTG limit. */
	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_02,
				 GENMASK(7, 6), BIT(6));
	if (ret)
		return ret;

	/* 150 mA top-off current, 70 minute top-off timer. */
	ret = regmap_write(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_03, 0x38);
	if (ret)
		return ret;

	ret = max77854_set_float_voltage(chg, chg->charge_voltage_uv);
	if (ret)
		return ret;

	/*
	 * Select wired CHGIN by default and Samsung's 4.3 V regulation / 4.5 V
	 * UVLO encoding for both inputs. Switched to WCIN at runtime by the
	 * wireless-supply notifier below when a pad is present and no wired
	 * cable is plugged in.
	 */
	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_12,
				 MAX77854_CHGINSEL |
				 MAX77854_VCHGIN_REG_MASK |
				 MAX77854_WCIN_REG_MASK |
				 MAX77854_DISSKIP,
				 MAX77854_CHGINSEL);
	if (ret)
		return ret;

	/*
	 * Start from a deterministic safe mode: buck on, charging/OTG/boost
	 * off and watchdog disabled. Charging is enabled after IRQ setup.
	 */
	ret = regmap_update_bits(chg->regmap, MAX77843_CHG_REG_CHG_CNFG_00,
				 MAX77843_CHG_MODE_MASK | MAX77854_CHG_WDTEN,
				 MAX77843_CHG_BUCK_MASK);
	if (ret)
		return ret;

	ret = max77854_set_input_current(chg, MAX77854_DEFAULT_INPUT_UA);
	if (ret)
		return ret;

	return max77854_set_charge_current(chg, MAX77854_DEFAULT_CHARGE_UA);
}

static int max77854_request_changed_irq(struct max77854_charger *chg,
					struct regmap_irq_chip_data *irq_data,
					unsigned int hwirq,
					const char *name)
{
	int virq;

	virq = regmap_irq_get_virq(irq_data, hwirq);
	if (virq <= 0)
		return virq ?: -EINVAL;

	return devm_request_threaded_irq(chg->dev, virq, NULL,
					 max77854_changed_irq,
					 IRQF_TRIGGER_NONE, name, chg);
}

static int max77854_charger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct max77693_dev *max77854 = dev_get_drvdata(dev->parent);
	struct regmap_irq_chip_data *irq_data;
	struct power_supply_config psy_cfg = {};
	struct max77854_charger *chg;
	unsigned int pending;
	int ret;

	if (!max77854 || max77854->type != TYPE_MAX77854 ||
	    !max77854->regmap_chg)
		return dev_err_probe(dev, -ENODEV,
				     "MAX77854 charger regmap is unavailable\n");

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = dev;
	chg->max77854 = max77854;
	chg->regmap = max77854->regmap_chg;
	mutex_init(&chg->lock);
	platform_set_drvdata(pdev, chg);

	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.drv_data = chg;

	chg->psy = devm_power_supply_register(dev, &max77854_charger_desc,
					      &psy_cfg);
	if (IS_ERR(chg->psy))
		return PTR_ERR(chg->psy);

	ret = power_supply_get_battery_info(chg->psy, &chg->bat_info);
	if (ret)
		return dev_err_probe(dev, ret,
				     "monitored-battery data is required\n");

	ret = devm_add_action_or_reset(dev, max77854_put_battery_info, chg);
	if (ret)
		return ret;

	chg->charge_voltage_uv = chg->bat_info->constant_charge_voltage_max_uv;
	if (chg->charge_voltage_uv <= 0)
		chg->charge_voltage_uv = chg->bat_info->voltage_max_design_uv;

	if (chg->charge_voltage_uv < MAX77854_CHG_CV_MIN_UV ||
	    chg->charge_voltage_uv > MAX77854_CHG_CV_MAX_UV)
		return dev_err_probe(dev, -EINVAL,
				     "unsafe battery CV target: %d uV\n",
				     chg->charge_voltage_uv);

	chg->max_charge_current_ua =
		chg->bat_info->constant_charge_current_max_ua;
	if (chg->max_charge_current_ua <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "battery charge-current limit is required\n");

	chg->max_charge_current_ua = min(chg->max_charge_current_ua,
					 MAX77854_CHG_CC_HW_MAX_UA);

	ret = max77854_charger_initialize(chg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize charger\n");

	ret = devm_regmap_add_irq_chip(dev, chg->regmap, max77854->irq,
				       IRQF_TRIGGER_LOW | IRQF_ONESHOT |
				       IRQF_SHARED,
				       0, &max77854_charger_irq_chip,
				       &irq_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register charger IRQ chip\n");

	ret = devm_work_autocancel(dev, &chg->changed_work,
				   max77854_changed_work);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize charger work\n");

	ret = max77854_request_changed_irq(chg, irq_data, MAX77854_IRQ_BATP,
					   "max77854-batp");
	if (ret)
		return ret;

	ret = max77854_request_changed_irq(chg, irq_data, MAX77854_IRQ_BAT,
					   "max77854-bat");
	if (ret)
		return ret;

	ret = max77854_request_changed_irq(chg, irq_data, MAX77854_IRQ_CHG,
					   "max77854-chg");
	if (ret)
		return ret;

	ret = max77854_request_changed_irq(chg, irq_data, MAX77854_IRQ_CHGIN,
					   "max77854-chgin");
	if (ret)
		return ret;

	ret = max77854_request_changed_irq(chg, irq_data, MAX77854_IRQ_WCIN,
					   "max77854-wcin");
	if (ret)
		return ret;

	{
		int virq = regmap_irq_get_virq(irq_data, MAX77854_IRQ_AICL);

		if (virq <= 0)
			return virq ?: -EINVAL;

		ret = devm_request_threaded_irq(dev, virq, NULL,
					       max77854_aicl_irq,
					       IRQF_TRIGGER_NONE,
					       "max77854-aicl", chg);
		if (ret)
			return ret;
	}

	/* Clear any charger status latched while the parent source was masked. */
	ret = regmap_read(chg->regmap, MAX77843_CHG_REG_CHG_INT, &pending);
	if (ret)
		return ret;

	ret = max77854_charger_source_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to unmask charger interrupt source\n");

	ret = devm_add_action_or_reset(dev, max77854_charger_source_disable,
				       chg);
	if (ret)
		return ret;

	ret = max77854_charger_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable charging\n");

	ret = devm_add_action_or_reset(dev, max77854_charger_disable, chg);
	if (ret)
		return ret;

	chg->wireless_nb.notifier_call = max77854_wireless_notifier_call;
	ret = power_supply_reg_notifier(&chg->wireless_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register wireless-supply notifier\n");

	ret = devm_add_action_or_reset(dev, max77854_unreg_wireless_notifier, chg);
	if (ret)
		return ret;

	/* Pick up a pad that's already reporting present at probe time. */
	{
		struct power_supply *wireless_psy = power_supply_get_by_name("p9220-wireless");

		if (wireless_psy) {
			union power_supply_propval online = { 0 };

			if (!power_supply_get_property(wireless_psy, POWER_SUPPLY_PROP_ONLINE,
							&online) && online.intval) {
				chg->wireless_active = true;
				max77854_select_input(chg, true);
			}
			power_supply_put(wireless_psy);
		}
	}

	dev_info(dev,
		 "wired charger ready: CV=%duV CC<=%duA input<=%duA\n",
		 chg->charge_voltage_uv, chg->max_charge_current_ua,
		 MAX77854_CHGIN_MAX_UA);

	return 0;
}

static const struct of_device_id max77854_charger_of_match[] = {
	{ .compatible = "maxim,max77854-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77854_charger_of_match);

static const struct platform_device_id max77854_charger_id[] = {
	{ MAX77854_CHARGER_NAME },
	{ }
};
MODULE_DEVICE_TABLE(platform, max77854_charger_id);

static struct platform_driver max77854_charger_driver = {
	.driver = {
		.name = MAX77854_CHARGER_NAME,
		.of_match_table = max77854_charger_of_match,
	},
	.probe = max77854_charger_probe,
	.id_table = max77854_charger_id,
};
module_platform_driver(max77854_charger_driver);

MODULE_DESCRIPTION("Maxim MAX77854 wired battery charger driver");
MODULE_LICENSE("GPL");
