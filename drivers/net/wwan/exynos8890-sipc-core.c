// SPDX-License-Identifier: GPL-2.0-only
/* Exynos8890 SIPC5 shared-memory transport. */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "exynos8890-sipc-internal.h"

#define EXYNOS8890_SIPC_RX_BUDGET	64

static const struct exynos8890_sipc_channel_config exynos8890_channels[] = {
	{ 235, EXYNOS8890_SIPC_CHANNEL_FORMATTED, "umts_ipc0", 16, 32, 4096, 4096, 0, true, true },
	{ 236, EXYNOS8890_SIPC_CHANNEL_FORMATTED, "umts_ipc1", 16, 32, 4096, 4096, 0, true, true },
	{ 245, EXYNOS8890_SIPC_CHANNEL_RFS, "umts_rfs0", 16, 512, 2048, 2048, 0, true, true },
	{ 1, EXYNOS8890_SIPC_CHANNEL_RAW, "umts_csd", 32, 64, 2048, 2048, 0, true, true },
	{ 25, EXYNOS8890_SIPC_CHANNEL_RAW, "umts_router", 16, 16, 2048, 2048, 0, true, true },
	{ 28, EXYNOS8890_SIPC_CHANNEL_LOG, "umts_dm0", 16, 128, 2048, 2048, 0, true, true },
	{ 10, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet0", 0, 0, 2048, 2048, 0, false, true },
	{ 11, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet1", 0, 0, 2048, 2048, 0, false, true },
	{ 12, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet2", 0, 0, 2048, 2048, 0, false, true },
	{ 13, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet3", 0, 0, 2048, 2048, 0, false, true },
	{ 14, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet4", 0, 0, 2048, 2048, 0, false, true },
	{ 15, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet5", 0, 0, 2048, 2048, 0, false, true },
	{ 16, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet6", 0, 0, 2048, 2048, 0, false, true },
	{ 17, EXYNOS8890_SIPC_CHANNEL_RAW, "rmnet7", 0, 0, 2048, 2048, 0, false, true },
	{ 215, EXYNOS8890_SIPC_CHANNEL_BOOT, "umts_boot0", 0, 0, 0, 0, 0, true, true },
	{ 225, EXYNOS8890_SIPC_CHANNEL_DUMP, "umts_ramdump0", 0, 0, 0, 0, 0, true, true },
	{ 33, EXYNOS8890_SIPC_CHANNEL_RAW, "smd4", 16, 128, 2048, 2048, 0, true, true },
	{ 26, EXYNOS8890_SIPC_CHANNEL_RAW, "umts_ciq0", 16, 128, 2048, 2048, 0, true, true },
	/*
	 * multipdp_hiprio / multipdp: legacy driver's QOS_HIPRIO(10) and
	 * QOS_NORMAL(11) SBD rings (link_device_memory_sbd.c
	 * init_ctrl_tables()). These channel ids are CP-ABI constants, not
	 * DT-derived - unlike umts_ipc0 etc. they do NOT come from the
	 * io_device's `iod,id` (both DT nodes use iod,id=0; the legacy
	 * driver discards that and reassigns ch=10/11 via its qos_prio
	 * counter). Every rmnetN channel above is PS range (10-24) and, per
	 * sipc_ps_ch()/ch2id() in the legacy driver, owns no ring of its
	 * own: with CONFIG_MODEM_IF_QOS unset (herolte's config) ALL of
	 * rmnet0..rmnet7's traffic is funneled onto the "hiprio" ring below,
	 * multiplexed via a per-slot channel tag - see
	 * exynos8890_sipc_init_channels()'s carrier wiring and
	 * exynos8890_sbd_tx()/rx()'s ps_multiplex handling. "multipdp"
	 * (QOS_NORMAL) is real, CP-visible ring that legacy still allocates
	 * and initializes, but it is never reachable through ch2id() once
	 * MODEM_IF_QOS is unset - it is deliberately inert here, kept only
	 * for shared-memory layout/ABI fidelity with the vendor driver.
	 */
	{ 10, EXYNOS8890_SIPC_CHANNEL_RAW, "multipdp_hiprio", 256, 256, 2048, 2048, 0, false, true },
	{ 11, EXYNOS8890_SIPC_CHANNEL_RAW, "multipdp", 512, 1024, 2048, 2048, 0, false, true },
};

static int exynos8890_sipc_get_phandle(struct device_node *np,
				       const char *property)
{
	struct device_node *target;

	target = of_parse_phandle(np, property, 0);
	if (!target)
		return -EINVAL;
	of_node_put(target);
	return 0;
}

int exynos8890_sipc_parse_dt(struct exynos8890_sipc *sipc)
{
	struct device_node *np;

	if (!sipc || !sipc->dev || !sipc->dev->of_node)
		return -EINVAL;

	np = sipc->dev->of_node;
	if (exynos8890_sipc_get_phandle(np, "memory-region"))
		return dev_err_probe(sipc->dev, -EINVAL,
				     "missing memory-region phandle\n");

	return 0;
}

int exynos8890_sipc_map_shared_memory(struct exynos8890_sipc *sipc)
{
	struct exynos8890_shmem_mapping *boot, *ipc;
	int ret;

	if (!sipc || !sipc->shmem)
		return -EINVAL;

	ret = exynos8890_shmem_map_boot(sipc->shmem, &boot);
	if (ret)
		return ret;
	ret = exynos8890_shmem_map_ipc(sipc->shmem, &ipc);
	if (ret) {
		exynos8890_shmem_unmap_boot(sipc->shmem);
		return ret;
	}

	sipc->shared_base = boot->vaddr;
	sipc->shared_phys = sipc->shmem->geometry.base;
	sipc->shared_size = sipc->shmem->geometry.total_size;
	sipc->ipc_base = ipc->vaddr;
	sipc->ipc_size = ipc->size;
	sipc->global_desc = (void __iomem *)((u8 __iomem *)ipc->vaddr +
					       EXYNOS8890_SBD_DESC_OFFSET);
	return 0;
}

void exynos8890_sipc_unmap_shared_memory(struct exynos8890_sipc *sipc)
{
	if (!sipc || !sipc->shmem)
		return;

	sipc->global_desc = NULL;
	sipc->ipc_base = NULL;
	sipc->shared_base = NULL;
	sipc->ipc_size = 0;
	sipc->shared_size = 0;
	exynos8890_shmem_unmap_ipc(sipc->shmem);
	exynos8890_shmem_unmap_boot(sipc->shmem);
}

int exynos8890_sipc_validate_layout(struct exynos8890_sipc *sipc)
{
	if (!sipc || !sipc->shmem)
		return -EINVAL;
	if (exynos8890_shmem_validate(&sipc->shmem->geometry))
		return -EINVAL;
	if (!sipc->ipc_base || sipc->ipc_size != EXYNOS8890_CP_IPC_SIZE)
		return -EINVAL;
	if (EXYNOS8890_SBD_DESC_END > sipc->ipc_size ||
	    EXYNOS8890_SBD_BUFFER_OFFSET < EXYNOS8890_SBD_DESC_END)
		return -EINVAL;
	return 0;
}

int exynos8890_sipc_init_channels(struct exynos8890_sipc *sipc)
{
	struct exynos8890_sipc_channel *hiprio = NULL;
	unsigned int i;

	if (!sipc || ARRAY_SIZE(exynos8890_channels) > ARRAY_SIZE(sipc->channels))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(exynos8890_channels); i++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[i];

		channel->sipc = sipc;
		channel->config = exynos8890_channels[i];
		refcount_set(&channel->users, 1);
		mutex_init(&channel->tx_lock);
		init_waitqueue_head(&channel->tx_wait);
		/*
		 * tx.queue/rx.queue must be valid the moment this channel
		 * table exists: exynos8890_transport_close_tx() (reachable
		 * from .shutdown, unconditionally, at any time - see reboot
		 * crash in skb_queue_purge_reason()) purges every channel's
		 * tx.queue regardless of whether the CP ever brought up its
		 * SBD ring. The rest of struct exynos8890_sbd_ring (buffer
		 * pointers, slot counts, ...) genuinely can't be set up until
		 * exynos8890_sbd_init() learns the ring layout from the CP,
		 * but the plain skb list has no such dependency.
		 */
		skb_queue_head_init(&channel->tx.queue);
		skb_queue_head_init(&channel->rx.queue);
		channel->started = false;
		channel->carrier = NULL;
		channel->ps_multiplex = false;
		if (channel->config.id == 10 &&
		    (channel->config.tx_slots || channel->config.rx_slots))
			hiprio = channel;
		if (channel->config.id == 10 || channel->config.id == 11)
			channel->ps_multiplex = true;
	}
	sipc->channel_count = ARRAY_SIZE(exynos8890_channels);

	/*
	 * PS-range channels (rmnetN et al) that own no ring of their own are
	 * carried on the "multipdp_hiprio" ring - see the comment above the
	 * table for why this, and not "multipdp", is the live ring when
	 * CONFIG_MODEM_IF_QOS is unset.
	 */
	if (!hiprio)
		return 0;
	for (i = 0; i < sipc->channel_count; i++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[i];

		if (channel != hiprio &&
		    exynos8890_sipc5_is_ps_channel(channel->config.id) &&
		    !channel->config.tx_slots && !channel->config.rx_slots)
			channel->carrier = hiprio;
	}
	return 0;
}

void exynos8890_sipc_deinit_channels(struct exynos8890_sipc *sipc)
{
	unsigned int i;

	if (!sipc)
		return;
	for (i = 0; i < sipc->channel_count; i++) {
		sipc->channels[i].started = false;
		skb_queue_purge(&sipc->channels[i].tx.queue);
		skb_queue_purge(&sipc->channels[i].rx.queue);
	}
	sipc->channel_count = 0;
}

bool exynos8890_sipc5_start_valid(const void *frame)
{
	const u8 *bytes = frame;

	return bytes && (bytes[0] & EXYNOS8890_SIPC_START_MASK) ==
		EXYNOS8890_SIPC_START_VALUE;
}

bool exynos8890_sipc5_has_padding(const void *frame)
{
	const u8 *bytes = frame;

	return bytes && (bytes[0] & EXYNOS8890_SIPC_PADDING_EXIST);
}

bool exynos8890_sipc5_is_multiframe(const void *frame)
{
	const u8 *bytes = frame;

	return bytes && (bytes[0] & EXYNOS8890_SIPC_EXT_FIELD_MASK) ==
		EXYNOS8890_SIPC_MULTI_FRAME_CFG;
}

bool exynos8890_sipc5_has_extended_length(const void *frame)
{
	const u8 *bytes = frame;

	return bytes && (bytes[0] & EXYNOS8890_SIPC_EXT_FIELD_MASK) ==
		EXYNOS8890_SIPC_EXT_LENGTH_CFG;
}

size_t exynos8890_sipc5_padding(size_t frame_length, bool aligned)
{
	return aligned ? ALIGN(frame_length, sizeof(u32)) - frame_length : 0;
}

bool exynos8890_sipc5_is_boot_channel(u8 channel)
{
	return channel >= 215 && channel <= 224;
}

bool exynos8890_sipc5_is_dump_channel(u8 channel)
{
	return channel >= 225 && channel <= 234;
}

bool exynos8890_sipc5_is_fmt_channel(u8 channel)
{
	return channel >= 235 && channel <= 244;
}

bool exynos8890_sipc5_is_rfs_channel(u8 channel)
{
	return channel >= 245 && channel <= 254;
}

/* Matches legacy sipc_ps_ch(): SIPC_CH_ID_PDP_0(10) .. SIPC_CH_ID_PDP_14(24). */
bool exynos8890_sipc5_is_ps_channel(u8 channel)
{
	return channel >= 10 && channel <= 24;
}

bool exynos8890_sipc5_is_ipc_channel(u8 channel)
{
	if (!channel || channel == 5 || channel == 6 || channel == 27 ||
	    channel == 255)
		return false;
	return !exynos8890_sipc5_is_boot_channel(channel) &&
	       !exynos8890_sipc5_is_dump_channel(channel);
}

int exynos8890_sipc_parse_frame(struct exynos8890_sipc *sipc,
			       const void *data, size_t available,
			       size_t *frame_length, size_t *header_length,
			       u8 *channel)
{
	const u8 *bytes = data;
	size_t header, length, padding;

	if (!sipc || !data || !frame_length || !header_length || !channel)
		return -EINVAL;
	if (available < EXYNOS8890_SIPC_MIN_HEADER)
		return -EMSGSIZE;
	if (!exynos8890_sipc5_start_valid(data))
		return -EPROTO;

	if (exynos8890_sipc5_is_multiframe(data))
		header = 5;
	else if (exynos8890_sipc5_has_extended_length(data))
		header = 6;
	else
		header = 4;
	if (available < header)
		return -EMSGSIZE;

	length = header == 6 ? get_unaligned_le32(bytes + 2) :
			       get_unaligned_le16(bytes + 2);
	if (length < header)
		return -EPROTO;
	padding = exynos8890_sipc5_padding(length,
					   exynos8890_sipc5_has_padding(data));
	if (length > available || padding > available - length)
		return -EMSGSIZE;
	if (!exynos8890_sipc5_is_ipc_channel(bytes[1]) &&
	    !exynos8890_sipc5_is_boot_channel(bytes[1]) &&
	    !exynos8890_sipc5_is_dump_channel(bytes[1]))
		return -EINVAL;

	*frame_length = length + padding;
	*header_length = header;
	*channel = bytes[1];
	return 0;
}

struct sk_buff *exynos8890_sipc_build_frame(struct exynos8890_sipc_channel *channel,
					   const void *payload,
					   size_t payload_length,
					   gfp_t gfp)
{
	struct sk_buff *skb;
	size_t header, frame_length, padding;
	u8 config = EXYNOS8890_SIPC_START_VALUE;

	if (!channel || (!payload && payload_length))
		return ERR_PTR(-EINVAL);
	if (payload_length > U32_MAX - EXYNOS8890_SIPC_MAX_HEADER)
		return ERR_PTR(-EMSGSIZE);

	header = payload_length + 4 > U16_MAX ? 6 : 4;
	frame_length = header + payload_length;
	if (header == 6)
		config |= EXYNOS8890_SIPC_EXT_LENGTH_CFG;
	if (channel->config.interrupt_driven)
		config |= EXYNOS8890_SIPC_PADDING_EXIST;
	padding = exynos8890_sipc5_padding(frame_length,
					   config & EXYNOS8890_SIPC_PADDING_EXIST);

	skb = alloc_skb(frame_length + padding, gfp);
	if (!skb)
		return ERR_PTR(-ENOMEM);
	*(u8 *)skb_put(skb, 1) = config;
	*(u8 *)skb_put(skb, 1) = channel->config.id;
	if (header == 6) {
		put_unaligned_le32(frame_length, skb_put(skb, sizeof(u32)));
	} else {
		put_unaligned_le16(frame_length, skb_put(skb, sizeof(u16)));
	}
	if (payload_length)
		memcpy(skb_put(skb, payload_length), payload, payload_length);
	if (padding)
		memset(skb_put(skb, padding), 0, padding);
	return skb;
}

void exynos8890_sipc_notify_clients(struct exynos8890_sipc *sipc,
				   enum exynos8890_sipc_link_state state)
{
	struct exynos8890_sipc_client *client;
	int idx;

	if (!sipc)
		return;
	idx = srcu_read_lock(&sipc->client_srcu);
	list_for_each_entry_rcu(client, &sipc->clients, node,
				srcu_read_lock_held(&sipc->client_srcu)) {
		if (client->ops->link_state_changed)
			client->ops->link_state_changed(client->data, state);
	}
	srcu_read_unlock(&sipc->client_srcu, idx);
}

void exynos8890_sipc_report_fault(struct exynos8890_sipc *sipc, int error)
{
	unsigned long flags;

	if (!sipc)
		return;
	spin_lock_irqsave(&sipc->stats_lock, flags);
	sipc->stats.ring_errors++;
	spin_unlock_irqrestore(&sipc->stats_lock, flags);
	sipc->fault = error ?: -EIO;
	schedule_work(&sipc->fault_work);
}

int exynos8890_shmem_validate(const struct exynos8890_shmem_geometry *geometry)
{
	if (!geometry || geometry->base != EXYNOS8890_CP_SHMEM_BASE ||
	    geometry->total_size != EXYNOS8890_CP_SHMEM_SIZE ||
	    geometry->boot_offset != 0 ||
	    geometry->boot_size != EXYNOS8890_CP_IPC_OFFSET ||
	    geometry->ipc_offset != EXYNOS8890_CP_IPC_OFFSET ||
	    geometry->ipc_size != EXYNOS8890_CP_IPC_SIZE)
		return -EINVAL;
	if (geometry->boot_offset > geometry->total_size ||
	    geometry->boot_size > geometry->total_size - geometry->boot_offset ||
	    geometry->ipc_offset > geometry->total_size ||
	    geometry->ipc_size > geometry->total_size - geometry->ipc_offset)
		return -ERANGE;
	return 0;
}

int exynos8890_shmem_get(struct device *consumer,
			struct exynos8890_shmem **out)
{
	struct exynos8890_shmem *shmem;
	struct device_node *memory;
	struct resource resource;
	int ret;

	if (!consumer || !consumer->of_node || !out)
		return -EINVAL;
	*out = NULL;
	memory = of_parse_phandle(consumer->of_node, "memory-region", 0);
	if (!memory)
		return -EINVAL;
	ret = of_address_to_resource(memory, 0, &resource);
	of_node_put(memory);
	if (ret)
		return ret;

	shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);
	if (!shmem)
		return -ENOMEM;
	/*
	 * Deliberately not of_reserved_mem_device_init(consumer): that only
	 * wires up rmem->ops->device_init, which requires a
	 * RESERVEDMEM_OF_DECLARE() handler registered for this node's
	 * "exynos,modem_if" compatible (drivers/misc/mcu_ipc/shm_ipc.c
	 * provides one, but only under CONFIG_SHM_IPC, which herolte_defconfig
	 * does not enable) - without it, rmem->ops is NULL and the call
	 * returns -EINVAL unconditionally. This code never needed it anyway:
	 * the physical region is resolved directly via
	 * of_address_to_resource() above and mapped by hand in
	 * exynos8890_shmem_map() below, bypassing the reserved-mem device/DMA
	 * API entirely.
	 */
	shmem->dev = get_device(consumer);
	shmem->geometry.base = resource.start;
	shmem->geometry.total_size = resource_size(&resource);
	shmem->geometry.boot_offset = 0;
	shmem->geometry.boot_size = EXYNOS8890_CP_IPC_OFFSET;
	shmem->geometry.ipc_offset = EXYNOS8890_CP_IPC_OFFSET;
	shmem->geometry.ipc_size = EXYNOS8890_CP_IPC_SIZE;
	mutex_init(&shmem->map_lock);

	ret = exynos8890_shmem_validate(&shmem->geometry);
	if (ret)
		goto err_put;
	*out = shmem;
	return 0;

err_put:
	put_device(shmem->dev);
	kfree(shmem);
	return ret;
}

void exynos8890_shmem_put(struct exynos8890_shmem *shmem)
{
	if (!shmem)
		return;
	mutex_lock(&shmem->map_lock);
	if (shmem->ipc.vaddr)
		iounmap(shmem->ipc.vaddr);
	if (shmem->boot.vaddr)
		iounmap(shmem->boot.vaddr);
	memset(&shmem->ipc, 0, sizeof(shmem->ipc));
	memset(&shmem->boot, 0, sizeof(shmem->boot));
	shmem->ipc_users = 0;
	shmem->boot_users = 0;
	mutex_unlock(&shmem->map_lock);
	put_device(shmem->dev);
	kfree(shmem);
}

static int exynos8890_shmem_map(struct exynos8890_shmem *shmem,
			       struct exynos8890_shmem_mapping *mapping,
			       size_t offset, size_t size, unsigned int *users,
			       struct exynos8890_shmem_mapping **out)
{
	if (!shmem || !out)
		return -EINVAL;
	if (offset > shmem->geometry.total_size ||
	    size > shmem->geometry.total_size - offset)
		return -ERANGE;

	mutex_lock(&shmem->map_lock);
	if (!mapping->vaddr) {
		mapping->paddr = shmem->geometry.base + offset;
		mapping->size = size;
		mapping->vaddr = ioremap_wc(mapping->paddr, mapping->size);
		if (!mapping->vaddr) {
			memset(mapping, 0, sizeof(*mapping));
			mutex_unlock(&shmem->map_lock);
			return -ENOMEM;
		}
	}
	(*users)++;
	*out = mapping;
	mutex_unlock(&shmem->map_lock);
	return 0;
}

int exynos8890_shmem_map_boot(struct exynos8890_shmem *shmem,
			     struct exynos8890_shmem_mapping **out)
{
	return exynos8890_shmem_map(shmem, &shmem->boot,
				    shmem->geometry.boot_offset,
				    shmem->geometry.boot_size,
				    &shmem->boot_users, out);
}

int exynos8890_shmem_map_ipc(struct exynos8890_shmem *shmem,
			    struct exynos8890_shmem_mapping **out)
{
	return exynos8890_shmem_map(shmem, &shmem->ipc,
				    shmem->geometry.ipc_offset,
				    shmem->geometry.ipc_size,
				    &shmem->ipc_users, out);
}

static void exynos8890_shmem_unmap(struct exynos8890_shmem *shmem,
				  struct exynos8890_shmem_mapping *mapping,
				  unsigned int *users)
{
	if (!shmem)
		return;
	mutex_lock(&shmem->map_lock);
	if (*users && !--(*users)) {
		iounmap(mapping->vaddr);
		memset(mapping, 0, sizeof(*mapping));
	}
	mutex_unlock(&shmem->map_lock);
}

void exynos8890_shmem_unmap_boot(struct exynos8890_shmem *shmem)
{
	if (shmem)
		exynos8890_shmem_unmap(shmem, &shmem->boot, &shmem->boot_users);
}

void exynos8890_shmem_unmap_ipc(struct exynos8890_shmem *shmem)
{
	if (shmem)
		exynos8890_shmem_unmap(shmem, &shmem->ipc, &shmem->ipc_users);
}

int exynos8890_shmem_copy_image(struct exynos8890_shmem *shmem,
			       bool dump_region, u32 offset,
			       const void __user *source, u32 length)
{
	struct exynos8890_shmem_mapping *mapping;
	void *buffer;
	int ret;

	if (!source && length)
		return -EINVAL;
	ret = dump_region ? exynos8890_shmem_map_ipc(shmem, &mapping) :
			    exynos8890_shmem_map_boot(shmem, &mapping);
	if (ret)
		return ret;
	if (offset > mapping->size || length > mapping->size - offset) {
		ret = -EFBIG;
		goto out_unmap;
	}
	buffer = memdup_user(source, length);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		goto out_unmap;
	}
	memcpy_toio((u8 __iomem *)mapping->vaddr + offset, buffer, length);
	wmb();
	kfree(buffer);
	ret = 0;
out_unmap:
	if (dump_region)
		exynos8890_shmem_unmap_ipc(shmem);
	else
		exynos8890_shmem_unmap_boot(shmem);
	return ret;
}

int exynos8890_transport_open_channel(struct exynos8890_sipc *sipc,
				     struct exynos8890_endpoint *endpoint)
{
	struct exynos8890_sipc_channel *channel;

	if (!sipc || !endpoint)
		return -EINVAL;
	channel = exynos8890_sipc_channel_get(sipc, endpoint->config.channel);
	if (IS_ERR(channel))
		return PTR_ERR(channel);
	mutex_lock(&channel->tx_lock);
	channel->started = true;
	mutex_unlock(&channel->tx_lock);
	exynos8890_sipc_channel_put(channel);
	return 0;
}

void exynos8890_transport_close_channel(struct exynos8890_sipc *sipc,
				       struct exynos8890_endpoint *endpoint)
{
	struct exynos8890_sipc_channel *channel;

	if (!sipc || !endpoint)
		return;
	channel = exynos8890_sipc_channel_get(sipc, endpoint->config.channel);
	if (IS_ERR(channel))
		return;
	mutex_lock(&channel->tx_lock);
	channel->started = false;
	skb_queue_purge(&channel->tx.queue);
	mutex_unlock(&channel->tx_lock);
	exynos8890_sipc_channel_put(channel);
}

/* Ring + tag to actually use for TX/RX of @channel's traffic. */
static struct exynos8890_sbd_ring *
exynos8890_sipc_tx_ring(struct exynos8890_sipc_channel *channel)
{
	return channel->carrier ? &channel->carrier->tx : &channel->tx;
}

static u8 exynos8890_sipc_tx_tag(struct exynos8890_sipc_channel *channel)
{
	return channel->carrier || channel->ps_multiplex ?
		channel->config.id : 0;
}

int exynos8890_transport_xmit(struct exynos8890_sipc *sipc,
			     struct exynos8890_endpoint *endpoint,
			     struct sk_buff *skb)
{
	struct exynos8890_sipc_channel *channel;
	int ret;

	if (!sipc || !endpoint || !skb)
		return -EINVAL;
	if (exynos8890_sipc_link_state(sipc) != EXYNOS8890_SIPC_LINK_IPC)
		return -ENOTCONN;
	channel = exynos8890_sipc_channel_get(sipc, endpoint->config.channel);
	if (IS_ERR(channel))
		return PTR_ERR(channel);
	if (!READ_ONCE(channel->started)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = exynos8890_sbd_tx(exynos8890_sipc_tx_ring(channel), skb,
				exynos8890_sipc_tx_tag(channel));
out:
	exynos8890_sipc_channel_put(channel);
	return ret;
}

int exynos8890_transport_prepare_boot(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	exynos8890_sbd_reset_all(sipc);
	ret = exynos8890_cpctl_mbox_clear_all(sipc->cpctl);
	if (ret)
		return ret;
	return exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_BOOT);
}

int exynos8890_transport_prepare_download(struct exynos8890_sipc *sipc)
{
	return exynos8890_transport_prepare_boot(sipc);
}

int exynos8890_transport_prepare_dump(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	ret = exynos8890_cpctl_start_dump(sipc->cpctl);
	if (ret)
		return ret;
	return exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_DUMP);
}

int exynos8890_transport_force_crash(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	ret = exynos8890_cpctl_force_crash(sipc->cpctl);
	if (!ret)
		exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_FAULTED);
	return ret;
}

void exynos8890_transport_close_tx(struct exynos8890_sipc *sipc)
{
	unsigned int i;

	if (!sipc)
		return;
	for (i = 0; i < sipc->channel_count; i++) {
		WRITE_ONCE(sipc->channels[i].started, false);
		skb_queue_purge(&sipc->channels[i].tx.queue);
		wake_up_all(&sipc->channels[i].tx_wait);
	}
}

int exynos8890_transport_handle_init_start(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	exynos8890_sbd_deinit(sipc);
	ret = exynos8890_sbd_init(sipc);
	if (ret)
		return ret;
	ret = exynos8890_sbd_validate_global(sipc);
	if (ret) {
		exynos8890_sbd_deinit(sipc);
		return ret;
	}
	return exynos8890_sipc_send_command(sipc,
		EXYNOS8890_SIPC_CMD_VALID | EXYNOS8890_SIPC_CMD_INIT_END);
}

int exynos8890_transport_handle_phone_start(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	ret = exynos8890_cpctl_complete_boot(sipc->cpctl);
	if (ret)
		return ret;
	return exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_IPC);
}

void exynos8890_transport_handle_crash_reset(struct exynos8890_sipc *sipc)
{
	exynos8890_transport_close_tx(sipc);
	exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_FAULTED);
}

