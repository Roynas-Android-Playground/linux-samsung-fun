// SPDX-License-Identifier: GPL-2.0-only
/* Exynos8890 SBD shared-memory ring implementation. */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "exynos8890-sipc-internal.h"

#define EXYNOS8890_SBD_INVALID_ID	U16_MAX

static int exynos8890_sbd_validate_offset(struct exynos8890_sipc *sipc,
					 u32 offset, size_t size,
					 size_t alignment)
{
	if (!sipc || !sipc->ipc_base || !alignment ||
	    !is_power_of_2(alignment) || !IS_ALIGNED(offset, alignment))
		return -EINVAL;
	if (offset > sipc->ipc_size || size > sipc->ipc_size - offset)
		return -ERANGE;

	return 0;
}

static int exynos8890_sbd_validate_desc_offset(struct exynos8890_sipc *sipc,
					      u32 offset, size_t size,
					      size_t alignment)
{
	int ret;

	ret = exynos8890_sbd_validate_offset(sipc, offset, size, alignment);
	if (ret)
		return ret;
	if (offset < EXYNOS8890_SBD_DESC_OFFSET ||
	    offset > EXYNOS8890_SBD_DESC_END ||
	    size > EXYNOS8890_SBD_DESC_END - offset)
		return -ERANGE;

	return 0;
}

static int exynos8890_sbd_read_pointers(struct exynos8890_sbd_ring *ring,
				       u16 *read_pointer,
				       u16 *write_pointer)
{
	u16 read, write;

	if (!ring || !read_pointer || !write_pointer || !ring->read_pointer ||
	    !ring->write_pointer || ring->slot_count < 2)
		return -EINVAL;

	/* The peer publishes slot contents before its pointer update. */
	read = readw_relaxed(ring->read_pointer);
	write = readw_relaxed(ring->write_pointer);
	dma_rmb();

	if (read >= ring->slot_count || write >= ring->slot_count)
		return -EIO;

	*read_pointer = read;
	*write_pointer = write;
	return 0;
}

static int exynos8890_sbd_advance_pointer(struct exynos8890_sbd_ring *ring,
					 __le16 __iomem *pointer,
					 u16 value)
{
	if (!ring || !pointer || value >= ring->slot_count)
		return -EINVAL;

	/* Publish all slot/vector writes before transferring ownership. */
	dma_wmb();
	writew_relaxed(value, pointer);
	return 0;
}

static void __iomem *exynos8890_sbd_slot(struct exynos8890_sbd_ring *ring,
					unsigned int index)
{
	struct exynos8890_sipc *sipc;
	u32 expected, offset;

	if (!ring || index >= ring->slot_count || !ring->offset_vector ||
	    !ring->buffer_region)
		return NULL;
	sipc = ring->sipc;
	offset = readl_relaxed(&ring->offset_vector[index]);
	expected = (u8 __iomem *)ring->buffer_region -
		   (u8 __iomem *)sipc->ipc_base + index * ring->slot_size;
	if (offset < EXYNOS8890_SBD_BUFFER_OFFSET || offset != expected ||
	    exynos8890_sbd_validate_offset(sipc, offset, ring->slot_size, 1))
		return NULL;

	return (u8 __iomem *)sipc->ipc_base + offset;
}

static int exynos8890_sbd_expected_buffer(struct exynos8890_sipc *sipc,
					 unsigned int wanted_id,
					 enum exynos8890_sipc_direction wanted_dir,
					 u32 *offset)
{
	u32 cursor = EXYNOS8890_SBD_BUFFER_OFFSET;
	unsigned int id, direction;

	for (id = 0; id < sipc->channel_count; id++) {
		for (direction = 0; direction < EXYNOS8890_SIPC_DIRECTIONS;
		     direction++) {
			u16 slots = direction == EXYNOS8890_SIPC_TX ?
				sipc->channels[id].config.tx_slots :
				sipc->channels[id].config.rx_slots;
			u16 size = direction == EXYNOS8890_SIPC_TX ?
				sipc->channels[id].config.tx_buffer_size :
				sipc->channels[id].config.rx_buffer_size;
			size_t bytes;

			if (id == wanted_id && direction == wanted_dir) {
				*offset = cursor;
				return 0;
			}
			if (check_mul_overflow((size_t)slots, (size_t)size, &bytes) ||
			    cursor > sipc->ipc_size ||
			    bytes > sipc->ipc_size - cursor)
				return -EOVERFLOW;
			cursor += bytes;
		}
	}

	return -EINVAL;
}

static unsigned int exynos8890_sbd_count(u16 slots, u16 read, u16 write)
{
	return write >= read ? write - read : slots - read + write;
}

