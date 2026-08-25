// SPDX-License-Identifier: GPL-2.0-only
/*
 * IDT P9220 wireless power receiver (RX-only)
 *
 * Register map and the OTP bootloader/programming sequence are ported from
 * Samsung's GPL vendor driver (drivers/battery/p9220_charger.c). TX mode,
 * FOD active-tuning and HV/9V-10V negotiation are intentionally not
 * implemented in this pass - 5V RX only.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/* 16-bit register address, 8-bit value - verified against vendor's p9220_charger.h */
#define P9220_CHIP_ID_L_REG		0x00
#define P9220_CHIP_ID_H_REG		0x01
#define P9220_CHIP_REVISION_REG	0x02
#define P9220_OTP_FW_MAJOR_L_REG	0x04
#define P9220_OTP_FW_MAJOR_H_REG	0x05
#define P9220_OTP_FW_MINOR_L_REG	0x06
#define P9220_OTP_FW_MINOR_H_REG	0x07
#define P9220_INT_STATUS_L_REG		0x34
#define P9220_INT_L_REG		0x36
#define P9220_INT_H_REG		0x37
#define P9220_INT_ENABLE_L_REG		0x38
#define P9220_ADC_VOUT_L_REG		0x3C
#define P9220_ADC_VOUT_H_REG		0x3D
#define P9220_VOUT_SET_REG		0x3E
#define P9220_ADC_RX_IOUT_L_REG	0x44
#define P9220_ADC_DIE_TEMP_L_REG	0x46
#define P9220_COMMAND_REG		0x4E
#define P9220_INT_CLEAR_L_REG		0x56
#define P9220_INT_CLEAR_H_REG		0x57
#define P9220_WPC_FOD_0A_REG		0x68

#define P9220_NUM_FOD_REG		12

#define P9220_STAT_MODE_CHANGE_MASK	BIT(5)
#define P9220_CMD_CLEAR_INT_MASK	BIT(5)

#define P9220_VOUT_5V_VAL		0x0f
#define P9220_VOUT_FULL_SCALE_MV	12600

/*
 * Vendor's version check actually only compares the OTP minor-rev word
 * (the major-rev read is discarded before the comparison) - replicated
 * as-is since this constant was calibrated against that behaviour.
 */
#define P9220_OTP_FW_VERSION_EXPECTED	0x4012

/* Bootloader-mode registers used only while programming OTP */
#define P9220_KEY_REG			0x3000
#define P9220_M0_CTRL_REG		0x3040
#define P9220_CODE_REMAP_REG		0x3048
#define P9220_OTP_BUF_REG		0x0400
#define P9220_OTP_LOADER_ADDR		0x1c00

#define P9220_KEY_UNLOCK		0x5a
#define P9220_M0_HALT			0x10
#define P9220_M0_RUN			0x80
#define P9220_REMAP_RAM_TO_OTP		0x80
#define P9220_REMAP_NONE		0x00
#define P9220_OTP_PAGE_LEN		128