void exynos8890_transport_handle_crash_exit(struct exynos8890_sipc *sipc)
{
	exynos8890_transport_close_tx(sipc);
	exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_DUMP);
}

void exynos8890_transport_handle_command(struct exynos8890_sipc *sipc,
					u16 command)
{
	if (!sipc || !(command & EXYNOS8890_SIPC_INT_VALID) ||
	    !(command & EXYNOS8890_SIPC_CMD_VALID))
		return;

	switch (command & EXYNOS8890_SIPC_CMD_MASK) {
	case EXYNOS8890_SIPC_CMD_INIT_START:
		if (exynos8890_transport_handle_init_start(sipc))
			exynos8890_sipc_report_fault(sipc, -EPROTO);
		break;
	case EXYNOS8890_SIPC_CMD_PHONE_START:
		if (exynos8890_transport_handle_phone_start(sipc))
			exynos8890_sipc_report_fault(sipc, -EPROTO);
		break;
	case EXYNOS8890_SIPC_CMD_CRASH_RESET:
		exynos8890_transport_handle_crash_reset(sipc);
		break;
	case EXYNOS8890_SIPC_CMD_CRASH_EXIT:
		exynos8890_transport_handle_crash_exit(sipc);
		break;
	default:
		dev_warn_ratelimited(sipc->dev, "unknown CP command %#x\n", command);
	}
}