static u16 exynos8890_sbd_next(struct exynos8890_sbd_ring *ring, u16 pointer)
{
	return pointer + 1 == ring->slot_count ? 0 : pointer + 1;
}

static int exynos8890_sbd_check_descriptor(struct exynos8890_sipc *sipc,
					  unsigned int id,
					  enum exynos8890_sipc_direction direction,
					  u32 descriptor_offset,
					  u32 vector_offset)
{
	struct exynos8890_sbd_ring_descriptor descriptor;
	struct exynos8890_sipc_channel *channel;
	u16 slots, slot_size, payload_offset;
	u32 expected_descriptor_offset;
	size_t vector_size;
	unsigned int i;
	int ret;

	if (id >= sipc->channel_count || direction >= EXYNOS8890_SIPC_DIRECTIONS)
		return -EINVAL;
	channel = &sipc->channels[id];
	expected_descriptor_offset = EXYNOS8890_SBD_DESC_OFFSET +
		offsetof(struct exynos8890_sbd_global_descriptor,
			 rings[id][direction]);
	if (descriptor_offset != expected_descriptor_offset)
		return -EINVAL;
	ret = exynos8890_sbd_validate_desc_offset(sipc, descriptor_offset,
						 sizeof(descriptor), 2);
	if (ret)
		return ret;
	memcpy_fromio(&descriptor, (u8 __iomem *)sipc->ipc_base +
		      descriptor_offset, sizeof(descriptor));

	slots = le16_to_cpu(descriptor.length);
	slot_size = le16_to_cpu(descriptor.buffer_size);
	payload_offset = le16_to_cpu(descriptor.payload_offset);
	if (le16_to_cpu(descriptor.channel) != channel->config.id ||
	    le16_to_cpu(descriptor.reserved) ||
	    le16_to_cpu(descriptor.direction) != direction ||
	    le16_to_cpu(descriptor.id) != id || slots < 2 || !slot_size ||
	    payload_offset >= slot_size)
		return -EINVAL;
	if (slots != (direction == EXYNOS8890_SIPC_TX ?
		     channel->config.tx_slots : channel->config.rx_slots) ||
	    slot_size != (direction == EXYNOS8890_SIPC_TX ?
			  channel->config.tx_buffer_size :
			  channel->config.rx_buffer_size) ||
	    payload_offset != channel->config.headroom)
		return -EINVAL;
	if (le16_to_cpu(descriptor.signaling) !=
	    channel->config.interrupt_driven ||
	    le32_to_cpu(descriptor.signal_mask) !=
	    (EXYNOS8890_SIPC_INT_VALID | EXYNOS8890_SIPC_SEND_RAW))
		return -EINVAL;

	if (check_mul_overflow((size_t)slots, sizeof(__le32), &vector_size))
		return -EOVERFLOW;
	ret = exynos8890_sbd_validate_desc_offset(sipc, vector_offset,
						 vector_size * 2, 4);
	if (ret)
		return ret;

	/* Offset-vector entries are CP-visible and must each name a whole slot. */
	for (i = 0; i < slots; i++) {
		u32 offset = readl_relaxed((__le32 __iomem *)
			((u8 __iomem *)sipc->ipc_base + vector_offset) + i);

		if (offset < EXYNOS8890_SBD_BUFFER_OFFSET ||
		    exynos8890_sbd_validate_offset(sipc, offset, slot_size, 1))
			return -ERANGE;
	}

	return 0;
}

static int exynos8890_sbd_bind_ring(struct exynos8890_sipc *sipc,
				   struct exynos8890_sipc_channel *channel,
				   struct exynos8890_sbd_ring *ring,
				   enum exynos8890_sipc_direction direction)
{
	struct exynos8890_sbd_channel_descriptor channel_descriptor;
	struct exynos8890_sbd_ring_descriptor descriptor;
	u32 buffer_offset, descriptor_offset, vector_offset, pointers_offset;
	size_t vector_size;
	unsigned int id = channel - sipc->channels;
	int ret;

	memcpy_fromio(&channel_descriptor, &sipc->global_desc->channels[id],
		      sizeof(channel_descriptor));
	if (direction == EXYNOS8890_SIPC_TX) {
		descriptor_offset = le32_to_cpu(channel_descriptor.tx_ring_offset);
		vector_offset = le32_to_cpu(channel_descriptor.tx_vector_offset);
	} else {
		descriptor_offset = le32_to_cpu(channel_descriptor.rx_ring_offset);
		vector_offset = le32_to_cpu(channel_descriptor.rx_vector_offset);
	}
	ret = exynos8890_sbd_check_descriptor(sipc, id, direction,
					      descriptor_offset, vector_offset);
	if (ret)
		return ret;
	memcpy_fromio(&descriptor, (u8 __iomem *)sipc->ipc_base +
		      descriptor_offset, sizeof(descriptor));

