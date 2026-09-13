// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2024 Linaro Ltd.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/exynos-message.h>
#include <linux/mailbox/exynos8890-mcu-ipc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define EXYNOS_MBOX_INTMR0		0x28	/* Interrupt Mask Register 0 */
#define EXYNOS_MBOX_INTGR1		0x40	/* Interrupt Generation Register 1 */

#define EXYNOS_MBOX_INTMR0_MASK		GENMASK(15, 0)
#define EXYNOS_MBOX_INTGR1_MASK		GENMASK(15, 0)

#define EXYNOS_MBOX_CHAN_COUNT		HWEIGHT32(EXYNOS_MBOX_INTGR1_MASK)

#define EXYNOS850_MBOX_INTGR0		0x8	/* Interrupt Generation Register 0	*/
#define EXYNOS850_MBOX_INTMR1		0x24	/* Interrupt Mask Register 1		*/

#define EXYNOS850_MBOX_INTMR1_MASK	GENMASK(15, 0)
#define EXYNOS850_MBOX_INTGR0_MASK	GENMASK(31, 16)

#define EXYNOS850_MBOX_CHAN_COUNT	HWEIGHT32(EXYNOS850_MBOX_INTGR0_MASK)

/*
 * The exynos8890 MCU_IPC block (the AP<->CP modem doorbell interconnect)
 * splits the two directions across separate register groups instead of
 * sharing one like the ACPM-style mailboxes above: INTGR1 generates a
 * doorbell towards CP, while INTSR0/INTMR0/INTCR0 report, mask and
 * acknowledge doorbells CP rings towards the AP.
 */
#define EXYNOS8890_MBOX_INTCR0		0xc	/* CP->AP IRQ clear (w1c) */
#define EXYNOS8890_MBOX_INTMR0		0x10	/* CP->AP IRQ mask */
#define EXYNOS8890_MBOX_INTSR0		0x14	/* CP->AP IRQ status */
#define EXYNOS8890_MBOX_INTGR1		0x1c	/* AP->CP IRQ generate */

#define EXYNOS8890_MBOX_RX_SHIFT	16
#define EXYNOS8890_MBOX_CHAN_COUNT	16

/**
 * struct exynos_mbox_driver_data - platform-specific mailbox configuration.
 * @intgr:		offset to the IRQ generation register, doorbell
 *			to APM co-processor.
 * @intgr_shift:	shift to apply to the value written to IRQ generation
 *			register.
 * @intmr:		offset to the IRQ mask register.
 * @intmr_mask:		value to write to the mask register to mask out all
 *			interrupts.
 * @num_chans:		number of channels the mailbox can support (hardware
 *			capability).
 * @rx_intsr:		offset to the incoming-doorbell status register, 0 if
 *			this controller has no receive side (the common
 *			ACPM-style case).
 * @rx_intcr:		offset to the incoming-doorbell clear register.
 * @rx_intmr:		offset to the incoming-doorbell mask register.
 * @rx_shift:		bit shift of channel 0 within the rx_int* registers.
 */
struct exynos_mbox_driver_data {
	u32 intgr;
	u32 intgr_shift;
	u32 intmr;
	u32 intmr_mask;
	int num_chans;
	u32 rx_intsr;
	u32 rx_intcr;
	u32 rx_intmr;
	u32 rx_shift;
};

/**
 * struct exynos_mbox - driver's private data.
 * @regs:	mailbox registers base address.
 * @mbox:	pointer to the mailbox controller.
 * @data:	pointer to driver platform-specific data.
 * @rx_lock:	serializes receive-channel publication and revocation.
 * @rx_enabled:	channels whose clients may be called by the IRQ.
 * @rx_paused:	channels temporarily revoked by their owners.
 * @rx_pause_depth: balanced owner pauses, protected by rx_lock.
 * @irq:	receive IRQ to drain before the mailbox core clears chan->cl.
 */
struct exynos_mbox {
	void __iomem *regs;
	struct mbox_controller *mbox;
	const struct exynos_mbox_driver_data *data;
	spinlock_t rx_lock;
	unsigned long rx_enabled;
	unsigned long rx_paused;
	unsigned int rx_pause_depth[EXYNOS8890_MBOX_CHAN_COUNT];
	int irq;
};