static const struct exynos8890_transport_ops exynos8890_transport_ops = {
	.start = exynos8890_sipc_start,
	.stop = exynos8890_sipc_stop,
	.open_channel = exynos8890_transport_open_channel,
	.close_channel = exynos8890_transport_close_channel,
	.xmit = exynos8890_transport_xmit,
	.prepare_boot = exynos8890_transport_prepare_boot,
	.prepare_download = exynos8890_transport_prepare_download,
	.prepare_dump = exynos8890_transport_prepare_dump,
	.force_crash = exynos8890_transport_force_crash,
	.close_tx = exynos8890_transport_close_tx,
};

static void exynos8890_sipc_release(struct kref *refcount)
{
	struct exynos8890_sipc *sipc = container_of(refcount,
						     struct exynos8890_sipc,
						     refcount);

	cleanup_srcu_struct(&sipc->client_srcu);
	put_device(sipc->dev);
	kfree(sipc);
}

static void exynos8890_sipc_rx_work(struct work_struct *work)
{
	struct exynos8890_sipc *sipc = container_of(work,
						     struct exynos8890_sipc,
						     rx_work);
	u32 message;

	if (READ_ONCE(sipc->stopping))
		return;
	if (!exynos8890_cpctl_mbox_read(sipc->cpctl, EXYNOS8890_MBX_CP2AP_MSG,
					&message)) {
		if (message & EXYNOS8890_SIPC_CMD_VALID)
			exynos8890_transport_handle_command(sipc, (u16)message);
		else
			exynos8890_sipc_poll_rx(sipc, EXYNOS8890_SIPC_RX_BUDGET);
	}
}

