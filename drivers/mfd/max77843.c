// SPDX-License-Identifier: GPL-2.0+
//
// MFD core driver for the Maxim MAX77843/MAX77854
//
// Copyright (C) 2015 Samsung Electronics
// Author: Jaewon Kim <jaewon02.kim@samsung.com>
// Author: Beomho Seo <beomho.seo@samsung.com>

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77843-private.h>
#include <linux/platform_device.h>

static const struct mfd_cell max77843_devs[] = {
	{
		.name = "max77843-muic",
		.of_compatible = "maxim,max77843-muic",
	}, {
		.name = "max77843-regulator",
		.of_compatible = "maxim,max77843-regulator",
	}, {
		.name = "max77843-charger",
		.of_compatible = "maxim,max77843-charger"
	}, {
		.name = "max77843-fuelgauge",
		.of_compatible = "maxim,max77843-fuelgauge",
	}, {
		.name = "max77843-haptic",
		.of_compatible = "maxim,max77843-haptic",
	},
};

/*
 * MAX77854 is register-compatible with MAX77843 for the functions enabled
 * here. Fuel-gauge support remains intentionally omitted until its
 * MAX77854-specific behaviour is validated.
 */
static const struct mfd_cell max77854_devs[] = {
	{
		.name = "max77843-muic",
		.of_compatible = "maxim,max77843-muic",
	}, {
		.name = "max77854-regulator",
		.of_compatible = "maxim,max77843-regulator",
	}, {
		.name = "max77854-charger",
		.of_compatible = "maxim,max77854-charger",
	}, {
		.name = "max77854-fuel-gauge",
		.of_compatible = "maxim,max77854-fuel-gauge",
	}, {
		.name = "max77843-haptic",
		.of_compatible = "maxim,max77843-haptic",
	},
};

static const struct regmap_config max77843_charger_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= MAX77843_CHG_REG_END,
};

static const struct regmap_config max77843_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= MAX77843_SYS_REG_END,
};

static bool max77854_fg_writeable_reg(struct device *dev, unsigned int reg)
{
	return false;
}

static const struct regmap_config max77854_fg_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.val_format_endian	= REGMAP_ENDIAN_LITTLE,
	.max_register		= 0xff,
	.cache_type		= REGCACHE_NONE,
	.writeable_reg		= max77854_fg_writeable_reg,
};

static const struct regmap_irq max77843_irqs[] = {
	/* TOPSYS interrupts */
	{ .reg_offset = 0, .mask = MAX77843_SYS_IRQ_SYSUVLO_INT, },
	{ .reg_offset = 0, .mask = MAX77843_SYS_IRQ_SYSOVLO_INT, },
	{ .reg_offset = 0, .mask = MAX77843_SYS_IRQ_TSHDN_INT, },
	{ .reg_offset = 0, .mask = MAX77843_SYS_IRQ_TM_INT, },
};

static const struct regmap_irq_chip max77843_irq_chip = {
	.name		= "max77843",
	.status_base	= MAX77843_SYS_REG_SYSINTSRC,
	.mask_base	= MAX77843_SYS_REG_SYSINTMASK,
	.num_regs	= 1,
	.irqs		= max77843_irqs,
	.num_irqs	= ARRAY_SIZE(max77843_irqs),
};

/* Charger and Charger regulator use same regmap. */
static int max77843_chg_init(struct max77693_dev *max77843)
{
	max77843->i2c_chg = devm_i2c_new_dummy_device(max77843->dev,
						      max77843->i2c->adapter,
						      I2C_ADDR_CHG);
	if (IS_ERR(max77843->i2c_chg))
		return dev_err_probe(max77843->dev, PTR_ERR(max77843->i2c_chg),
				     "failed to allocate charger I2C client\n");

	i2c_set_clientdata(max77843->i2c_chg, max77843);
	max77843->regmap_chg = devm_regmap_init_i2c(max77843->i2c_chg,
						    &max77843_charger_regmap_config);
	if (IS_ERR(max77843->regmap_chg))
		return dev_err_probe(max77843->dev, PTR_ERR(max77843->regmap_chg),
				     "failed to initialize charger regmap\n");

	return 0;
}

static int max77854_fg_init(struct max77693_dev *max77843)
{
	max77843->i2c_fg = devm_i2c_new_dummy_device(max77843->dev,
						     max77843->i2c->adapter,
						     I2C_ADDR_FG);
	if (IS_ERR(max77843->i2c_fg))
		return dev_err_probe(max77843->dev, PTR_ERR(max77843->i2c_fg),
				     "failed to allocate fuel-gauge I2C client\n");

	i2c_set_clientdata(max77843->i2c_fg, max77843);
	max77843->regmap_fg = devm_regmap_init_i2c(max77843->i2c_fg,
						   &max77854_fg_regmap_config);
	if (IS_ERR(max77843->regmap_fg))
		return dev_err_probe(max77843->dev, PTR_ERR(max77843->regmap_fg),
				     "failed to initialize fuel-gauge regmap\n");

	return 0;
}

