// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos mailbox controller
 *
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC
 * Copyright 2024 Linaro Ltd.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/exynos-message.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

struct exynos_mbox_data {
	u32 intgr1;
	u32 intcr0;
	u32 intmr0;
	u32 intsr0;
	u32 rx_mask;
	u8 rx_shift;
	u8 num_chans;
	bool indexed_channels;
	bool has_rx_irq;
	bool needs_clock;
};

struct exynos_mbox {
	void __iomem *regs;
	struct mbox_controller *mbox;
	const struct exynos_mbox_data *data;
	spinlock_t lock;
};

static unsigned int exynos_mbox_chan_id(struct mbox_chan *chan)
{
	return chan - chan->mbox->chans;
}

static int exynos_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(chan->mbox->dev);
	const struct exynos_mbox_data *match = exynos_mbox->data;
	unsigned int id;

	if (match->indexed_channels) {
		id = exynos_mbox_chan_id(chan);
	} else {
		struct exynos_mbox_msg *msg = data;

		if (!msg || msg->chan_type != EXYNOS_MBOX_CHAN_TYPE_DOORBELL)
			return -EINVAL;
		id = msg->chan_id;
	}
	if (id >= match->num_chans)
		return -EINVAL;

	writel(BIT(id), exynos_mbox->regs + match->intgr1);
	return 0;
}

static int exynos_mbox_startup(struct mbox_chan *chan)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(chan->mbox->dev);
	const struct exynos_mbox_data *match = exynos_mbox->data;
	unsigned long flags;
	u32 mask;

	if (!match->has_rx_irq)
		return 0;
	spin_lock_irqsave(&exynos_mbox->lock, flags);
	mask = readl(exynos_mbox->regs + match->intmr0);
	mask &= ~BIT(exynos_mbox_chan_id(chan) + match->rx_shift);
	writel(mask, exynos_mbox->regs + match->intmr0);
	spin_unlock_irqrestore(&exynos_mbox->lock, flags);
	return 0;
}

static void exynos_mbox_shutdown(struct mbox_chan *chan)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(chan->mbox->dev);
	const struct exynos_mbox_data *match = exynos_mbox->data;
	unsigned long flags;
	u32 mask;

	if (!match->has_rx_irq)
		return;
	spin_lock_irqsave(&exynos_mbox->lock, flags);
	mask = readl(exynos_mbox->regs + match->intmr0);
	mask |= BIT(exynos_mbox_chan_id(chan) + match->rx_shift);
	writel(mask, exynos_mbox->regs + match->intmr0);
	spin_unlock_irqrestore(&exynos_mbox->lock, flags);
}

static const struct mbox_chan_ops exynos_mbox_chan_ops = {
	.send_data = exynos_mbox_send_data,
	.startup = exynos_mbox_startup,
	.shutdown = exynos_mbox_shutdown,
};

static irqreturn_t exynos_mbox_irq(int irq, void *data)
{
	struct exynos_mbox *exynos_mbox = data;
	const struct exynos_mbox_data *match = exynos_mbox->data;
	u32 status;
	int i;

	status = readl(exynos_mbox->regs + match->intsr0) & match->rx_mask;
	if (!status)
		return IRQ_NONE;

	status >>= match->rx_shift;
	for (i = 0; i < match->num_chans; i++)
		if ((status & BIT(i)) && exynos_mbox->mbox->chans[i].cl)
			mbox_chan_received_data(&exynos_mbox->mbox->chans[i], NULL);
	writel(status << match->rx_shift,
	       exynos_mbox->regs + match->intcr0);
	return IRQ_HANDLED;
}

static struct mbox_chan *exynos_mbox_of_xlate(struct mbox_controller *mbox,
					      const struct of_phandle_args *sp)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(mbox->dev);
	int i;

	if (exynos_mbox->data->indexed_channels) {
		if (sp->args_count != 1 || sp->args[0] >= mbox->num_chans)
			return ERR_PTR(-EINVAL);
		return &mbox->chans[sp->args[0]];
	}
	if (sp->args_count != 0)
		return ERR_PTR(-EINVAL);
	for (i = 0; i < mbox->num_chans; i++)
		if (!mbox->chans[i].cl)
			return &mbox->chans[i];
	return ERR_PTR(-EBUSY);
}

static const struct exynos_mbox_data gs101_mbox_data = {
	.intgr1 = 0x40,
	.intmr0 = 0x28,
	.rx_mask = GENMASK(15, 0),
	.num_chans = 16,
	.needs_clock = true,
};

static const struct exynos_mbox_data exynos8890_mbox_data = {
	.intgr1 = 0x1c,
	.intcr0 = 0x0c,
	.intmr0 = 0x10,
	.intsr0 = 0x14,
	.rx_mask = GENMASK(31, 16),
	.rx_shift = 16,
	.num_chans = 16,
	.indexed_channels = true,
	.has_rx_irq = true,
};

static const struct of_device_id exynos_mbox_match[] = {
	{ .compatible = "google,gs101-mbox", .data = &gs101_mbox_data },
	{ .compatible = "samsung,exynos8890-mbox", .data = &exynos8890_mbox_data },
	{},
};
MODULE_DEVICE_TABLE(of, exynos_mbox_match);

static int exynos_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_mbox *exynos_mbox;
	struct mbox_controller *mbox;
	struct mbox_chan *chans;
	struct clk *pclk;
	int irq, ret;

	exynos_mbox = devm_kzalloc(dev, sizeof(*exynos_mbox), GFP_KERNEL);
	if (!exynos_mbox)
		return -ENOMEM;
	exynos_mbox->data = device_get_match_data(dev);
	if (!exynos_mbox->data)
		return -EINVAL;
	spin_lock_init(&exynos_mbox->lock);

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	chans = devm_kcalloc(dev, exynos_mbox->data->num_chans,
			     sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	exynos_mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(exynos_mbox->regs))
		return PTR_ERR(exynos_mbox->regs);
	if (exynos_mbox->data->needs_clock) {
		pclk = devm_clk_get_enabled(dev, "pclk");
		if (IS_ERR(pclk))
			return dev_err_probe(dev, PTR_ERR(pclk),
					     "failed to enable clock\n");
	}

	mbox->num_chans = exynos_mbox->data->num_chans;
	mbox->chans = chans;
	mbox->dev = dev;
	mbox->ops = &exynos_mbox_chan_ops;
	mbox->of_xlate = exynos_mbox_of_xlate;
	exynos_mbox->mbox = mbox;
	platform_set_drvdata(pdev, exynos_mbox);

	writel(exynos_mbox->data->rx_mask,
	       exynos_mbox->regs + exynos_mbox->data->intmr0);
	if (exynos_mbox->data->has_rx_irq) {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0)
			return irq;
		ret = devm_request_irq(dev, irq, exynos_mbox_irq, 0,
				       dev_name(dev), exynos_mbox);
		if (ret)
			return ret;
	}

	return devm_mbox_controller_register(dev, mbox);
}

static struct platform_driver exynos_mbox_driver = {
	.probe = exynos_mbox_probe,
	.driver = {
		.name = "exynos-mailbox",
		.of_match_table = exynos_mbox_match,
	},
};
module_platform_driver(exynos_mbox_driver);

MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Samsung Exynos mailbox driver");
MODULE_LICENSE("GPL");