static void exynos8890_sipc_fault_work(struct work_struct *work)
{
	struct exynos8890_sipc *sipc = container_of(work,
						     struct exynos8890_sipc,
						     fault_work);
	struct exynos8890_sipc_client *client;
	int idx;

	exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_FAULTED);
	idx = srcu_read_lock(&sipc->client_srcu);
	list_for_each_entry_rcu(client, &sipc->clients, node,
				srcu_read_lock_held(&sipc->client_srcu)) {
		if (client->ops->transport_fault)
			client->ops->transport_fault(client->data, sipc->fault);
	}
	srcu_read_unlock(&sipc->client_srcu, idx);
}

static int exynos8890_sipc_cp_event(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct exynos8890_sipc *sipc = container_of(nb,
						     struct exynos8890_sipc,
						     cp_notifier);

	if (READ_ONCE(sipc->stopping))
		return NOTIFY_DONE;
	switch (event) {
	case EXYNOS8890_CP_EVENT_MAILBOX:
		if (((struct exynos8890_cp_event_data *)data)->raw_status ==
		    EXYNOS8890_DB_CP2AP_MSG) {
			unsigned long flags;

			spin_lock_irqsave(&sipc->stats_lock, flags);
			sipc->stats.mailbox_irqs++;
			spin_unlock_irqrestore(&sipc->stats_lock, flags);
			schedule_work(&sipc->rx_work);
		}
		break;
	case EXYNOS8890_CP_EVENT_INIT_START:
		schedule_work(&sipc->rx_work);
		break;
	case EXYNOS8890_CP_EVENT_PHONE_START:
		exynos8890_transport_handle_phone_start(sipc);
		break;
	case EXYNOS8890_CP_EVENT_FAIL:
	case EXYNOS8890_CP_EVENT_WATCHDOG:
		exynos8890_sipc_report_fault(sipc,
			((struct exynos8890_cp_event_data *)data)->error ?: -EIO);
		break;
	case EXYNOS8890_CP_EVENT_SHUTDOWN:
		exynos8890_sipc_set_link_state(sipc, EXYNOS8890_SIPC_LINK_OFFLINE);
		break;
	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}

struct exynos8890_sipc *exynos8890_sipc_get(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct exynos8890_sipc *sipc;

	if (!consumer || !consumer->of_node)
		return ERR_PTR(-EINVAL);
	np = of_parse_phandle(consumer->of_node, "samsung,sipc", 0);
	if (!np && of_device_is_compatible(consumer->of_node,
					   "samsung,exynos8890-sipc5"))
		np = of_node_get(consumer->of_node);
	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	sipc = platform_get_drvdata(pdev);
	if (!sipc || !kref_get_unless_zero(&sipc->refcount))
		sipc = ERR_PTR(-EPROBE_DEFER);
	else if (READ_ONCE(sipc->stopping)) {
		exynos8890_sipc_put(sipc);
		sipc = ERR_PTR(-ESHUTDOWN);
	}
	put_device(&pdev->dev);
	return sipc;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_get);

void exynos8890_sipc_put(struct exynos8890_sipc *sipc)
{
	if (sipc)
		kref_put(&sipc->refcount, exynos8890_sipc_release);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_put);

struct device *exynos8890_sipc_device(struct exynos8890_sipc *sipc)
{
	return sipc ? sipc->dev : NULL;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_device);

int exynos8890_sipc_register_client(struct exynos8890_sipc *sipc,
				   const struct exynos8890_sipc_client_ops *ops,
				   void *data)
{
	struct exynos8890_sipc_client *client, *iter;

	if (!sipc || !ops)
		return -EINVAL;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	client->ops = ops;
	client->data = data;

	mutex_lock(&sipc->client_lock);
	if (sipc->stopping) {
		mutex_unlock(&sipc->client_lock);
		kfree(client);
		return -ESHUTDOWN;
	}
	list_for_each_entry(iter, &sipc->clients, node) {
		if (iter->ops == ops && iter->data == data) {
			mutex_unlock(&sipc->client_lock);
			kfree(client);
			return -EEXIST;
		}
	}
	list_add_tail_rcu(&client->node, &sipc->clients);
	mutex_unlock(&sipc->client_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_register_client);

void exynos8890_sipc_unregister_client(struct exynos8890_sipc *sipc,
				      const struct exynos8890_sipc_client_ops *ops,
				      void *data)
{
	struct exynos8890_sipc_client *client;

	if (!sipc || !ops)
		return;
	mutex_lock(&sipc->client_lock);
	list_for_each_entry(client, &sipc->clients, node) {
		if (client->ops == ops && client->data == data) {
			list_del_rcu(&client->node);
			mutex_unlock(&sipc->client_lock);
			synchronize_srcu(&sipc->client_srcu);
			kfree(client);
			return;
		}
	}
	mutex_unlock(&sipc->client_lock);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_unregister_client);

enum exynos8890_sipc_link_state
exynos8890_sipc_link_state(struct exynos8890_sipc *sipc)
{
	enum exynos8890_sipc_link_state state;

	if (!sipc)
		return EXYNOS8890_SIPC_LINK_FAULTED;
	mutex_lock(&sipc->state_lock);
	state = sipc->state;
	mutex_unlock(&sipc->state_lock);
	return state;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_link_state);

int exynos8890_sipc_set_link_state(struct exynos8890_sipc *sipc,
				  enum exynos8890_sipc_link_state state)
{
	enum exynos8890_sipc_link_state old;

	if (!sipc || state > EXYNOS8890_SIPC_LINK_FAULTED)
		return -EINVAL;
	mutex_lock(&sipc->state_lock);
	if (sipc->stopping && state != EXYNOS8890_SIPC_LINK_OFFLINE) {
		mutex_unlock(&sipc->state_lock);
		return -ESHUTDOWN;
	}
	old = sipc->state;
	sipc->state = state;
	mutex_unlock(&sipc->state_lock);
	if (old != state)
		exynos8890_sipc_notify_clients(sipc, state);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_set_link_state);

int exynos8890_sipc_reset(struct exynos8890_sipc *sipc)
{
	int ret;

	if (!sipc)
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->cpctl)
		return -ESHUTDOWN;
	exynos8890_transport_close_tx(sipc);
	exynos8890_sbd_reset_all(sipc);
	ret = exynos8890_cpctl_reset(sipc->cpctl);
	if (!ret)
		ret = exynos8890_sipc_set_link_state(sipc,
						 EXYNOS8890_SIPC_LINK_OFFLINE);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_reset);

int exynos8890_sipc_start(struct exynos8890_sipc *sipc)
{
	int ret = 0;

	if (!sipc)
		return -EINVAL;
	mutex_lock(&sipc->state_lock);
	if (sipc->stopping)
		ret = -ESHUTDOWN;
	else if (!sipc->transport_started)
		sipc->transport_started = true;
	mutex_unlock(&sipc->state_lock);
	if (ret)
		return ret;
	ret = exynos8890_cpctl_set_protocol_suspended(sipc->cpctl, false);
	if (ret) {
		mutex_lock(&sipc->state_lock);
		sipc->transport_started = false;
		mutex_unlock(&sipc->state_lock);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_start);

void exynos8890_sipc_stop(struct exynos8890_sipc *sipc)
{
	if (!sipc)
		return;
	exynos8890_transport_close_tx(sipc);
	exynos8890_sbd_deinit(sipc);
	mutex_lock(&sipc->state_lock);
	sipc->transport_started = false;
	sipc->state = EXYNOS8890_SIPC_LINK_OFFLINE;
	mutex_unlock(&sipc->state_lock);
	exynos8890_cpctl_set_protocol_suspended(sipc->cpctl, true);
	exynos8890_sipc_notify_clients(sipc, EXYNOS8890_SIPC_LINK_OFFLINE);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_stop);

struct exynos8890_sipc_channel *
exynos8890_sipc_channel_get(struct exynos8890_sipc *sipc, u8 channel_id)
{
	unsigned int i;

	if (!sipc)
		return ERR_PTR(-EINVAL);
	if (READ_ONCE(sipc->stopping))
		return ERR_PTR(-ESHUTDOWN);
	for (i = 0; i < sipc->channel_count; i++) {
		if (sipc->channels[i].config.id == channel_id &&
		    refcount_inc_not_zero(&sipc->channels[i].users))
			return &sipc->channels[i];
	}
	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_channel_get);

void exynos8890_sipc_channel_put(struct exynos8890_sipc_channel *channel)
{
	if (channel)
		refcount_dec(&channel->users);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_channel_put);

const struct exynos8890_sipc_channel_config *
exynos8890_sipc_channel_config(struct exynos8890_sipc_channel *channel)
{
	return channel ? &channel->config : NULL;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_channel_config);

int exynos8890_sipc_send(struct exynos8890_sipc_channel *channel,
			 struct sk_buff *skb, bool nonblock)
{
	struct exynos8890_sipc *sipc;
	struct exynos8890_sbd_ring *ring;
	int ret;

	if (!channel || !skb)
		return -EINVAL;
	sipc = channel->sipc;
	if (exynos8890_sipc_link_state(sipc) != EXYNOS8890_SIPC_LINK_IPC)
		return -ENOTCONN;
	if (!READ_ONCE(channel->started))
		return -ESHUTDOWN;
	ring = exynos8890_sipc_tx_ring(channel);

	if (!nonblock) {
		ret = wait_event_interruptible(channel->tx_wait,
			!exynos8890_sbd_ring_full(ring) ||
			exynos8890_sipc_link_state(sipc) !=
			EXYNOS8890_SIPC_LINK_IPC);
		if (ret)
			return ret;
		if (exynos8890_sipc_link_state(sipc) != EXYNOS8890_SIPC_LINK_IPC)
			return -ENOTCONN;
	} else if (exynos8890_sbd_ring_full(ring)) {
		return -EAGAIN;
	}
	return exynos8890_sbd_tx(ring, skb, exynos8890_sipc_tx_tag(channel));
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_send);

int exynos8890_sipc_send_command(struct exynos8890_sipc *sipc, u16 command)
{
	if (!sipc || command & ~(EXYNOS8890_SIPC_INT_VALID |
					 EXYNOS8890_SIPC_CMD_VALID |
					 EXYNOS8890_SIPC_CMD_MASK))
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->cpctl)
		return -ESHUTDOWN;
	command |= EXYNOS8890_SIPC_INT_VALID;
	return exynos8890_cpctl_write_and_ring(sipc->cpctl,
			EXYNOS8890_MBX_AP2CP_MSG, command,
			EXYNOS8890_DB_AP2CP_MSG);
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_send_command);

static bool exynos8890_sipc_deliver_client(struct exynos8890_sipc *sipc,
					  u8 channel, struct sk_buff *skb)
{
	struct exynos8890_sipc_client *client;
	bool delivered = false;
	int idx;

	idx = srcu_read_lock(&sipc->client_srcu);
	list_for_each_entry_rcu(client, &sipc->clients, node,
				srcu_read_lock_held(&sipc->client_srcu)) {
		if (!client->ops->rx_frame)
			continue;
		client->ops->rx_frame(client->data, channel, skb);
		delivered = true;
		break;
	}
	srcu_read_unlock(&sipc->client_srcu, idx);
	return delivered;
}

int exynos8890_sipc_poll_rx(struct exynos8890_sipc *sipc, int budget)
{
	unsigned long flags;
	int received = 0;
	unsigned int i;

	if (!sipc || budget < 0)
		return -EINVAL;
	for (i = 0; i < sipc->channel_count && received < budget; i++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[i];
		struct exynos8890_endpoint *endpoint;
		struct sk_buff *skb;

		while (received < budget) {
			size_t packet_length;
			bool delivered;
			u8 tag = channel->config.id;

			skb = exynos8890_sbd_rx(&channel->rx, GFP_KERNEL, &tag);
			if (IS_ERR(skb)) {
				if (PTR_ERR(skb) != -EAGAIN)
					exynos8890_sipc_report_fault(sipc, PTR_ERR(skb));
				break;
			}
			if (!skb)
				break;
			packet_length = skb->len;
			endpoint = exynos8890_endpoint_by_channel(sipc, tag);
			if (endpoint)
				delivered = !exynos8890_endpoint_rx(endpoint, skb);
			else
				delivered = exynos8890_sipc_deliver_client(sipc,
								tag, skb);
			if (!delivered) {
				dev_kfree_skb_any(skb);
				spin_lock_irqsave(&sipc->stats_lock, flags);
				sipc->stats.rx_dropped++;
				spin_unlock_irqrestore(&sipc->stats_lock, flags);
			} else {
				spin_lock_irqsave(&sipc->stats_lock, flags);
				sipc->stats.rx_packets++;
				sipc->stats.rx_bytes += packet_length;
				spin_unlock_irqrestore(&sipc->stats_lock, flags);
			}
			received++;
		}
	}
	return received;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_poll_rx);

int exynos8890_sipc_get_stats(struct exynos8890_sipc *sipc,
			     struct exynos8890_sipc_stats *stats)
{
	unsigned long flags;

	if (!sipc || !stats)
		return -EINVAL;
	spin_lock_irqsave(&sipc->stats_lock, flags);
	*stats = sipc->stats;
	spin_unlock_irqrestore(&sipc->stats_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_get_stats);

ssize_t exynos8890_sipc_boot_write(struct exynos8890_sipc *sipc,
				  const void *buffer, size_t length,
				  loff_t offset)
{
	if (!sipc || (!buffer && length) || offset < 0)
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->shmem ||
	    !sipc->shmem->boot.vaddr)
		return -ESHUTDOWN;
	if ((u64)offset > sipc->shmem->boot.size ||
	    length > sipc->shmem->boot.size - (size_t)offset)
		return -EFBIG;
	memcpy_toio((u8 __iomem *)sipc->shmem->boot.vaddr + offset, buffer, length);
	wmb();
	return length;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_boot_write);

ssize_t exynos8890_sipc_dump_read(struct exynos8890_sipc *sipc,
				 void *buffer, size_t length, loff_t offset)
{
	size_t boot_size, first;
	u8 *destination = buffer;

	if (!sipc || (!buffer && length) || offset < 0)
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->shmem ||
	    !sipc->shmem->boot.vaddr || !sipc->shmem->ipc.vaddr)
		return -ESHUTDOWN;
	if ((u64)offset > sipc->shared_size ||
	    length > sipc->shared_size - (size_t)offset)
		return -EFBIG;

	boot_size = sipc->shmem->geometry.boot_size;
	rmb();
	if ((size_t)offset < boot_size) {
		first = min(length, boot_size - (size_t)offset);
		memcpy_fromio(destination,
			      (u8 __iomem *)sipc->shmem->boot.vaddr + offset,
			      first);
		destination += first;
		length -= first;
		offset = boot_size;
	}
	if (length)
		memcpy_fromio(destination,
			      (u8 __iomem *)sipc->shmem->ipc.vaddr +
			      ((size_t)offset - boot_size), length);
	return destination - (u8 *)buffer + length;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_dump_read);

