/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_WWAN_EXYNOS8890_SIPC_H
#define _LINUX_WWAN_EXYNOS8890_SIPC_H

#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/types.h>

struct exynos8890_sipc;
struct exynos8890_sipc_channel;

enum exynos8890_sipc_direction {
	EXYNOS8890_SIPC_TX,
	EXYNOS8890_SIPC_RX,
};

enum exynos8890_sipc_channel_type {
	EXYNOS8890_SIPC_CHANNEL_FORMATTED,
	EXYNOS8890_SIPC_CHANNEL_RAW,
	EXYNOS8890_SIPC_CHANNEL_RFS,
	EXYNOS8890_SIPC_CHANNEL_BOOT,
	EXYNOS8890_SIPC_CHANNEL_DUMP,
	EXYNOS8890_SIPC_CHANNEL_LOG,
	EXYNOS8890_SIPC_CHANNEL_LOOPBACK,
};

enum exynos8890_sipc_link_state {
	EXYNOS8890_SIPC_LINK_OFFLINE,
	EXYNOS8890_SIPC_LINK_BOOT,
	EXYNOS8890_SIPC_LINK_IPC,
	EXYNOS8890_SIPC_LINK_DUMP,
	EXYNOS8890_SIPC_LINK_FAULTED,
};

struct exynos8890_sipc_channel_config {
	u8 id;
	enum exynos8890_sipc_channel_type type;
	const char *name;
	u16 tx_slots;
	u16 rx_slots;
	u16 tx_buffer_size;
	u16 rx_buffer_size;
	u16 headroom;
	bool link_header;
	bool interrupt_driven;
};

struct exynos8890_sipc_stats {
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_dropped;
	u64 malformed_frames;
	u64 ring_errors;
	u64 mailbox_irqs;
};

struct exynos8890_sipc_client_ops {
	void (*link_state_changed)(void *data,
				   enum exynos8890_sipc_link_state state);
	void (*rx_frame)(void *data, u8 channel, struct sk_buff *skb);
	void (*tx_space_available)(void *data, u8 channel);
	void (*transport_fault)(void *data, int error);
};

struct exynos8890_sipc *exynos8890_sipc_get(struct device *consumer);
void exynos8890_sipc_put(struct exynos8890_sipc *sipc);
struct device *exynos8890_sipc_device(struct exynos8890_sipc *sipc);

int exynos8890_sipc_register_client(struct exynos8890_sipc *sipc,
				   const struct exynos8890_sipc_client_ops *ops,
				   void *data);
void exynos8890_sipc_unregister_client(struct exynos8890_sipc *sipc,
				      const struct exynos8890_sipc_client_ops *ops,
				      void *data);

enum exynos8890_sipc_link_state
exynos8890_sipc_link_state(struct exynos8890_sipc *sipc);
int exynos8890_sipc_set_link_state(struct exynos8890_sipc *sipc,
				  enum exynos8890_sipc_link_state state);
int exynos8890_sipc_reset(struct exynos8890_sipc *sipc);
int exynos8890_sipc_start(struct exynos8890_sipc *sipc);
void exynos8890_sipc_stop(struct exynos8890_sipc *sipc);

struct exynos8890_sipc_channel *
exynos8890_sipc_channel_get(struct exynos8890_sipc *sipc, u8 channel);
void exynos8890_sipc_channel_put(struct exynos8890_sipc_channel *channel);
const struct exynos8890_sipc_channel_config *
exynos8890_sipc_channel_config(struct exynos8890_sipc_channel *channel);

int exynos8890_sipc_send(struct exynos8890_sipc_channel *channel,
			 struct sk_buff *skb, bool nonblock);
int exynos8890_sipc_send_command(struct exynos8890_sipc *sipc, u16 command);
int exynos8890_sipc_poll_rx(struct exynos8890_sipc *sipc, int budget);
int exynos8890_sipc_get_stats(struct exynos8890_sipc *sipc,
			     struct exynos8890_sipc_stats *stats);

ssize_t exynos8890_sipc_boot_write(struct exynos8890_sipc *sipc,
				  const void *buffer, size_t length,
				  loff_t offset);
ssize_t exynos8890_sipc_dump_read(struct exynos8890_sipc *sipc,
				 void *buffer, size_t length, loff_t offset);
int exynos8890_sipc_get_srinfo(struct exynos8890_sipc *sipc,
			      void *buffer, size_t length);
int exynos8890_sipc_set_srinfo(struct exynos8890_sipc *sipc,
			      const void *buffer, size_t length);

#endif /* _LINUX_WWAN_EXYNOS8890_SIPC_H */