	ring->sipc = sipc;
	ring->channel = channel;
	ring->direction = direction;
	ring->slot_count = le16_to_cpu(descriptor.length);
	ring->slot_size = le16_to_cpu(descriptor.buffer_size);
	ring->payload_offset = le16_to_cpu(descriptor.payload_offset);
	ring->id = id;
	ring->offset_vector = (void __iomem *)((u8 __iomem *)sipc->ipc_base +
					       vector_offset);
	vector_size = (size_t)ring->slot_count * sizeof(__le32);
	ring->size_vector = (void __iomem *)((u8 __iomem *)ring->offset_vector +
					     vector_size);
	ret = exynos8890_sbd_expected_buffer(sipc, id, direction, &buffer_offset);
	if (ret)
		return ret;
	ring->buffer_region = (u8 __iomem *)sipc->ipc_base + buffer_offset;

	pointers_offset = readl(&sipc->global_desc->ring_pointer_offset);
	ret = exynos8890_sbd_validate_desc_offset(sipc, pointers_offset,
						 sipc->channel_count * 4 *
						 sizeof(__le16), 2);
	if (ret)
		return ret;
	ring->read_pointer = (__le16 __iomem *)((u8 __iomem *)sipc->ipc_base +
		pointers_offset + (direction * 2 * sipc->channel_count + id) *
		sizeof(__le16));
	ring->write_pointer = (__le16 __iomem *)((u8 __iomem *)sipc->ipc_base +
		pointers_offset + ((direction * 2 + 1) * sipc->channel_count + id) *
		sizeof(__le16));
	ring->fragmented = false;
	ring->expected_length = 0;
	ring->received_length = 0;
	ring->generation++;

	return exynos8890_sbd_validate_ring(ring);
}