int exynos8890_sipc_get_srinfo(struct exynos8890_sipc *sipc,
			      void *buffer, size_t length)
{
	if (!sipc || (!buffer && length) || length > EXYNOS8890_SIPC_SRINFO_SIZE ||
	    EXYNOS8890_SIPC_SRINFO_OFFSET > sipc->ipc_size ||
	    length > sipc->ipc_size - EXYNOS8890_SIPC_SRINFO_OFFSET)
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->ipc_base)
		return -ESHUTDOWN;
	rmb();
	memcpy_fromio(buffer, (u8 __iomem *)sipc->ipc_base +
			      EXYNOS8890_SIPC_SRINFO_OFFSET, length);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_get_srinfo);

int exynos8890_sipc_set_srinfo(struct exynos8890_sipc *sipc,
			      const void *buffer, size_t length)
{
	if (!sipc || (!buffer && length) || length > EXYNOS8890_SIPC_SRINFO_SIZE ||
	    EXYNOS8890_SIPC_SRINFO_OFFSET > sipc->ipc_size ||
	    length > sipc->ipc_size - EXYNOS8890_SIPC_SRINFO_OFFSET)
		return -EINVAL;
	if (READ_ONCE(sipc->stopping) || !sipc->ipc_base)
		return -ESHUTDOWN;
	memcpy_toio((u8 __iomem *)sipc->ipc_base +
		    EXYNOS8890_SIPC_SRINFO_OFFSET, buffer, length);
	wmb();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_sipc_set_srinfo);