static const struct exynos_mbox_driver_data exynos850_mbox_data = {
	.intgr = EXYNOS850_MBOX_INTGR0,
	.intgr_shift = 16,
	.intmr = EXYNOS850_MBOX_INTMR1,
	.intmr_mask = EXYNOS850_MBOX_INTMR1_MASK,
	.num_chans = EXYNOS850_MBOX_CHAN_COUNT,
};

static const struct exynos_mbox_driver_data exynos_gs101_mbox_data = {
	.intgr = EXYNOS_MBOX_INTGR1,
	.intgr_shift = 0,
	.intmr = EXYNOS_MBOX_INTMR0,
	.intmr_mask = EXYNOS_MBOX_INTMR0_MASK,
	.num_chans = EXYNOS_MBOX_CHAN_COUNT,
};

static const struct exynos_mbox_driver_data exynos8890_mbox_data = {
	.intgr = EXYNOS8890_MBOX_INTGR1,
	.intgr_shift = 0,
	.num_chans = EXYNOS8890_MBOX_CHAN_COUNT,
	.rx_intsr = EXYNOS8890_MBOX_INTSR0,
	.rx_intcr = EXYNOS8890_MBOX_INTCR0,
	.rx_intmr = EXYNOS8890_MBOX_INTMR0,
	.rx_shift = EXYNOS8890_MBOX_RX_SHIFT,
};

static int exynos_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct device *dev = chan->mbox->dev;
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(dev);
	struct exynos_mbox_msg *msg = data;

	if (msg->chan_id >= exynos_mbox->mbox->num_chans) {
		dev_err(dev, "Invalid channel ID %d\n", msg->chan_id);
		return -EINVAL;
	}

	if (msg->chan_type != EXYNOS_MBOX_CHAN_TYPE_DOORBELL) {
		dev_err(dev, "Unsupported channel type [%d]\n", msg->chan_type);
		return -EINVAL;
	}

	/* Ring the doorbell */
	writel(BIT(msg->chan_id) << exynos_mbox->data->intgr_shift,
	       exynos_mbox->regs + exynos_mbox->data->intgr);

	return 0;
}

static const struct mbox_chan_ops exynos_mbox_chan_ops = {
	.send_data = exynos_mbox_send_data,
};

static struct mbox_chan *exynos_mbox_of_xlate(struct mbox_controller *mbox,
					      const struct of_phandle_args *sp)
{
	int i;

	if (sp->args_count != 0)
		return ERR_PTR(-EINVAL);

	/*
	 * Return the first available channel. When we don't pass the
	 * channel ID from device tree, each channel populated by the driver is
	 * just a software construct or a virtual channel. We use 'void *data'
	 * in send_data() to pass the channel identifiers.
	 */
	for (i = 0; i < mbox->num_chans; i++)
		if (mbox->chans[i].cl == NULL)
			return &mbox->chans[i];
	return ERR_PTR(-EINVAL);
}

/*
 * exynos8890 MCU_IPC has a fixed 1:1 mapping between DT-requested channel
 * and physical doorbell bit (mboxes = <&mcu_ipc 0>, <&mcu_ipc 1>, ... in the
 * consumer, one entry per doorbell), unlike the ACPM-style mailboxes above
 * where the client picks any free channel and the doorbell ID travels in
 * the message payload. Reflect that with a dedicated of_xlate/send_data
 * pair that key off the channel's own index in mbox->chans[] instead of
 * struct exynos_mbox_msg, since consumers such as exynos8890-cpctl.c ring
 * a doorbell with mbox_send_message(chan, NULL).
 */
static struct mbox_chan *exynos8890_mbox_of_xlate(struct mbox_controller *mbox,
						   const struct of_phandle_args *sp)
{
	if (sp->args_count != 1 || sp->args[0] >= mbox->num_chans)
		return ERR_PTR(-EINVAL);

	return &mbox->chans[sp->args[0]];
}