int exynos8890_sbd_init(struct exynos8890_sipc *sipc)
{
	struct exynos8890_sbd_global_descriptor descriptor = { };
	size_t pointer_size, vector_size, buffer_size;
	u32 desc_offset, buffer_offset, pointers_offset;
	unsigned int id, direction, i;
	int ret;

	if (!sipc || !sipc->ipc_base || sipc->channel_count >
	    EXYNOS8890_SIPC_MAX_CHANNELS || !sipc->channel_count ||
	    sipc->ipc_size != SZ_8M)
		return -EINVAL;

	BUILD_BUG_ON(sizeof(struct exynos8890_sbd_ring_descriptor) != 20);
	BUILD_BUG_ON(sizeof(struct exynos8890_sbd_channel_descriptor) != 16);
	BUILD_BUG_ON(sizeof(struct exynos8890_sbd_global_descriptor) != 1804);
	for (id = 0; id < sipc->channel_count; id++) {
		spin_lock_init(&sipc->channels[id].tx.lock);
		skb_queue_head_init(&sipc->channels[id].tx.queue);
		spin_lock_init(&sipc->channels[id].rx.lock);
		skb_queue_head_init(&sipc->channels[id].rx.queue);
	}

	memset(sipc->channel_to_id, 0xff, sizeof(sipc->channel_to_id));
	memset(sipc->id_to_channel, 0xff, sizeof(sipc->id_to_channel));
	for (id = 0; id < sipc->channel_count; id++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[id];
		u8 channel_id = channel->config.id;

		sipc->id_to_channel[id] = channel_id;
		/*
		 * Channels with no slots in either direction (rmnetN and
		 * friends) own no SBD ring - their traffic is carried on
		 * @channel->carrier's ring instead. Keep them out of the
		 * config.id -> ring-id table so a real ring-owning channel
		 * (multipdp_hiprio/multipdp) may validly reuse the same
		 * legacy-numbered wire channel id.
		 */
		if (!channel->config.tx_slots && !channel->config.rx_slots)
			continue;
		if (sipc->channel_to_id[channel_id] != EXYNOS8890_SBD_INVALID_ID)
			return -EINVAL;
		sipc->channel_to_id[channel_id] = id;
	}

	desc_offset = EXYNOS8890_SBD_DESC_OFFSET + sizeof(descriptor);
	if (check_mul_overflow((size_t)sipc->channel_count, 4 * sizeof(__le16),
			       &pointer_size) ||
	    desc_offset > EXYNOS8890_SBD_DESC_END ||
	    pointer_size > EXYNOS8890_SBD_DESC_END - desc_offset)
		return -EOVERFLOW;
	pointers_offset = desc_offset;
	desc_offset += pointer_size;
	desc_offset = ALIGN(desc_offset, sizeof(__le32));
	buffer_offset = EXYNOS8890_SBD_BUFFER_OFFSET;

	descriptor.version = cpu_to_le32(EXYNOS8890_SBD_VERSION);
	descriptor.channel_count = cpu_to_le32(sipc->channel_count);
	descriptor.ring_pointer_offset = cpu_to_le32(pointers_offset);

	for (id = 0; id < sipc->channel_count; id++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[id];

		for (direction = 0; direction < EXYNOS8890_SIPC_DIRECTIONS;
		     direction++) {
			struct exynos8890_sbd_ring_descriptor *ring_desc;
			u16 slots = direction == EXYNOS8890_SIPC_TX ?
				channel->config.tx_slots : channel->config.rx_slots;
			u16 slot_size = direction == EXYNOS8890_SIPC_TX ?
				channel->config.tx_buffer_size :
				channel->config.rx_buffer_size;
			u32 ring_desc_offset = EXYNOS8890_SBD_DESC_OFFSET +
				offsetof(struct exynos8890_sbd_global_descriptor,
					 rings[id][direction]);
			__le32 *ring_offset, *ring_vector;

			/*
			 * No ring in this direction (e.g. rmnetN, boot/dump)
			 * - see exynos8890_sipc_channel::carrier. Leave the
			 * descriptor/vector/buffer space untouched and skip.
			 */
			if (!slots)
				continue;
			if (slots < 2 || !slot_size || channel->config.headroom >= slot_size ||
			    check_mul_overflow((size_t)slots, sizeof(__le32), &vector_size) ||
			    vector_size > (EXYNOS8890_SBD_DESC_END - desc_offset) / 2 ||
			    check_mul_overflow((size_t)slots, (size_t)slot_size,
					       &buffer_size) ||
			    buffer_offset > sipc->ipc_size ||
			    buffer_size > sipc->ipc_size - buffer_offset)
				goto overflow;

			ring_desc = &descriptor.rings[id][direction];
			ring_desc->channel = cpu_to_le16(channel->config.id);
			ring_desc->direction = cpu_to_le16(direction);
			ring_desc->signaling = cpu_to_le16(channel->config.interrupt_driven);
			ring_desc->signal_mask = cpu_to_le32(EXYNOS8890_SIPC_INT_VALID |
							 EXYNOS8890_SIPC_SEND_RAW);
			ring_desc->length = cpu_to_le16(slots);
			ring_desc->id = cpu_to_le16(id);
			ring_desc->buffer_size = cpu_to_le16(slot_size);
			ring_desc->payload_offset = cpu_to_le16(channel->config.headroom);
			if (direction == EXYNOS8890_SIPC_TX) {
				ring_offset = &descriptor.channels[id].tx_ring_offset;
				ring_vector = &descriptor.channels[id].tx_vector_offset;
			} else {
				ring_offset = &descriptor.channels[id].rx_ring_offset;
				ring_vector = &descriptor.channels[id].rx_vector_offset;
			}
			*ring_offset = cpu_to_le32(ring_desc_offset);
			*ring_vector = cpu_to_le32(desc_offset);

			for (i = 0; i < slots; i++) {
				writel_relaxed(buffer_offset + i * slot_size,
					       (__le32 __iomem *)((u8 __iomem *)sipc->ipc_base +
					       desc_offset) + i);
				writel_relaxed(0, (__le32 __iomem *)
					       ((u8 __iomem *)sipc->ipc_base + desc_offset +
						vector_size) + i);
			}
			desc_offset += vector_size * 2;
			buffer_offset += buffer_size;
		}
	}

	memset_io((u8 __iomem *)sipc->ipc_base + EXYNOS8890_SBD_DESC_OFFSET, 0,
		  sizeof(descriptor) + pointer_size);
	memcpy_toio((u8 __iomem *)sipc->ipc_base + EXYNOS8890_SBD_DESC_OFFSET,
		    &descriptor, sizeof(descriptor));
	/* Descriptor/vector initialization must precede CP visibility. */
	dma_wmb();
	sipc->global_desc = (void __iomem *)((u8 __iomem *)sipc->ipc_base +
					    EXYNOS8890_SBD_DESC_OFFSET);

	ret = exynos8890_sbd_validate_global(sipc);
	if (ret)
		goto fail;
	for (id = 0; id < sipc->channel_count; id++) {
		/* Ringless directions (see above) have nothing to bind. */
		if (sipc->channels[id].config.tx_slots) {
			ret = exynos8890_sbd_bind_ring(sipc, &sipc->channels[id],
						       &sipc->channels[id].tx,
						       EXYNOS8890_SIPC_TX);
			if (ret)
				goto fail;
		}
		if (sipc->channels[id].config.rx_slots) {
			ret = exynos8890_sbd_bind_ring(sipc, &sipc->channels[id],
						       &sipc->channels[id].rx,
						       EXYNOS8890_SIPC_RX);
			if (ret)
				goto fail;
		}
	}

	return 0;

overflow:
	ret = -EOVERFLOW;
fail:
	exynos8890_sbd_deinit(sipc);
	return ret;
}