static void exynos8890_sipc_free_clients(struct exynos8890_sipc *sipc)
{
	struct exynos8890_sipc_client *client, *tmp;
	LIST_HEAD(dead);

	mutex_lock(&sipc->client_lock);
	list_splice_init(&sipc->clients, &dead);
	mutex_unlock(&sipc->client_lock);
	synchronize_srcu(&sipc->client_srcu);
	list_for_each_entry_safe(client, tmp, &dead, node)
		kfree(client);
}

static int exynos8890_sipc_probe(struct platform_device *pdev)
{
	struct exynos8890_sipc *sipc;
	int ret;

	sipc = kzalloc(sizeof(*sipc), GFP_KERNEL);
	if (!sipc)
		return -ENOMEM;
	sipc->dev = get_device(&pdev->dev);
	kref_init(&sipc->refcount);
	mutex_init(&sipc->state_lock);
	mutex_init(&sipc->client_lock);
	spin_lock_init(&sipc->stats_lock);
	INIT_LIST_HEAD(&sipc->clients);
	INIT_WORK(&sipc->rx_work, exynos8890_sipc_rx_work);
	INIT_WORK(&sipc->fault_work, exynos8890_sipc_fault_work);
	sipc->state = EXYNOS8890_SIPC_LINK_OFFLINE;
	sipc->transport_ops = &exynos8890_transport_ops;
	ret = init_srcu_struct(&sipc->client_srcu);
	if (ret)
		goto err_put;
	platform_set_drvdata(pdev, sipc);

	ret = exynos8890_sipc_parse_dt(sipc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to parse device tree\n");
		goto err_srcu;
	}
	sipc->cpctl = exynos8890_cpctl_get(&pdev->dev);
	if (IS_ERR(sipc->cpctl)) {
		ret = PTR_ERR(sipc->cpctl);
		sipc->cpctl = NULL;
		dev_err_probe(&pdev->dev, ret, "failed to get cpctl\n");
		goto err_srcu;
	}
	ret = exynos8890_shmem_get(&pdev->dev, &sipc->shmem);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			     "failed to get shared memory region\n");
		goto err_cpctl;
	}
	ret = exynos8890_sipc_map_shared_memory(sipc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			     "failed to map shared memory\n");
		goto err_shmem;
	}
	ret = exynos8890_sipc_validate_layout(sipc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			     "shared memory layout mismatch (base=%pa size=%#zx ipc_base=%p ipc_size=%#zx)\n",
			     &sipc->shmem->geometry.base,
			     sipc->shmem->geometry.total_size,
			     sipc->ipc_base, sipc->ipc_size);
		goto err_unmap;
	}
	ret = exynos8890_sipc_init_channels(sipc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to init channels\n");
		goto err_unmap;
	}

	sipc->cp_notifier.notifier_call = exynos8890_sipc_cp_event;
	ret = exynos8890_cpctl_register_notifier(sipc->cpctl,
						 &sipc->cp_notifier);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			     "failed to register cpctl notifier\n");
		goto err_channels;
	}
	sipc->notifier_registered = true;
	ret = exynos8890_sipc_register_wwan(sipc);
	if (ret) {
		dev_err_probe(&pdev->dev, ret,
			     "failed to register wwan endpoints\n");
		goto err_notifier;
	}

	dev_info(&pdev->dev, "SIPC5 transport mapped %pa+%#zx (IPC %#zx)\n",
		 &sipc->shared_phys, sipc->shared_size, sipc->ipc_size);
	return 0;