static int max77843_probe(struct i2c_client *i2c)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(i2c);
	const struct mfd_cell *cells;
	struct max77693_dev *max77843;
	unsigned int cells_size;
	unsigned int intsrc_mask;
	unsigned int reg_data;
	int ret;

	max77843 = devm_kzalloc(&i2c->dev, sizeof(*max77843), GFP_KERNEL);
	if (!max77843)
		return -ENOMEM;

	i2c_set_clientdata(i2c, max77843);
	max77843->dev = &i2c->dev;
	max77843->i2c = i2c;
	max77843->irq = i2c->irq;
	max77843->type = id->driver_data;

	max77843->regmap = devm_regmap_init_i2c(i2c,
			&max77843_regmap_config);
	if (IS_ERR(max77843->regmap)) {
		dev_err(&i2c->dev, "Failed to allocate topsys register map\n");
		return PTR_ERR(max77843->regmap);
	}

	ret = regmap_add_irq_chip(max77843->regmap, max77843->irq,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED,
			0, &max77843_irq_chip, &max77843->irq_data_topsys);
	if (ret) {
		dev_err(&i2c->dev, "Failed to add TOPSYS IRQ chip\n");
		return ret;
	}

	ret = regmap_read(max77843->regmap,
			MAX77843_SYS_REG_PMICID, &reg_data);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read PMIC ID\n");
		goto err_pmic_id;
	}
	dev_info(&i2c->dev, "device ID: 0x%x\n", reg_data);

	ret = max77843_chg_init(max77843);
	if (ret) {
		dev_err(&i2c->dev, "Failed to init Charger\n");
		goto err_pmic_id;
	}

	if (max77843->type == TYPE_MAX77854) {
		ret = max77854_fg_init(max77843);
		if (ret)
			goto err_pmic_id;

		cells = max77854_devs;
		cells_size = ARRAY_SIZE(max77854_devs);
		/*
		 * Keep charger and fuel-gauge sources masked here. The MAX77854
		 * charger child unmasks CHGR only after its own IRQ chip and
		 * handlers are ready.
		 */
		intsrc_mask = MAX77843_INTSRCMASK_CHGR_MASK |
			      MAX77843_INTSRCMASK_FG_MASK;
	} else {
		cells = max77843_devs;
		cells_size = ARRAY_SIZE(max77843_devs);
		intsrc_mask = 0;
	}

	ret = regmap_update_bits(max77843->regmap,
				 MAX77843_SYS_REG_INTSRCMASK,
				 MAX77843_INTSRC_MASK_MASK,
				 intsrc_mask);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to configure interrupt sources\n");
		goto err_pmic_id;
	}

	ret = mfd_add_devices(max77843->dev, -1, cells, cells_size,
			      NULL, 0, NULL);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to add mfd device\n");
		goto err_pmic_id;
	}

	device_init_wakeup(max77843->dev, true);

	return 0;

err_pmic_id:
	regmap_del_irq_chip(max77843->irq, max77843->irq_data_topsys);

	return ret;
}

static const struct of_device_id max77843_dt_match[] = {
	{ .compatible = "maxim,max77843", },
	{ .compatible = "maxim,max77854", },
	{ },
};

static const struct i2c_device_id max77843_id[] = {
	{ "max77843", TYPE_MAX77843, },
	{ "max77854", TYPE_MAX77854, },
	{ },
};

static void max77843_remove(struct i2c_client *i2c)
{
	struct max77693_dev *max77843 = i2c_get_clientdata(i2c);

	mfd_remove_devices(max77843->dev);
	regmap_del_irq_chip(max77843->irq, max77843->irq_data_topsys);
}

static int __maybe_unused max77843_suspend(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77693_dev *max77843 = i2c_get_clientdata(i2c);

	disable_irq(max77843->irq);
	if (device_may_wakeup(dev))
		enable_irq_wake(max77843->irq);

	return 0;
}

static int __maybe_unused max77843_resume(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77693_dev *max77843 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev))
		disable_irq_wake(max77843->irq);
	enable_irq(max77843->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(max77843_pm, max77843_suspend, max77843_resume);

static struct i2c_driver max77843_i2c_driver = {
	.driver	= {
		.name = "max77843",
		.pm = &max77843_pm,
		.of_match_table = max77843_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = max77843_probe,
	.remove = max77843_remove,
	.id_table = max77843_id,
};

static int __init max77843_i2c_init(void)
{
	return i2c_add_driver(&max77843_i2c_driver);
}
subsys_initcall(max77843_i2c_init);