void exynos8890_sbd_deinit(struct exynos8890_sipc *sipc)
{
	unsigned int id, direction;

	if (!sipc)
		return;
	for (id = 0; id < sipc->channel_count &&
	     id < EXYNOS8890_SIPC_MAX_CHANNELS; id++) {
		for (direction = 0; direction < EXYNOS8890_SIPC_DIRECTIONS;
		     direction++) {
			struct exynos8890_sbd_ring *ring = direction == EXYNOS8890_SIPC_TX ?
				&sipc->channels[id].tx : &sipc->channels[id].rx;
			unsigned long flags;

			spin_lock_irqsave(&ring->lock, flags);
			ring->generation++;
			ring->read_pointer = NULL;
			ring->write_pointer = NULL;
			ring->offset_vector = NULL;
			ring->size_vector = NULL;
			ring->buffer_region = NULL;
			ring->fragmented = false;
			ring->expected_length = 0;
			ring->received_length = 0;
			skb_queue_purge(&ring->queue);
			spin_unlock_irqrestore(&ring->lock, flags);
		}
	}
	sipc->global_desc = NULL;
	memset(sipc->channel_to_id, 0xff, sizeof(sipc->channel_to_id));
	memset(sipc->id_to_channel, 0xff, sizeof(sipc->id_to_channel));
}

int exynos8890_sbd_validate_global(struct exynos8890_sipc *sipc)
{
	struct exynos8890_sbd_global_descriptor global;
	size_t pointer_size;
	u32 pointer_offset, vector_offset;
	unsigned int direction, id;
	int ret;

	if (!sipc || !sipc->global_desc ||
	    sipc->global_desc != (void __iomem *)((u8 __iomem *)sipc->ipc_base +
						 EXYNOS8890_SBD_DESC_OFFSET) ||
	    !sipc->channel_count ||
	    sipc->channel_count > EXYNOS8890_SIPC_MAX_CHANNELS)
		return -EINVAL;
	ret = exynos8890_sbd_validate_desc_offset(sipc,
			EXYNOS8890_SBD_DESC_OFFSET, sizeof(global), 4);
	if (ret)
		return ret;
	memcpy_fromio(&global, sipc->global_desc, sizeof(global));
	dma_rmb();
	if (le32_to_cpu(global.version) != EXYNOS8890_SBD_VERSION ||
	    le32_to_cpu(global.channel_count) != sipc->channel_count)
		return -EPROTO;
	pointer_offset = le32_to_cpu(global.ring_pointer_offset);
	if (pointer_offset != EXYNOS8890_SBD_DESC_OFFSET + sizeof(global))
		return -EINVAL;
	if (check_mul_overflow((size_t)sipc->channel_count, 4 * sizeof(__le16),
			       &pointer_size))
		return -EOVERFLOW;
	ret = exynos8890_sbd_validate_desc_offset(sipc, pointer_offset,
						 pointer_size, 2);
	if (ret)
		return ret;
	vector_offset = ALIGN(pointer_offset + pointer_size, sizeof(__le32));

	for (id = 0; id < sipc->channel_count; id++) {
		struct exynos8890_sbd_channel_descriptor *channel =
			&global.channels[id];

		for (direction = 0; direction < EXYNOS8890_SIPC_DIRECTIONS;
		     direction++) {
			u32 ring_offset = direction == EXYNOS8890_SIPC_TX ?
				le32_to_cpu(channel->tx_ring_offset) :
				le32_to_cpu(channel->rx_ring_offset);
			u32 actual_vector = direction == EXYNOS8890_SIPC_TX ?
				le32_to_cpu(channel->tx_vector_offset) :
				le32_to_cpu(channel->rx_vector_offset);
			u16 slots = direction == EXYNOS8890_SIPC_TX ?
				sipc->channels[id].config.tx_slots :
				sipc->channels[id].config.rx_slots;
			u16 slot_size = direction == EXYNOS8890_SIPC_TX ?
				sipc->channels[id].config.tx_buffer_size :
				sipc->channels[id].config.rx_buffer_size;
			u32 expected_buffer;
			unsigned int slot;

			/* Ringless direction - nothing was ever written here. */
			if (!slots)
				continue;
			if (actual_vector != vector_offset)
				return -EINVAL;
			ret = exynos8890_sbd_check_descriptor(sipc, id, direction,
						      ring_offset, actual_vector);
			if (ret)
				return ret;
			ret = exynos8890_sbd_expected_buffer(sipc, id, direction,
						     &expected_buffer);
			if (ret)
				return ret;
			for (slot = 0; slot < slots; slot++)
				if (readl_relaxed((__le32 __iomem *)
					((u8 __iomem *)sipc->ipc_base + actual_vector) +
					slot) != expected_buffer + slot * slot_size)
					return -EINVAL;
			vector_offset += (u32)slots * sizeof(__le32) * 2;
		}
	}

	return 0;
}