static int exynos8890_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(chan->mbox->dev);
	unsigned int chan_id = chan - chan->mbox->chans;

	writel(BIT(chan_id) << exynos_mbox->data->intgr_shift,
	       exynos_mbox->regs + exynos_mbox->data->intgr);

	return 0;
}

static int exynos8890_mbox_startup(struct mbox_chan *chan)
{
	struct exynos_mbox *mbox = dev_get_drvdata(chan->mbox->dev);
	unsigned long flags;

	spin_lock_irqsave(&mbox->rx_lock, flags);
	__set_bit(chan - chan->mbox->chans, &mbox->rx_enabled);
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	return 0;
}

static void exynos8890_mbox_shutdown(struct mbox_chan *chan)
{
	struct exynos_mbox *mbox = dev_get_drvdata(chan->mbox->dev);
	unsigned long flags;

	spin_lock_irqsave(&mbox->rx_lock, flags);
	__clear_bit(chan - chan->mbox->chans, &mbox->rx_enabled);
	__clear_bit(chan - chan->mbox->chans, &mbox->rx_paused);
	mbox->rx_pause_depth[chan - chan->mbox->chans] = 0;
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	/* mbox_free_channel() clears chan->cl AFTER ->shutdown(). No IRQ
	 * may retain that client when this returns, including an IRQ which
	 * took its enabled snapshot before revocation.
	 */
	synchronize_irq(mbox->irq);
}

static const struct mbox_chan_ops exynos8890_mbox_chan_ops = {
	.send_data = exynos8890_mbox_send_data,
	.startup = exynos8890_mbox_startup,
	.shutdown = exynos8890_mbox_shutdown,
};

int exynos8890_mbox_rx_pause(struct mbox_chan *chan)
{
	struct exynos_mbox *mbox;
	unsigned long flags;
	unsigned int id;

	/* Validate the provider before interpreting its private data. */
	if (!chan || chan->mbox->ops != &exynos8890_mbox_chan_ops)
		return -EOPNOTSUPP;
	mbox = dev_get_drvdata(chan->mbox->dev);
	id = chan - chan->mbox->chans;
	spin_lock_irqsave(&mbox->rx_lock, flags);
	if (mbox->rx_pause_depth[id] == UINT_MAX) {
		spin_unlock_irqrestore(&mbox->rx_lock, flags);
		return -EOVERFLOW;
	}
	mbox->rx_pause_depth[id]++;
	__set_bit(id, &mbox->rx_paused);
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	/* Also drain an IRQ which already took its delivery snapshot. */
	synchronize_irq(mbox->irq);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_mbox_rx_pause);