/* Small RAM-resident bootloader the chip needs to program its own OTP - vendor's OTPBootloader[] */
static const u8 p9220_otp_loader[] = {
0x00, 0x04, 0x00, 0x20, 0x57, 0x01, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0xFE, 0xE7, 0x00, 0x00, 0x80, 0x00, 0x00, 0xE0, 0x00, 0xBF, 0x40, 0x1E, 0xFC, 0xD2, 0x70, 0x47,
0x00, 0xB5, 0x6F, 0x4A, 0x6F, 0x4B, 0x01, 0x70, 0x01, 0x20, 0xFF, 0xF7, 0xF3, 0xFF, 0x52, 0x1E,
0x02, 0xD0, 0x18, 0x8B, 0x00, 0x06, 0xF7, 0xD4, 0x00, 0xBD, 0xF7, 0xB5, 0x05, 0x46, 0x6A, 0x48,
0x81, 0xB0, 0x00, 0x21, 0x94, 0x46, 0x81, 0x81, 0x66, 0x48, 0x31, 0x21, 0x01, 0x80, 0x04, 0x21,
0x81, 0x80, 0x06, 0x21, 0x01, 0x82, 0x28, 0x20, 0xFF, 0xF7, 0xDC, 0xFF, 0x00, 0x24, 0x15, 0xE0,
0x02, 0x99, 0x28, 0x5D, 0x09, 0x5D, 0x02, 0x46, 0x8A, 0x43, 0x01, 0xD0, 0x10, 0x20, 0x50, 0xE0,
0x81, 0x43, 0x0A, 0xD0, 0x5D, 0x4E, 0xB0, 0x89, 0x08, 0x27, 0x38, 0x43, 0xB0, 0x81, 0x28, 0x19,
0xFF, 0xF7, 0xCE, 0xFF, 0xB0, 0x89, 0xB8, 0x43, 0xB0, 0x81, 0x64, 0x1C, 0x64, 0x45, 0xE7, 0xD3,
0x54, 0x48, 0x36, 0x21, 0x01, 0x82, 0x00, 0x24, 0x38, 0xE0, 0x02, 0x98, 0x00, 0x27, 0x06, 0x5D,
0x52, 0x48, 0x82, 0x89, 0x08, 0x21, 0x0A, 0x43, 0x82, 0x81, 0x28, 0x19, 0x00, 0x90, 0x4D, 0x4A,
0x08, 0x20, 0x90, 0x80, 0x02, 0x20, 0xFF, 0xF7, 0xAD, 0xFF, 0x28, 0x5D, 0x33, 0x46, 0x83, 0x43,
0x15, 0xD0, 0x48, 0x49, 0x04, 0x20, 0x88, 0x80, 0x02, 0x20, 0xFF, 0xF7, 0xA3, 0xFF, 0x19, 0x46,
0x00, 0x98, 0xFF, 0xF7, 0xA5, 0xFF, 0x43, 0x49, 0x0F, 0x20, 0x88, 0x80, 0x02, 0x20, 0xFF, 0xF7,
0x99, 0xFF, 0x28, 0x5D, 0xB0, 0x42, 0x02, 0xD0, 0x7F, 0x1C, 0x0A, 0x2F, 0xDF, 0xD3, 0x3F, 0x48,
0x82, 0x89, 0x08, 0x21, 0x8A, 0x43, 0x82, 0x81, 0x0A, 0x2F, 0x06, 0xD3, 0x3C, 0x48, 0x29, 0x19,
0x41, 0x80, 0x29, 0x5D, 0xC1, 0x80, 0x04, 0x20, 0x03, 0xE0, 0x64, 0x1C, 0x64, 0x45, 0xC4, 0xD3,
0x02, 0x20, 0x34, 0x49, 0x11, 0x22, 0x0A, 0x80, 0x04, 0x22, 0x8A, 0x80, 0x32, 0x49, 0xFF, 0x22,
0x8A, 0x81, 0x04, 0xB0, 0xF0, 0xBD, 0x34, 0x49, 0x32, 0x48, 0x08, 0x60, 0x2F, 0x4D, 0x00, 0x22,
0xAA, 0x81, 0x2E, 0x4E, 0x20, 0x3E, 0xB2, 0x83, 0x2A, 0x80, 0x2B, 0x48, 0x5A, 0x21, 0x40, 0x38,
0x01, 0x80, 0x81, 0x15, 0x81, 0x80, 0x0B, 0x21, 0x01, 0x81, 0x2C, 0x49, 0x81, 0x81, 0x14, 0x20,
0xFF, 0xF7, 0x60, 0xFF, 0x2A, 0x4B, 0x01, 0x20, 0x18, 0x80, 0x02, 0x20, 0xFF, 0xF7, 0x5A, 0xFF,
0x8D, 0x20, 0x18, 0x80, 0x9A, 0x80, 0xFF, 0x20, 0x98, 0x82, 0x03, 0x20, 0x00, 0x02, 0x18, 0x82,
0xFC, 0x20, 0x98, 0x83, 0x22, 0x49, 0x95, 0x20, 0x20, 0x31, 0x08, 0x80, 0x1C, 0x4C, 0x0C, 0x20,
0x22, 0x80, 0xA8, 0x81, 0x20, 0x20, 0xB0, 0x83, 0x28, 0x80, 0xAA, 0x81, 0x04, 0x26, 0xA8, 0x89,
0x30, 0x43, 0xA8, 0x81, 0x20, 0x88, 0x01, 0x28, 0x1B, 0xD1, 0x61, 0x88, 0x80, 0x03, 0xA2, 0x88,
0x08, 0x18, 0x51, 0x18, 0x8B, 0xB2, 0x00, 0x21, 0x04, 0xE0, 0x0F, 0x19, 0x3F, 0x7A, 0xFB, 0x18,
0x9B, 0xB2, 0x49, 0x1C, 0x8A, 0x42, 0xF8, 0xD8, 0xE1, 0x88, 0x27, 0x46, 0x99, 0x42, 0x01, 0xD0,
0x08, 0x20, 0x0B, 0xE0, 0x00, 0x2A, 0x08, 0xD0, 0x09, 0x49, 0x08, 0x31, 0xFF, 0xF7, 0x35, 0xFF,
0x38, 0x80, 0xA8, 0x89, 0xB0, 0x43, 0xA8, 0x81, 0xD9, 0xE7, 0x02, 0x20, 0x20, 0x80, 0xD6, 0xE7,
0x10, 0x27, 0x00, 0x00, 0x00, 0x5C, 0x00, 0x40, 0x40, 0x30, 0x00, 0x40, 0x20, 0x6C, 0x00, 0x40,
0x00, 0x04, 0x00, 0x20, 0xFF, 0x0F, 0x00, 0x00, 0x80, 0xE1, 0x00, 0xE0, 0x04, 0x1D, 0x00, 0x00,
0x00, 0x64, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct p9220_data {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *en;
	struct gpio_desc *detect;
	struct power_supply *psy;

	struct delayed_work det_work;
	struct work_struct otp_work;

	int irq_det;
	int irq_int;

	bool present;
	bool fw_flash_attempted;
	bool have_fod;
	u8 fod_data[P9220_NUM_FOD_REG];
};

static int p9220_get_adc_raw(struct p9220_data *p9220, u16 reg_l)
{
	u8 buf[2];
	int ret;

	ret = regmap_raw_read(p9220->regmap, reg_l, buf, sizeof(buf));
	if (ret)
		return ret;

	return buf[0] | ((buf[1] & 0x0f) << 8);
}

static int p9220_get_adc_mv(struct p9220_data *p9220, u16 reg_l, unsigned int full_scale_mv)
{
	int raw = p9220_get_adc_raw(p9220, reg_l);

	if (raw < 0)
		return raw;

	return raw * full_scale_mv / 4095;
}

static int p9220_read_otp_version(struct p9220_data *p9220, u16 *version)
{
	u8 buf[2];
	int ret;

	ret = regmap_raw_read(p9220->regmap, P9220_OTP_FW_MINOR_L_REG, buf, sizeof(buf));
	if (ret)
		return ret;

	*version = buf[0] | (buf[1] << 8);
	return 0;
}

static void p9220_apply_fod(struct p9220_data *p9220)
{
	if (!p9220->have_fod)
		return;

	regmap_raw_write(p9220->regmap, P9220_WPC_FOD_0A_REG, p9220->fod_data,
			  P9220_NUM_FOD_REG);
}

static int p9220_load_otp_loader(struct p9220_data *p9220)
{
	u8 rd[sizeof(p9220_otp_loader)];
	int ret;

	ret = regmap_raw_write(p9220->regmap, P9220_OTP_LOADER_ADDR,
				p9220_otp_loader, sizeof(p9220_otp_loader));
	if (ret)
		return ret;

	ret = regmap_raw_read(p9220->regmap, P9220_OTP_LOADER_ADDR, rd, sizeof(rd));
	if (ret)
		return ret;

	if (memcmp(rd, p9220_otp_loader, sizeof(rd))) {
		dev_err(p9220->dev, "OTP bootloader readback mismatch\n");
		return -EIO;
	}

	return 0;
}

static int p9220_program_otp_page(struct p9220_data *p9220, unsigned int offset,
				   const u8 *data, size_t page_len)
{
	u8 buf[8 + P9220_OTP_PAGE_LEN] = { 0 };
	u16 start_addr = offset;
	u16 code_len = P9220_OTP_PAGE_LEN;
	u16 checksum;
	unsigned int status = 0;
	int ret, i, timeout;

	memcpy(buf + 8, data, page_len);

	for (i = P9220_OTP_PAGE_LEN - 1; i >= 0; i--) {
		if (buf[8 + i])
			break;
		code_len--;
	}
	if (code_len == 0)
		return 0;

	checksum = start_addr;
	for (i = 0; i < code_len; i++)
		checksum += buf[8 + i];
	checksum += code_len;

	buf[2] = start_addr & 0xff;
	buf[3] = start_addr >> 8;
	buf[4] = code_len & 0xff;
	buf[5] = code_len >> 8;
	buf[6] = checksum & 0xff;
	buf[7] = checksum >> 8;

	ret = regmap_raw_write(p9220->regmap, P9220_OTP_BUF_REG, buf, code_len + 8);
	if (ret)
		return ret;

	ret = regmap_write(p9220->regmap, P9220_OTP_BUF_REG, 1);
	if (ret)
		return ret;

	for (timeout = 0; timeout < 1000; timeout++) {
		msleep(20);
		ret = regmap_read(p9220->regmap, P9220_OTP_BUF_REG, &status);
		if (ret)
			return ret;
		if (status != 1)
			break;
	}

	if (status != 2) {
		dev_err(p9220->dev, "OTP page write at 0x%x failed (status %u)\n",
			offset, status);
		return -EIO;
	}

	return 0;
}

static int p9220_program_otp(struct p9220_data *p9220, const u8 *data, size_t size)
{
	unsigned int offset;
	int ret;

	ret = regmap_write(p9220->regmap, P9220_KEY_REG, P9220_KEY_UNLOCK);
	if (ret)
		return ret;

	ret = regmap_write(p9220->regmap, P9220_M0_CTRL_REG, P9220_M0_HALT);
	if (ret)
		return ret;

	ret = p9220_load_otp_loader(p9220);
	if (ret)
		return ret;

	ret = regmap_write(p9220->regmap, P9220_CODE_REMAP_REG, P9220_REMAP_RAM_TO_OTP);
	if (ret)
		return ret;

	regmap_write(p9220->regmap, P9220_M0_CTRL_REG, P9220_M0_RUN);
	msleep(100);

	for (offset = 0; offset < size; offset += P9220_OTP_PAGE_LEN) {
		size_t page_len = min_t(size_t, P9220_OTP_PAGE_LEN, size - offset);

		ret = p9220_program_otp_page(p9220, offset, data + offset, page_len);
		if (ret) {
			dev_err(p9220->dev, "OTP programming failed at offset 0x%x: %d\n",
				offset, ret);
			return ret;
		}
	}

	regmap_write(p9220->regmap, P9220_KEY_REG, P9220_KEY_UNLOCK);
	return regmap_write(p9220->regmap, P9220_CODE_REMAP_REG, P9220_REMAP_NONE);
}

static void p9220_otp_work(struct work_struct *work)
{
	struct p9220_data *p9220 = container_of(work, struct p9220_data, otp_work);
	const struct firmware *fw;
	u16 version;
	int ret;

	disable_irq(p9220->irq_det);
	if (p9220->irq_int > 0)
		disable_irq(p9220->irq_int);

	ret = request_firmware(&fw, "idt/p9220_otp.bin", p9220->dev);
	if (ret) {
		dev_err(p9220->dev, "failed to load idt/p9220_otp.bin: %d\n", ret);
		goto out;
	}

	dev_info(p9220->dev, "OTP firmware version mismatch, programming %zu bytes\n", fw->size);
	ret = p9220_program_otp(p9220, fw->data, fw->size);
	release_firmware(fw);

	if (ret) {
		dev_err(p9220->dev, "OTP programming failed: %d\n", ret);
		goto out;
	}

	if (!p9220_read_otp_version(p9220, &version))
		dev_info(p9220->dev, "OTP firmware now at version 0x%x\n", version);

out:
	if (p9220->irq_int > 0)
		enable_irq(p9220->irq_int);
	enable_irq(p9220->irq_det);
}

static void p9220_det_work(struct work_struct *work)
{
	struct p9220_data *p9220 = container_of(work, struct p9220_data, det_work.work);
	bool present = gpiod_get_value_cansleep(p9220->detect);
	u16 version;

	if (present == p9220->present)
		return;
	p9220->present = present;

	if (present) {
		regmap_write(p9220->regmap, P9220_VOUT_SET_REG, P9220_VOUT_5V_VAL);
		p9220_apply_fod(p9220);
		regmap_update_bits(p9220->regmap, P9220_INT_ENABLE_L_REG,
				    P9220_STAT_MODE_CHANGE_MASK, P9220_STAT_MODE_CHANGE_MASK);

		if (!p9220->fw_flash_attempted &&
		    !p9220_read_otp_version(p9220, &version) &&
		    version != P9220_OTP_FW_VERSION_EXPECTED) {
			p9220->fw_flash_attempted = true;
			schedule_work(&p9220->otp_work);
		}
	}

	power_supply_changed(p9220->psy);
}

static irqreturn_t p9220_det_irq_thread(int irq, void *data)
{
	struct p9220_data *p9220 = data;

	schedule_delayed_work(&p9220->det_work, msecs_to_jiffies(50));
	return IRQ_HANDLED;
}

static irqreturn_t p9220_int_irq_thread(int irq, void *data)
{
	struct p9220_data *p9220 = data;
	u8 irq_src[2];

	if (regmap_raw_read(p9220->regmap, P9220_INT_L_REG, irq_src, sizeof(irq_src)))
		return IRQ_NONE;

	if (irq_src[0] & P9220_STAT_MODE_CHANGE_MASK)
		power_supply_changed(p9220->psy);

	regmap_write(p9220->regmap, P9220_INT_CLEAR_L_REG, irq_src[0]);
	regmap_write(p9220->regmap, P9220_INT_CLEAR_H_REG, irq_src[1]);
	regmap_update_bits(p9220->regmap, P9220_COMMAND_REG,
			    P9220_CMD_CLEAR_INT_MASK, P9220_CMD_CLEAR_INT_MASK);

	return IRQ_HANDLED;
}

static enum power_supply_property p9220_psy_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int p9220_get_property(struct power_supply *psy,
			       enum power_supply_property property,
			       union power_supply_propval *val)
{
	struct p9220_data *p9220 = power_supply_get_drvdata(psy);
	int ret;

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = p9220->present;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "IDT";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "P9220";
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (!p9220->present)
			return -ENODATA;
		ret = p9220_get_adc_mv(p9220, P9220_ADC_VOUT_L_REG, P9220_VOUT_FULL_SCALE_MV);
		if (ret < 0)
			return ret;
		val->intval = ret * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* raw ADC code - vendor's own driver leaves this scale factor unverified */
		if (!p9220->present)
			return -ENODATA;
		ret = p9220_get_adc_raw(p9220, P9220_ADC_RX_IOUT_L_REG);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		/* raw ADC code - vendor's own driver leaves this scale factor unverified */
		if (!p9220->present)
			return -ENODATA;
		ret = p9220_get_adc_raw(p9220, P9220_ADC_DIE_TEMP_L_REG);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc p9220_psy_desc = {
	.name = "p9220-wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = p9220_psy_properties,
	.num_properties = ARRAY_SIZE(p9220_psy_properties),
	.get_property = p9220_get_property,
};

static const struct regmap_config p9220_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xffff,
};

static int p9220_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct p9220_data *p9220;
	struct power_supply_config psy_cfg = {};
	u8 id[2];
	int ret;

	p9220 = devm_kzalloc(dev, sizeof(*p9220), GFP_KERNEL);
	if (!p9220)
		return -ENOMEM;

	p9220->dev = dev;
	i2c_set_clientdata(client, p9220);

	p9220->regmap = devm_regmap_init_i2c(client, &p9220_regmap_config);
	if (IS_ERR(p9220->regmap))
		return PTR_ERR(p9220->regmap);

	p9220->en = devm_gpiod_get(dev, "en", GPIOD_OUT_HIGH);
	if (IS_ERR(p9220->en))
		return dev_err_probe(dev, PTR_ERR(p9220->en), "failed to get enable GPIO\n");

	p9220->detect = devm_gpiod_get(dev, "detect", GPIOD_IN);
	if (IS_ERR(p9220->detect))
		return dev_err_probe(dev, PTR_ERR(p9220->detect),
				      "failed to get pad-detect GPIO\n");

	msleep(10);

	ret = regmap_raw_read(p9220->regmap, P9220_CHIP_ID_L_REG, id, sizeof(id));
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");
	dev_info(dev, "chip ID 0x%02x%02x\n", id[1], id[0]);

	if (!of_property_read_u8_array(dev->of_node, "idt,fod-data", p9220->fod_data,
					P9220_NUM_FOD_REG))
		p9220->have_fod = true;

	INIT_DELAYED_WORK(&p9220->det_work, p9220_det_work);
	INIT_WORK(&p9220->otp_work, p9220_otp_work);

	psy_cfg.drv_data = p9220;
	psy_cfg.fwnode = dev_fwnode(dev);
	p9220->psy = devm_power_supply_register(dev, &p9220_psy_desc, &psy_cfg);
	if (IS_ERR(p9220->psy))
		return PTR_ERR(p9220->psy);

	p9220->irq_det = gpiod_to_irq(p9220->detect);
	if (p9220->irq_det < 0)
		return p9220->irq_det;

	ret = devm_request_threaded_irq(dev, p9220->irq_det, NULL, p9220_det_irq_thread,
					 IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
					 dev_name(dev), p9220);
	if (ret)
		return ret;

	p9220->irq_int = client->irq;
	if (p9220->irq_int > 0) {
		ret = devm_request_threaded_irq(dev, p9220->irq_int, NULL,
						 p9220_int_irq_thread, IRQF_ONESHOT,
						 dev_name(dev), p9220);
		if (ret)
			return ret;
	}

	/* pick up a pad that's already present at probe time */
	schedule_delayed_work(&p9220->det_work, 0);

	return 0;
}

static void p9220_remove(struct i2c_client *client)
{
	struct p9220_data *p9220 = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&p9220->det_work);
	cancel_work_sync(&p9220->otp_work);
}

static const struct of_device_id p9220_of_match[] = {
	{ .compatible = "idt,p9220" },
	{ }
};
MODULE_DEVICE_TABLE(of, p9220_of_match);

static struct i2c_driver p9220_driver = {
	.driver = {
		.name = "p9220-charger",
		.of_match_table = p9220_of_match,
	},
	.probe = p9220_probe,
	.remove = p9220_remove,
};
module_i2c_driver(p9220_driver);

MODULE_FIRMWARE("idt/p9220_otp.bin");
MODULE_DESCRIPTION("IDT P9220 wireless power receiver");
MODULE_LICENSE("GPL");