int exynos8890_sbd_validate_ring(struct exynos8890_sbd_ring *ring)
{
	u16 read, write;
	unsigned int i;

	if (!ring || !ring->sipc || !ring->channel ||
	    ring->direction >= EXYNOS8890_SIPC_DIRECTIONS ||
	    ring->id >= ring->sipc->channel_count || ring->slot_count < 2 ||
	    !ring->slot_size || ring->payload_offset >= ring->slot_size ||
	    !ring->offset_vector || !ring->size_vector)
		return -EINVAL;
	if (exynos8890_sbd_read_pointers(ring, &read, &write))
		return -EIO;
	for (i = 0; i < ring->slot_count; i++)
		if (!exynos8890_sbd_slot(ring, i))
			return -ERANGE;

	return 0;
}

bool exynos8890_sbd_ring_empty(struct exynos8890_sbd_ring *ring)
{
	unsigned long flags;
	u16 read, write;
	bool empty = true;

	if (!ring)
		return true;
	spin_lock_irqsave(&ring->lock, flags);
	if (!exynos8890_sbd_read_pointers(ring, &read, &write))
		empty = read == write;
	spin_unlock_irqrestore(&ring->lock, flags);
	return empty;
}

bool exynos8890_sbd_ring_full(struct exynos8890_sbd_ring *ring)
{
	unsigned long flags;
	u16 read, write;
	bool full = true;

	if (!ring)
		return true;
	spin_lock_irqsave(&ring->lock, flags);
	if (!exynos8890_sbd_read_pointers(ring, &read, &write))
		full = exynos8890_sbd_next(ring, write) == read;
	spin_unlock_irqrestore(&ring->lock, flags);
	return full;
}

unsigned int exynos8890_sbd_ring_count(struct exynos8890_sbd_ring *ring)
{
	unsigned long flags;
	u16 read, write;
	unsigned int count = 0;

	if (!ring)
		return 0;
	spin_lock_irqsave(&ring->lock, flags);
	if (!exynos8890_sbd_read_pointers(ring, &read, &write))
		count = exynos8890_sbd_count(ring->slot_count, read, write);
	spin_unlock_irqrestore(&ring->lock, flags);
	return count;
}

int exynos8890_sbd_tx(struct exynos8890_sbd_ring *ring, struct sk_buff *skb,
		      u8 tag_channel)
{
	void __iomem *slot;
	unsigned long flags;
	u16 read, write, next;
	u32 length, size_entry;
	u8 *data;
	size_t capacity;
	int ret;

	if (!ring || !skb || ring->direction != EXYNOS8890_SIPC_TX)
		return -EINVAL;
	/*
	 * Shared PDP-multiplex rings (multipdp_hiprio/multipdp) demux by a
	 * per-slot channel tag in the upper 16 bits of the size vector -
	 * see link_device_memory_sbd.c sbd_pio_tx(). Plain rings carry no
	 * tag; reject a stray one so a caller bug doesn't silently corrupt
	 * the length field on the wire.
	 */
	if (ring->channel->ps_multiplex != !!tag_channel)
		return -EINVAL;
	length = skb->len;
	if (!length || length > U16_MAX)
		return -EMSGSIZE;
	data = kmalloc(length, GFP_ATOMIC);
	if (!data)
		return -ENOMEM;
	ret = skb_copy_bits(skb, 0, data, length);
	if (ret)
		goto free_data;

	spin_lock_irqsave(&ring->lock, flags);
	ret = exynos8890_sbd_read_pointers(ring, &read, &write);
	if (ret)
		goto unlock;
	next = exynos8890_sbd_next(ring, write);
	if (next == read) {
		ret = -ENOSPC;
		goto unlock;
	}
	capacity = ring->slot_size - ring->payload_offset;
	if (length > capacity) {
		ret = -EMSGSIZE;
		goto unlock;
	}
	slot = exynos8890_sbd_slot(ring, write);
	if (!slot) {
		ret = -ERANGE;
		goto unlock;
	}

	memcpy_toio((u8 __iomem *)slot + ring->payload_offset, data, length);
	size_entry = ring->channel->ps_multiplex ?
		length | ((u32)tag_channel << 16) : length;
	writel_relaxed(size_entry, &ring->size_vector[write]);
	ret = exynos8890_sbd_advance_pointer(ring, ring->write_pointer, next);
unlock:
	spin_unlock_irqrestore(&ring->lock, flags);
free_data:
	kfree(data);
	/* On success SBD owns and consumes skb; on error ownership stays caller's. */
	if (!ret)
		dev_kfree_skb_any(skb);
	return ret ? ret : length;
}

