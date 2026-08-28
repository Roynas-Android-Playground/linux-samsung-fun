// SPDX-License-Identifier: GPL-2.0-only
/*
 * ABOV mc96ft16xx capacitive touchkey (back / recent-apps, flanking the
 * physical home button)
 *
 * Register map and the ISP firmware-update protocol are ported from
 * Samsung's GPL vendor driver (drivers/input/keyboard/abov_touchkey.c).
 * LED backlight control, glove/keyboard/flip modes and the factory raw/diff
 * sysfs debug attributes aren't ported - key events and firmware bring-up
 * only, RF/EMI tuning and SDCARD firmware loading also dropped.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define ABOV_BTNSTATUS		0x00
#define ABOV_FW_VER		0x01
#define ABOV_MODEL_NUMBER	0x14

#define ABOV_BOOT_DELAY_MS	45
#define ABOV_RESET_DELAY_MS	150
#define ABOV_PAGE_LEN		32
#define ABOV_FW_START_ADDR	0x1000

struct abov_touchkey {
	struct i2c_client *client;
	struct regmap *regmap;
	struct input_dev *input;
	struct regulator *vdd;
	struct mutex lock;
	int irq;
};

static int abov_power(struct abov_touchkey *abov, bool on)
{
	return on ? regulator_enable(abov->vdd) : regulator_disable(abov->vdd);
}

static int abov_reset_for_bootmode(struct abov_touchkey *abov)
{
	int ret;

	ret = abov_power(abov, false);
	if (ret)
		return ret;
	msleep(50);
	return abov_power(abov, true);
}

static irqreturn_t abov_irq_thread(int irq, void *data)
{
	struct abov_touchkey *abov = data;
	unsigned int status;
	int menu_data, back_data;

	if (regmap_read(abov->regmap, ABOV_BTNSTATUS, &status))
		return IRQ_NONE;

	menu_data = status & 0x03;
	back_data = (status >> 2) & 0x03;

	if (menu_data)
		input_report_key(abov->input, KEY_APPSELECT, !(menu_data % 2));
	if (back_data)
		input_report_key(abov->input, KEY_BACK, !(back_data % 2));

	input_sync(abov->input);
	return IRQ_HANDLED;
}

static int abov_fw_enter_mode(struct abov_touchkey *abov)
{
	u8 cmd[3] = { 0xac, 0x5b, 0x2d };
	int ret;

	ret = i2c_master_send(abov->client, cmd, sizeof(cmd));
	return ret == sizeof(cmd) ? 0 : (ret < 0 ? ret : -EIO);
}

static int abov_fw_check_busy(struct abov_touchkey *abov)
{
	u8 val;
	int ret, count;

	for (count = 0; count < 1000; count++) {
		ret = i2c_master_recv(abov->client, &val, sizeof(val));
		if (ret < 0)
			return ret;
		if (!(val & 0x01))
			return 0;
	}

	return -ETIMEDOUT;
}

static int abov_fw_write_page(struct abov_touchkey *abov, u16 addr, const u8 *data)
{
	u8 buf[4 + ABOV_PAGE_LEN];
	int ret;

	buf[0] = 0xac;
	buf[1] = 0x7a;
	buf[2] = addr >> 8;
	buf[3] = addr & 0xff;
	memcpy(buf + 4, data, ABOV_PAGE_LEN);

	ret = i2c_master_send(abov->client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	usleep_range(3000, 3000);
	return abov_fw_check_busy(abov);
}

static int abov_fw_read_checksum(struct abov_touchkey *abov, u8 *checksum_h, u8 *checksum_l)
{
	u8 cmd[6] = { 0xac, 0x9e, 0x10, 0x00, 0x3f, 0xff };
	u8 result[5];
	int ret;

	ret = i2c_master_send(abov->client, cmd, sizeof(cmd));
	if (ret != sizeof(cmd))
		return ret < 0 ? ret : -EIO;

	usleep_range(5000, 5000);
	ret = abov_fw_check_busy(abov);
	if (ret)
		return ret;

	ret = i2c_master_recv(abov->client, result, sizeof(result));
	if (ret != sizeof(result))
		return ret < 0 ? ret : -EIO;

	*checksum_h = result[3];
	*checksum_l = result[4];
	return 0;
}

static int abov_flash_fw(struct abov_touchkey *abov, const struct firmware *fw)
{
	u8 checksum_h, checksum_l;
	u16 addr = ABOV_FW_START_ADDR;
	size_t pos;
	int ret, retry;

	for (retry = 0; retry < 2; retry++) {
		ret = abov_reset_for_bootmode(abov);
		if (ret)
			return ret;
		msleep(ABOV_BOOT_DELAY_MS);

		ret = abov_fw_enter_mode(abov);
		if (ret)
			continue;
		msleep(1100);

		addr = ABOV_FW_START_ADDR;
		/* first 32-byte block is the header, not flashed */
		for (pos = ABOV_PAGE_LEN; pos + ABOV_PAGE_LEN <= fw->size; pos += ABOV_PAGE_LEN) {
			ret = abov_fw_write_page(abov, addr, fw->data + pos);
			if (ret)
				break;
			addr += ABOV_PAGE_LEN;
		}
		if (ret)
			continue;

		ret = abov_fw_read_checksum(abov, &checksum_h, &checksum_l);
		if (ret)
			continue;

		if (checksum_h != fw->data[8] || checksum_l != fw->data[9]) {
			dev_err(&abov->client->dev,
				"checksum mismatch (0x%02x%02x != 0x%02x%02x), retry %d\n",
				checksum_h, checksum_l, fw->data[8], fw->data[9], retry);
			ret = -EIO;
			continue;
		}

		ret = abov_reset_for_bootmode(abov);
		if (ret)
			return ret;
		msleep(ABOV_RESET_DELAY_MS);
		return 0;
	}

	return ret;
}

