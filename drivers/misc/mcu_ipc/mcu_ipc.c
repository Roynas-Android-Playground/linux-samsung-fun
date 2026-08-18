// SPDX-License-Identifier: GPL-2.0-only
/*
 * SS310 modem compatibility API over the upstream mailbox framework.
 *
 * The mailbox controller owns interrupt masking, acknowledgement and doorbell
 * generation. This adapter retains the vendor modem's shared-register API
 * while clients are converted incrementally.
 */

#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include <linux/misc/mcu_ipc.h>

#define EXYNOS8890_MBOX_CHANNELS	16
#define EXYNOS8890_MBOX_ISSR0		0x80

struct modem_mbox;

struct modem_mbox_channel {
	struct mbox_client client;
	struct mbox_chan *chan;
	struct modem_mbox *parent;
	u8 id;
};

struct modem_mbox {
	struct device *dev;
	struct regmap *regmap;
	struct modem_mbox_channel channel[EXYNOS8890_MBOX_CHANNELS];
	struct mcu_ipc_ipc_handler handler[EXYNOS8890_MBOX_CHANNELS];
	spinlock_t handler_lock;
};

static struct modem_mbox *modem_mbox_data;

static void modem_mbox_rx(struct mbox_client *client, void *message)
{
	struct modem_mbox_channel *channel =
		container_of(client, struct modem_mbox_channel, client);
	struct modem_mbox *mbox = channel->parent;
	void (*handler)(void *);
	void *data;
	unsigned long flags;

	rcu_read_lock();
	spin_lock_irqsave(&mbox->handler_lock, flags);
	handler = mbox->handler[channel->id].handler;
	data = mbox->handler[channel->id].data;
	spin_unlock_irqrestore(&mbox->handler_lock, flags);
	if (handler)
		handler(data);
	rcu_read_unlock();
}

int mbox_request_irq(u32 int_num, void (*handler)(void *), void *data)
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);
	unsigned long flags;

	if (!mbox)
		return -EPROBE_DEFER;
	if (int_num >= EXYNOS8890_MBOX_CHANNELS || !handler)
		return -EINVAL;

	spin_lock_irqsave(&mbox->handler_lock, flags);
	if (mbox->handler[int_num].handler) {
		spin_unlock_irqrestore(&mbox->handler_lock, flags);
		return -EBUSY;
	}
	mbox->handler[int_num].handler = handler;
	mbox->handler[int_num].data = data;
	spin_unlock_irqrestore(&mbox->handler_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(mbox_request_irq);

int mcu_ipc_unregister_handler(u32 int_num, void (*handler)(void *))
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);
	unsigned long flags;

	if (!mbox || int_num >= EXYNOS8890_MBOX_CHANNELS || !handler)
		return -EINVAL;

	spin_lock_irqsave(&mbox->handler_lock, flags);
	if (mbox->handler[int_num].handler != handler) {
		spin_unlock_irqrestore(&mbox->handler_lock, flags);
		return -EINVAL;
	}
	mbox->handler[int_num].handler = NULL;
	mbox->handler[int_num].data = NULL;
	spin_unlock_irqrestore(&mbox->handler_lock, flags);
	synchronize_rcu();
	return 0;
}
EXPORT_SYMBOL_GPL(mcu_ipc_unregister_handler);

void mbox_set_interrupt(u32 int_num)
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);
	int ret;

	if (!mbox || int_num >= EXYNOS8890_MBOX_CHANNELS)
		return;
	ret = mbox_send_message(mbox->channel[int_num].chan, NULL);
	if (ret < 0) {
		dev_err_ratelimited(mbox->dev,
				    "failed to ring mailbox %u: %d\n", int_num, ret);
		return;
	}
	mbox_client_txdone(mbox->channel[int_num].chan, 0);
}
EXPORT_SYMBOL_GPL(mbox_set_interrupt);

void mcu_ipc_send_command(u32 int_num, u16 cmd)
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);

	if (!mbox || int_num >= EXYNOS8890_MBOX_CHANNELS)
		return;
	regmap_write(mbox->regmap, EXYNOS8890_MBOX_ISSR0 + 8 * int_num, cmd);
	mbox_set_interrupt(int_num);
}
EXPORT_SYMBOL_GPL(mcu_ipc_send_command);

u32 mbox_get_value(u32 mbx_num)
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);
	u32 value;

	if (!mbox || mbx_num >= MAX_MBOX_NUM)
		return 0;
	if (regmap_read(mbox->regmap, EXYNOS8890_MBOX_ISSR0 + 4 * mbx_num,
			&value))
		return 0;
	return value;
}
EXPORT_SYMBOL_GPL(mbox_get_value);

void mbox_set_value(u32 mbx_num, u32 msg)
{
	struct modem_mbox *mbox = READ_ONCE(modem_mbox_data);

	if (!mbox || mbx_num >= MAX_MBOX_NUM)
		return;
	regmap_write(mbox->regmap, EXYNOS8890_MBOX_ISSR0 + 4 * mbx_num, msg);
}
EXPORT_SYMBOL_GPL(mbox_set_value);

static int modem_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modem_mbox *mbox;
	int i, ret;

	if (READ_ONCE(modem_mbox_data))
		return -EBUSY;
	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	mbox->dev = dev;
	spin_lock_init(&mbox->handler_lock);
	mbox->regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "samsung,mbox-syscon");
	if (IS_ERR(mbox->regmap))
		return dev_err_probe(dev, PTR_ERR(mbox->regmap),
				     "failed to get mailbox regmap\n");

	for (i = 0; i < EXYNOS8890_MBOX_CHANNELS; i++) {
		struct modem_mbox_channel *channel = &mbox->channel[i];

		channel->parent = mbox;
		channel->id = i;
		channel->client.dev = dev;
		channel->client.rx_callback = modem_mbox_rx;
		channel->client.knows_txdone = true;
		channel->chan = mbox_request_channel(&channel->client, i);
		if (IS_ERR(channel->chan)) {
			ret = PTR_ERR(channel->chan);
			goto free_channels;
		}
	}

	platform_set_drvdata(pdev, mbox);
	WRITE_ONCE(modem_mbox_data, mbox);
	return 0;

free_channels:
	while (--i >= 0)
		mbox_free_channel(mbox->channel[i].chan);
	return dev_err_probe(dev, ret, "failed to request mailbox channel\n");
}

static void modem_mbox_remove(struct platform_device *pdev)
{
	struct modem_mbox *mbox = platform_get_drvdata(pdev);
	int i;

	WRITE_ONCE(modem_mbox_data, NULL);
	for (i = 0; i < EXYNOS8890_MBOX_CHANNELS; i++)
		mbox_free_channel(mbox->channel[i].chan);
}

static const struct of_device_id modem_mbox_of_match[] = {
	{ .compatible = "samsung,exynos8890-modem-mailbox" },
	{ }
};
MODULE_DEVICE_TABLE(of, modem_mbox_of_match);

static struct platform_driver modem_mbox_driver = {
	.probe = modem_mbox_probe,
	.remove = modem_mbox_remove,
	.driver = {
		.name = "exynos8890-modem-mailbox",
		.of_match_table = modem_mbox_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(modem_mbox_driver);

MODULE_DESCRIPTION("Exynos8890 modem mailbox adapter");
MODULE_LICENSE("GPL");