static int exynos8890_sbd_rx_layout(struct exynos8890_sbd_ring *ring,
				    u16 read, u16 write, u32 *wire_length,
				    unsigned int *slot_count)
{
	u8 header[EXYNOS8890_SIPC_MAX_HEADER];
	u32 frame_length, total = 0;
	u16 pointer = read;
	u32 capacity = ring->slot_size - ring->payload_offset;
	u32 size;
	unsigned int available, used = 0;
	void __iomem *slot;

	available = exynos8890_sbd_count(ring->slot_count, read, write);
	if (!available)
		return -EAGAIN;
	size = readl_relaxed(&ring->size_vector[pointer]) & 0xffff;
	if (!size || size > capacity)
		return -EPROTO;
	slot = exynos8890_sbd_slot(ring, pointer);
	if (!slot)
		return -ERANGE;

	if (!ring->channel->config.link_header) {
		*wire_length = size;
		*slot_count = 1;
		return 0;
	}
	if (size < EXYNOS8890_SIPC_MIN_HEADER)
		return -EPROTO;
	memcpy_fromio(header, (u8 __iomem *)slot + ring->payload_offset,
		      min_t(u32, size, sizeof(header)));
	if ((header[0] & EXYNOS8890_SIPC_START_MASK) !=
	    EXYNOS8890_SIPC_START_VALUE ||
	    header[1] != ring->channel->config.id)
		return -EPROTO;
	if ((header[0] & EXYNOS8890_SIPC_EXT_FIELD_MASK) ==
	    EXYNOS8890_SIPC_EXT_LENGTH_CFG) {
		if (size < EXYNOS8890_SIPC_MAX_HEADER)
			return -EPROTO;
		frame_length = get_unaligned_le32(header + 2);
	} else {
		frame_length = get_unaligned_le16(header + 2);
	}
	if (frame_length < EXYNOS8890_SIPC_MIN_HEADER)
		return -EPROTO;
	if (header[0] & EXYNOS8890_SIPC_PADDING_EXIST) {
		u32 padded;

		if (check_add_overflow(frame_length,
				       (u32)sizeof(size_t) - 1, &padded))
			return -EOVERFLOW;
		frame_length = padded & ~((u32)sizeof(size_t) - 1);
	}
	if (!frame_length ||
	    frame_length > (u64)capacity * (ring->slot_count - 1))
		return -EPROTO;

	while (used < available && total < frame_length) {
		size = readl_relaxed(&ring->size_vector[pointer]) & 0xffff;
		if (!size || size > capacity || size > frame_length - total ||
		    !exynos8890_sbd_slot(ring, pointer))
			return -EPROTO;
		total += size;
		used++;
		pointer = exynos8890_sbd_next(ring, pointer);
	}
	if (total != frame_length)
		return -EAGAIN;
	*wire_length = frame_length;
	*slot_count = used;
	return 0;
}