int exynos8890_mbox_rx_resume(struct mbox_chan *chan)
{
	struct exynos_mbox *mbox;
	unsigned long flags;
	unsigned int id;

	if (!chan || chan->mbox->ops != &exynos8890_mbox_chan_ops)
		return -EOPNOTSUPP;
	mbox = dev_get_drvdata(chan->mbox->dev);
	id = chan - chan->mbox->chans;
	spin_lock_irqsave(&mbox->rx_lock, flags);
	if (!mbox->rx_pause_depth[id]) {
		spin_unlock_irqrestore(&mbox->rx_lock, flags);
		return -EINVAL;
	}
	if (!--mbox->rx_pause_depth[id]) {
		/* Discard this channel's frozen doorbell, not another client's.
		 * Snapshot/ack and publication share rx_lock, so no IRQ can
		 * carry frozen status across this boundary into the new epoch.
		 * Never change rx_enabled: shutdown remains authoritative.
		 */
		writel(BIT(id) << mbox->data->rx_shift,
		       mbox->regs + mbox->data->rx_intcr);
		__clear_bit(id, &mbox->rx_paused);
	}
	spin_unlock_irqrestore(&mbox->rx_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_mbox_rx_resume);

static irqreturn_t exynos8890_mbox_irq(int irq, void *dev_id)
{
	struct exynos_mbox *exynos_mbox = dev_id;
	const struct exynos_mbox_driver_data *data = exynos_mbox->data;
	unsigned long status, enabled, flags;
	int chan_id;

	spin_lock_irqsave(&exynos_mbox->rx_lock, flags);
	status = readl(exynos_mbox->regs + data->rx_intsr) >> data->rx_shift;
	if (!status) {
		spin_unlock_irqrestore(&exynos_mbox->rx_lock, flags);
		return IRQ_NONE;
	}

	writel(status << data->rx_shift, exynos_mbox->regs + data->rx_intcr);
	enabled = exynos_mbox->rx_enabled & ~exynos_mbox->rx_paused;
	spin_unlock_irqrestore(&exynos_mbox->rx_lock, flags);
	status &= enabled;

	for_each_set_bit(chan_id, &status, data->num_chans) {
		struct mbox_chan *chan = &exynos_mbox->mbox->chans[chan_id];

		mbox_chan_received_data(chan, NULL);
	}

	return IRQ_HANDLED;
}

static const struct of_device_id exynos_mbox_match[] = {
	{
		.compatible = "google,gs101-mbox",
		.data = &exynos_gs101_mbox_data
	},
	{
		.compatible = "samsung,exynos850-mbox",
		.data = &exynos850_mbox_data
	},
	{
		.compatible = "samsung,exynos8890-mbox",
		.data = &exynos8890_mbox_data
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_mbox_match);

static int exynos_mbox_probe(struct platform_device *pdev)
{
	const struct exynos_mbox_driver_data *data;
	struct device *dev = &pdev->dev;
	struct exynos_mbox *exynos_mbox;
	struct mbox_controller *mbox;
	struct mbox_chan *chans;
	struct clk *pclk;
	int ret;

	data = device_get_match_data(&pdev->dev);
	if (!data)
		return -ENODEV;

	exynos_mbox = devm_kzalloc(dev, sizeof(*exynos_mbox), GFP_KERNEL);
	if (!exynos_mbox)
		return -ENOMEM;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	chans = devm_kcalloc(dev, data->num_chans, sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	exynos_mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(exynos_mbox->regs))
		return PTR_ERR(exynos_mbox->regs);

	/* MCU_IPC has no separate peripheral clock; ACPM requires pclk. */
	if (data->rx_intsr)
		pclk = devm_clk_get_optional_enabled(dev, "pclk");
	else
		pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk),
				     "Failed to enable clock.\n");

	exynos_mbox->data = data;
	mbox->num_chans = data->num_chans;
	mbox->chans = chans;
	mbox->dev = dev;
	if (data->rx_intsr) {
		mbox->ops = &exynos8890_mbox_chan_ops;
		mbox->of_xlate = exynos8890_mbox_of_xlate;
	} else {
		mbox->ops = &exynos_mbox_chan_ops;
		mbox->of_xlate = exynos_mbox_of_xlate;
	}

	exynos_mbox->mbox = mbox;
	spin_lock_init(&exynos_mbox->rx_lock);
	if (data->rx_intsr) {
		exynos_mbox->irq = platform_get_irq(pdev, 0);
		if (exynos_mbox->irq < 0)
			return exynos_mbox->irq;
	}

	platform_set_drvdata(pdev, exynos_mbox);

	if (!data->rx_intsr) {
		/*
		 * Mask out all interrupts. We support just polling channels
		 * for now.
		 */
		writel(data->intmr_mask, exynos_mbox->regs + data->intmr);
	}

	ret = devm_mbox_controller_register(dev, mbox);
	if (ret)
		return ret;

	if (data->rx_intsr) {
		/* Unmask every incoming doorbell; mbox_chan_received_data()
		 * routes each one to whichever client requested that channel.
		 */
		writel(0, exynos_mbox->regs + data->rx_intmr);

		ret = devm_request_irq(dev, exynos_mbox->irq, exynos8890_mbox_irq, 0,
				       dev_name(dev), exynos_mbox);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to request IRQ.\n");
	}

	return 0;
}

static struct platform_driver exynos_mbox_driver = {
	.probe	= exynos_mbox_probe,
	.driver	= {
		.name = "exynos-acpm-mbox",
		.of_match_table	= exynos_mbox_match,
	},
};
module_platform_driver(exynos_mbox_driver);

MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Samsung Exynos mailbox driver");
MODULE_LICENSE("GPL");