static int abov_fw_check(struct abov_touchkey *abov)
{
	const struct firmware *fw;
	unsigned int fw_ver, model;
	int ret;

	ret = request_firmware(&fw, "abov/abov_hero.fw", &abov->client->dev);
	if (ret)
		return ret;

	ret = regmap_read(abov->regmap, ABOV_MODEL_NUMBER, &model);
	if (ret)
		goto out;

	ret = regmap_read(abov->regmap, ABOV_FW_VER, &fw_ver);
	if (ret)
		goto out;

	if (model != fw->data[1] || fw_ver == 0 || fw_ver > 0xf0 || fw_ver < fw->data[5]) {
		dev_info(&abov->client->dev,
			 "firmware update needed (chip 0x%02x/0x%02x, bundled 0x%02x/0x%02x)\n",
			 model, fw_ver, fw->data[1], fw->data[5]);
		ret = abov_flash_fw(abov, fw);
		if (ret) {
			dev_err(&abov->client->dev, "firmware update failed: %d\n", ret);
			goto out;
		}

		ret = regmap_read(abov->regmap, ABOV_FW_VER, &fw_ver);
		if (ret)
			goto out;
		dev_info(&abov->client->dev, "firmware now at version 0x%02x\n", fw_ver);
	}

out:
	release_firmware(fw);
	return ret;
}

static const struct regmap_config abov_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int abov_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct abov_touchkey *abov;
	int ret;

	abov = devm_kzalloc(dev, sizeof(*abov), GFP_KERNEL);
	if (!abov)
		return -ENOMEM;

	abov->client = client;
	mutex_init(&abov->lock);
	i2c_set_clientdata(client, abov);

	abov->regmap = devm_regmap_init_i2c(client, &abov_regmap_config);
	if (IS_ERR(abov->regmap))
		return PTR_ERR(abov->regmap);

	abov->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(abov->vdd))
		return dev_err_probe(dev, PTR_ERR(abov->vdd), "failed to get vdd\n");

	ret = abov_power(abov, true);
	if (ret)
		return ret;
	msleep(ABOV_RESET_DELAY_MS);

	ret = abov_fw_check(abov);
	if (ret)
		return dev_err_probe(dev, ret, "firmware check failed\n");

	abov->input = devm_input_allocate_device(dev);
	if (!abov->input)
		return -ENOMEM;

	abov->input->name = "sec_touchkey";
	abov->input->id.bustype = BUS_HOST;
	input_set_capability(abov->input, EV_KEY, KEY_APPSELECT);
	input_set_capability(abov->input, EV_KEY, KEY_BACK);
	input_set_drvdata(abov->input, abov);

	ret = input_register_device(abov->input);
	if (ret)
		return ret;

	abov->irq = client->irq;
	ret = devm_request_threaded_irq(dev, abov->irq, NULL, abov_irq_thread,
					 IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					 "abov-touchkey", abov);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id abov_of_match[] = {
	{ .compatible = "abov,mc96ft16xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, abov_of_match);

static struct i2c_driver abov_touchkey_driver = {
	.driver = {
		.name = "abov-touchkey",
		.of_match_table = abov_of_match,
	},
	.probe = abov_probe,
};
module_i2c_driver(abov_touchkey_driver);

MODULE_FIRMWARE("abov/abov_hero.fw");
MODULE_DESCRIPTION("ABOV mc96ft16xx touchkey driver");
MODULE_LICENSE("GPL");