struct sk_buff *exynos8890_sbd_rx(struct exynos8890_sbd_ring *ring, gfp_t gfp,
				  u8 *tag_channel)
{
	struct sk_buff *skb;
	unsigned long flags;
	u32 wire_length, copied;
	u32 generation;
	unsigned int slots, i;
	u16 read, write, pointer;
	int ret;

	if (!ring || ring->direction != EXYNOS8890_SIPC_RX || !tag_channel)
		return ERR_PTR(-EINVAL);

	/* First pass validates without sleeping while holding the ring lock. */
	spin_lock_irqsave(&ring->lock, flags);
	ret = exynos8890_sbd_read_pointers(ring, &read, &write);
	if (!ret)
		ret = exynos8890_sbd_rx_layout(ring, read, write, &wire_length,
					       &slots);
	generation = ring->generation;
	spin_unlock_irqrestore(&ring->lock, flags);
	if (ret)
		return ERR_PTR(ret);

	skb = alloc_skb(wire_length, gfp);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	/* Reset/deinit can race allocation; generation makes pointer ABA harmless. */
	spin_lock_irqsave(&ring->lock, flags);
	if (generation != ring->generation) {
		ret = -EAGAIN;
		goto unlock_free;
	}
	{
		u16 current_read, current_write;
		u32 current_length;
		unsigned int current_slots;

		ret = exynos8890_sbd_read_pointers(ring, &current_read,
						     &current_write);
		if (ret || current_read != read) {
			ret = ret ?: -EAGAIN;
			goto unlock_free;
		}
		ret = exynos8890_sbd_rx_layout(ring, current_read, current_write,
					       &current_length, &current_slots);
		if (ret || current_length != wire_length || current_slots != slots) {
			ret = ret ?: -EAGAIN;
			goto unlock_free;
		}
	}

	/*
	 * Shared PDP-multiplex rings tag each slot with the real target
	 * channel in the upper 16 bits of the size vector (see
	 * exynos8890_sbd_tx()); plain rings have no such tag, so the ring's
	 * own channel id is the answer. ps_multiplex channels never use a
	 * link-layer header, so rx_layout() above always returned a single
	 * slot for them and @read still names it.
	 */
	*tag_channel = ring->channel->ps_multiplex ?
		(u8)(readl_relaxed(&ring->size_vector[read]) >> 16) :
		ring->channel->config.id;
	if (ring->channel->ps_multiplex &&
	    !exynos8890_sipc5_is_ps_channel(*tag_channel)) {
		ret = -EPROTO;
		goto unlock_free;
	}

	ring->fragmented = slots > 1;
	ring->expected_length = wire_length;
	ring->received_length = 0;
	pointer = read;
	copied = 0;
	for (i = 0; i < slots; i++) {
		void __iomem *slot = exynos8890_sbd_slot(ring, pointer);
		u32 size = readl_relaxed(&ring->size_vector[pointer]) & 0xffff;

		if (!slot || !size || size > wire_length - copied) {
			ret = -EPROTO;
			goto unlock_free;
		}
		memcpy_fromio(skb_put(skb, size),
			      (u8 __iomem *)slot + ring->payload_offset, size);
		copied += size;
		ring->received_length = copied;
		pointer = exynos8890_sbd_next(ring, pointer);
	}
	ret = exynos8890_sbd_advance_pointer(ring, ring->read_pointer, pointer);
	if (ret)
		goto unlock_free;
	ring->fragmented = false;
	ring->expected_length = 0;
	ring->received_length = 0;
	spin_unlock_irqrestore(&ring->lock, flags);
	return skb;

unlock_free:
	ring->fragmented = false;
	ring->expected_length = 0;
	ring->received_length = 0;
	spin_unlock_irqrestore(&ring->lock, flags);
	kfree_skb(skb);
	return ERR_PTR(ret);
}

void exynos8890_sbd_reset_ring(struct exynos8890_sbd_ring *ring)
{
	unsigned long flags;
	unsigned int i;

	if (!ring)
		return;
	spin_lock_irqsave(&ring->lock, flags);
	ring->generation++;
	ring->fragmented = false;
	ring->expected_length = 0;
	ring->received_length = 0;
	if (ring->size_vector)
		for (i = 0; i < ring->slot_count; i++)
			writel_relaxed(0, &ring->size_vector[i]);
	if (ring->read_pointer && ring->write_pointer) {
		dma_wmb();
		writew_relaxed(0, ring->read_pointer);
		writew_relaxed(0, ring->write_pointer);
	}
	skb_queue_purge(&ring->queue);
	spin_unlock_irqrestore(&ring->lock, flags);
}

void exynos8890_sbd_reset_all(struct exynos8890_sipc *sipc)
{
	unsigned int id;

	if (!sipc)
		return;
	for (id = 0; id < sipc->channel_count &&
	     id < EXYNOS8890_SIPC_MAX_CHANNELS; id++) {
		exynos8890_sbd_reset_ring(&sipc->channels[id].tx);
		exynos8890_sbd_reset_ring(&sipc->channels[id].rx);
	}
}

MODULE_DESCRIPTION("Exynos8890 SBD shared-memory rings");
MODULE_LICENSE("GPL");