err_notifier:
	exynos8890_cpctl_unregister_notifier(sipc->cpctl, &sipc->cp_notifier);
	sipc->notifier_registered = false;
err_channels:
	exynos8890_sipc_deinit_channels(sipc);
err_unmap:
	exynos8890_sipc_unmap_shared_memory(sipc);
err_shmem:
	exynos8890_shmem_put(sipc->shmem);
	sipc->shmem = NULL;
err_cpctl:
	exynos8890_cpctl_put(sipc->cpctl);
	sipc->cpctl = NULL;
err_srcu:
	platform_set_drvdata(pdev, NULL);
	cleanup_srcu_struct(&sipc->client_srcu);
err_put:
	put_device(sipc->dev);
	kfree(sipc);
	return ret;
}

static void exynos8890_sipc_remove(struct platform_device *pdev)
{
	struct exynos8890_sipc *sipc = platform_get_drvdata(pdev);

	if (!sipc)
		return;
	platform_set_drvdata(pdev, NULL);
	mutex_lock(&sipc->state_lock);
	sipc->stopping = true;
	mutex_unlock(&sipc->state_lock);
	exynos8890_sipc_unregister_wwan(sipc);
	if (sipc->notifier_registered) {
		exynos8890_cpctl_unregister_notifier(sipc->cpctl,
						   &sipc->cp_notifier);
		sipc->notifier_registered = false;
	}
	cancel_work_sync(&sipc->rx_work);
	cancel_work_sync(&sipc->fault_work);
	exynos8890_sipc_stop(sipc);
	exynos8890_sipc_free_clients(sipc);
	exynos8890_sipc_deinit_channels(sipc);
	exynos8890_sipc_unmap_shared_memory(sipc);
	exynos8890_shmem_put(sipc->shmem);
	sipc->shmem = NULL;
	exynos8890_cpctl_put(sipc->cpctl);
	sipc->cpctl = NULL;
	exynos8890_sipc_put(sipc);
}

static void exynos8890_sipc_shutdown(struct platform_device *pdev)
{
	struct exynos8890_sipc *sipc = platform_get_drvdata(pdev);

	if (!sipc)
		return;
	mutex_lock(&sipc->state_lock);
	sipc->stopping = true;
	mutex_unlock(&sipc->state_lock);
	exynos8890_transport_close_tx(sipc);
	cancel_work_sync(&sipc->rx_work);
	cancel_work_sync(&sipc->fault_work);
	exynos8890_cpctl_shutdown(sipc->cpctl);
}

static const struct of_device_id exynos8890_sipc_of_match[] = {
	{ .compatible = "samsung,exynos8890-sipc5" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_sipc_of_match);

static struct platform_driver exynos8890_sipc_driver = {
	.probe = exynos8890_sipc_probe,
	.remove = exynos8890_sipc_remove,
	.shutdown = exynos8890_sipc_shutdown,
	.driver = {
		.name = "exynos8890-sipc5",
		.of_match_table = exynos8890_sipc_of_match,
	},
};
module_platform_driver(exynos8890_sipc_driver);

MODULE_DESCRIPTION("Exynos8890 SIPC5 shared-memory transport");
MODULE_LICENSE("GPL");
