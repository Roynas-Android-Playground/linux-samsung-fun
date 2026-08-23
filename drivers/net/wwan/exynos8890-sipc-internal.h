/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _EXYNOS8890_SIPC_INTERNAL_H
#define _EXYNOS8890_SIPC_INTERNAL_H

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/pm_wakeup.h>
#include <linux/refcount.h>
#include <linux/srcu.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/wwan.h>
#include <linux/wwan/exynos8890-sipc.h>
#include <linux/soc/samsung/exynos8890-cpctl.h>

#define EXYNOS8890_SIPC_MAX_CHANNELS	32
#define EXYNOS8890_SIPC_DIRECTIONS	2
#define EXYNOS8890_SIPC_MIN_HEADER	4
#define EXYNOS8890_SIPC_MAX_HEADER	6
#define EXYNOS8890_SIPC_START_MASK	GENMASK(7, 3)
#define EXYNOS8890_SIPC_START_VALUE	GENMASK(7, 3)
#define EXYNOS8890_SIPC_PADDING_EXIST	BIT(2)
#define EXYNOS8890_SIPC_EXT_FIELD_EXIST	BIT(1)
#define EXYNOS8890_SIPC_CTL_FIELD_EXIST	BIT(0)
#define EXYNOS8890_SIPC_EXT_FIELD_MASK	GENMASK(1, 0)
#define EXYNOS8890_SIPC_MULTI_FRAME_CFG	GENMASK(1, 0)
#define EXYNOS8890_SIPC_EXT_LENGTH_CFG	BIT(1)
#define EXYNOS8890_SIPC_SRINFO_OFFSET	0xf800
#define EXYNOS8890_SIPC_SRINFO_SIZE	0x0800
#define EXYNOS8890_SBD_CTRL_OFFSET	0x0000
#define EXYNOS8890_SBD_CTRL_SIZE		0x0400
#define EXYNOS8890_SBD_COMMAND_OFFSET	0x0400
#define EXYNOS8890_SBD_COMMAND_SIZE	0x0400
#define EXYNOS8890_SBD_DESC_OFFSET	0x0800
#define EXYNOS8890_SBD_BUFFER_OFFSET	0x10000
#define EXYNOS8890_SBD_DESC_END		0x10000
#define EXYNOS8890_SBD_VERSION		1

#define EXYNOS8890_SIPC_INT_VALID	0x0080
#define EXYNOS8890_SIPC_CMD_VALID	0x0040
#define EXYNOS8890_SIPC_CMD_MASK		0x003f
#define EXYNOS8890_SIPC_CMD_INIT_START	0x0001
#define EXYNOS8890_SIPC_CMD_INIT_END	0x0002
#define EXYNOS8890_SIPC_CMD_CRASH_RESET	0x0007
#define EXYNOS8890_SIPC_CMD_PHONE_START	0x0008
#define EXYNOS8890_SIPC_CMD_CRASH_EXIT	0x0009
#define EXYNOS8890_SIPC_CMD_PIF_INIT_DONE 0x000d
#define EXYNOS8890_SIPC_REQ_ACK_FMT	0x0020
#define EXYNOS8890_SIPC_REQ_ACK_RAW	0x0010
#define EXYNOS8890_SIPC_RES_ACK_FMT	0x0008
#define EXYNOS8890_SIPC_RES_ACK_RAW	0x0004
#define EXYNOS8890_SIPC_SEND_FMT		0x0002
#define EXYNOS8890_SIPC_SEND_RAW		0x0001

enum exynos8890_endpoint_format {
	EXYNOS8890_ENDPOINT_FMT,
	EXYNOS8890_ENDPOINT_RAW,
	EXYNOS8890_ENDPOINT_RFS,
	EXYNOS8890_ENDPOINT_MULTI_RAW,
	EXYNOS8890_ENDPOINT_BOOT,
	EXYNOS8890_ENDPOINT_DUMP,
	EXYNOS8890_ENDPOINT_COMMAND,
	EXYNOS8890_ENDPOINT_DEBUG,
};

enum exynos8890_endpoint_type {
	EXYNOS8890_ENDPOINT_MISC,
	EXYNOS8890_ENDPOINT_NET,
	EXYNOS8890_ENDPOINT_DUMMY,
};

#define EXYNOS8890_ENDPOINT_F_SIPC5		BIT(1)
#define EXYNOS8890_ENDPOINT_F_SBD		BIT(7)
#define EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER	BIT(8)
#define EXYNOS8890_ENDPOINT_F_NO_RXQ_LIMIT	BIT(9)
#define EXYNOS8890_ENDPOINT_F_DUAL_SIM		BIT(10)
#define EXYNOS8890_ENDPOINT_F_OPTION_REGION	BIT(11)

struct exynos8890_endpoint_config {
	const char *name;
	u8 channel;
	enum exynos8890_endpoint_format format;
	enum exynos8890_endpoint_type type;
	u32 flags;
	const char *application;
	const char *option_region;
	u16 tx_entries;
	u16 tx_buffer_size;
	u16 rx_entries;
	u16 rx_buffer_size;
};

struct exynos8890_shmem_geometry {
	phys_addr_t base;
	size_t total_size;
	size_t boot_offset;
	size_t boot_size;
	size_t ipc_offset;
	size_t ipc_size;
};

struct exynos8890_shmem_mapping {
	void __iomem *vaddr;
	phys_addr_t paddr;
	size_t size;
};

struct exynos8890_shmem {
	struct device *dev;
	struct exynos8890_shmem_geometry geometry;
	struct exynos8890_shmem_mapping boot;
	struct exynos8890_shmem_mapping ipc;
	struct mutex map_lock;
	unsigned int boot_users;
	unsigned int ipc_users;
	bool attached;
};

/* Wire layouts. Validate every size/offset against the pinned vendor ABI. */
struct exynos8890_sipc5_header {
	u8 config;
	u8 channel;
	__le16 length;
	union {
		u8 control;
		__le16 extended_length;
	};
} __packed;

struct exynos8890_sbd_ring_descriptor {
	__le16 channel;
	__le16 reserved;
	__le16 direction;
	__le16 signaling;
	__le32 signal_mask;
	__le16 length;
	__le16 id;
	__le16 buffer_size;
	__le16 payload_offset;
} __packed;

struct exynos8890_sbd_channel_descriptor {
	__le32 tx_ring_offset;
	__le32 tx_vector_offset;
	__le32 rx_ring_offset;
	__le32 rx_vector_offset;
} __packed;

struct exynos8890_sbd_global_descriptor {
	__le32 version;
	__le32 channel_count;
	__le32 ring_pointer_offset;
	struct exynos8890_sbd_channel_descriptor
		channels[EXYNOS8890_SIPC_MAX_CHANNELS];
	struct exynos8890_sbd_ring_descriptor
		rings[EXYNOS8890_SIPC_MAX_CHANNELS][EXYNOS8890_SIPC_DIRECTIONS];
} __packed;

struct exynos8890_sbd_ring {
	struct exynos8890_sipc *sipc;
	struct exynos8890_sipc_channel *channel;
	enum exynos8890_sipc_direction direction;
	spinlock_t lock;
	struct sk_buff_head queue;
	void __iomem *buffer_region;
	__le16 __iomem *read_pointer;
	__le16 __iomem *write_pointer;
	__le32 __iomem *offset_vector;
	__le32 __iomem *size_vector;
	u16 slot_count;
	u16 slot_size;
	u16 payload_offset;
	u16 id;
	bool fragmented;
	u32 expected_length;
	u32 received_length;
	u32 generation;
};

struct exynos8890_sipc_channel {
	struct exynos8890_sipc *sipc;
	struct exynos8890_sipc_channel_config config;
	struct exynos8890_sbd_ring tx;
	struct exynos8890_sbd_ring rx;
	struct exynos8890_endpoint *endpoint;
	struct wwan_port *port;
	struct net_device *netdev;
	refcount_t users;
	struct mutex tx_lock;
	wait_queue_head_t tx_wait;
	bool started;
	/*
	 * Legacy driver PDP-multiplexing: channels whose id falls in the
	 * SIPC "PS" range (multipdp_hiprio/multipdp's real ring peers, e.g.
	 * rmnetN) own no ring of their own (tx/rx slot_count == 0). Their
	 * traffic is carried on @carrier's ring instead, tagged with this
	 * channel's id in the upper 16 bits of the SBD size vector entry -
	 * see link_device_memory_sbd.c sbd_pio_tx()/set_skb_priv().
	 */
	struct exynos8890_sipc_channel *carrier;
	/*
	 * True for a channel that itself is a shared PDP-multiplex ring
	 * (multipdp_hiprio/multipdp): every slot on tx/rx carries a 16-bit
	 * real-channel tag alongside the payload length.
	 */
	bool ps_multiplex;
};

struct exynos8890_sipc_client {
	struct list_head node;
	const struct exynos8890_sipc_client_ops *ops;
	void *data;
	struct rcu_head rcu;
};

struct exynos8890_endpoint {
	struct exynos8890_sipc *sipc;
	struct exynos8890_sipc_channel *channel;
	struct exynos8890_endpoint_config config;
	atomic_t open_count;
	struct mutex open_lock;
	struct mutex read_lock;
	wait_queue_head_t close_wait;
	struct miscdevice miscdev;
	struct net_device *netdev;
	struct napi_struct napi;
	wait_queue_head_t read_wait;
	struct sk_buff_head rx_queue;
	struct sk_buff_head multiframe[128];
	spinlock_t multiframe_lock;
	u8 multiframe_id;
	struct wakeup_source *wakeup;
	unsigned long wake_time;
	atomic64_t tx_packets;
	atomic64_t tx_bytes;
	atomic64_t tx_dropped;
	atomic64_t rx_packets;
	atomic64_t rx_bytes;
	atomic64_t rx_dropped;
	struct device dummy_dev;
	struct completion dummy_released;
	bool loopback;
	int txlink;
	bool registered;
	bool dummy_registered;
	bool stopping;
	bool napi_enabled;
};

struct exynos8890_transport_ops {
	int (*start)(struct exynos8890_sipc *sipc);
	void (*stop)(struct exynos8890_sipc *sipc);
	int (*open_channel)(struct exynos8890_sipc *sipc,
			    struct exynos8890_endpoint *endpoint);
	void (*close_channel)(struct exynos8890_sipc *sipc,
			      struct exynos8890_endpoint *endpoint);
	int (*xmit)(struct exynos8890_sipc *sipc,
		    struct exynos8890_endpoint *endpoint,
		    struct sk_buff *skb);
	int (*prepare_boot)(struct exynos8890_sipc *sipc);
	int (*prepare_download)(struct exynos8890_sipc *sipc);
	int (*prepare_dump)(struct exynos8890_sipc *sipc);
	int (*force_crash)(struct exynos8890_sipc *sipc);
	void (*close_tx)(struct exynos8890_sipc *sipc);
};

struct exynos8890_sipc {
	struct device *dev;
	struct kref refcount;
	struct exynos8890_cpctl *cpctl;
	struct exynos8890_shmem *shmem;
	const struct exynos8890_transport_ops *transport_ops;
	struct notifier_block cp_notifier;
	void __iomem *shared_base;
	phys_addr_t shared_phys;
	size_t shared_size;
	void __iomem *ipc_base;
	size_t ipc_size;
	struct exynos8890_sbd_global_descriptor __iomem *global_desc;
	struct exynos8890_sipc_channel channels[EXYNOS8890_SIPC_MAX_CHANNELS];
	unsigned int channel_count;
	u16 id_to_channel[EXYNOS8890_SIPC_MAX_CHANNELS];
	u16 channel_to_id[256];
	struct exynos8890_endpoint *endpoints;
	unsigned int endpoint_count;
	struct mutex state_lock;
	enum exynos8890_sipc_link_state state;
	struct mutex client_lock;
	struct srcu_struct client_srcu;
	struct list_head clients;
	struct napi_struct napi;
	struct work_struct rx_work;
	struct work_struct fault_work;
	struct exynos8890_sipc_stats stats;
	spinlock_t stats_lock;
	int fault;
	bool stopping;
	bool transport_started;
	bool notifier_registered;
};

/* Core and protocol. */
int exynos8890_sipc_parse_dt(struct exynos8890_sipc *sipc);
int exynos8890_sipc_map_shared_memory(struct exynos8890_sipc *sipc);
void exynos8890_sipc_unmap_shared_memory(struct exynos8890_sipc *sipc);
int exynos8890_sipc_validate_layout(struct exynos8890_sipc *sipc);
int exynos8890_sipc_init_channels(struct exynos8890_sipc *sipc);
void exynos8890_sipc_deinit_channels(struct exynos8890_sipc *sipc);
int exynos8890_sipc_parse_frame(struct exynos8890_sipc *sipc,
			       const void *data, size_t available,
			       size_t *frame_length, size_t *header_length,
			       u8 *channel);
struct sk_buff *exynos8890_sipc_build_frame(struct exynos8890_sipc_channel *channel,
					   const void *payload,
					   size_t payload_length,
					   gfp_t gfp);
void exynos8890_sipc_report_fault(struct exynos8890_sipc *sipc, int error);
void exynos8890_sipc_notify_clients(struct exynos8890_sipc *sipc,
				   enum exynos8890_sipc_link_state state);
bool exynos8890_sipc5_start_valid(const void *frame);
bool exynos8890_sipc5_has_padding(const void *frame);
bool exynos8890_sipc5_is_multiframe(const void *frame);
bool exynos8890_sipc5_has_extended_length(const void *frame);
size_t exynos8890_sipc5_padding(size_t frame_length, bool aligned);
bool exynos8890_sipc5_is_ipc_channel(u8 channel);
bool exynos8890_sipc5_is_boot_channel(u8 channel);
bool exynos8890_sipc5_is_dump_channel(u8 channel);
bool exynos8890_sipc5_is_fmt_channel(u8 channel);
bool exynos8890_sipc5_is_rfs_channel(u8 channel);
bool exynos8890_sipc5_is_ps_channel(u8 channel);

/* Reserved memory. */
int exynos8890_shmem_get(struct device *consumer,
			struct exynos8890_shmem **out);
void exynos8890_shmem_put(struct exynos8890_shmem *shmem);
int exynos8890_shmem_validate(const struct exynos8890_shmem_geometry *geometry);
int exynos8890_shmem_map_boot(struct exynos8890_shmem *shmem,
			     struct exynos8890_shmem_mapping **out);
int exynos8890_shmem_map_ipc(struct exynos8890_shmem *shmem,
			    struct exynos8890_shmem_mapping **out);
void exynos8890_shmem_unmap_boot(struct exynos8890_shmem *shmem);
void exynos8890_shmem_unmap_ipc(struct exynos8890_shmem *shmem);
int exynos8890_shmem_copy_image(struct exynos8890_shmem *shmem,
			       bool dump_region, u32 offset,
			       const void __user *source, u32 length);

/* Transport state and command handling. */
int exynos8890_transport_open_channel(struct exynos8890_sipc *sipc,
				     struct exynos8890_endpoint *endpoint);
void exynos8890_transport_close_channel(struct exynos8890_sipc *sipc,
				       struct exynos8890_endpoint *endpoint);
int exynos8890_transport_xmit(struct exynos8890_sipc *sipc,
			     struct exynos8890_endpoint *endpoint,
			     struct sk_buff *skb);
int exynos8890_transport_prepare_boot(struct exynos8890_sipc *sipc);
int exynos8890_transport_prepare_download(struct exynos8890_sipc *sipc);
int exynos8890_transport_prepare_dump(struct exynos8890_sipc *sipc);
int exynos8890_transport_force_crash(struct exynos8890_sipc *sipc);
void exynos8890_transport_close_tx(struct exynos8890_sipc *sipc);
void exynos8890_transport_handle_command(struct exynos8890_sipc *sipc,
					u16 command);
int exynos8890_transport_handle_init_start(struct exynos8890_sipc *sipc);
int exynos8890_transport_handle_phone_start(struct exynos8890_sipc *sipc);
void exynos8890_transport_handle_crash_reset(struct exynos8890_sipc *sipc);
void exynos8890_transport_handle_crash_exit(struct exynos8890_sipc *sipc);

/* SBD transport. */
int exynos8890_sbd_init(struct exynos8890_sipc *sipc);
void exynos8890_sbd_deinit(struct exynos8890_sipc *sipc);
int exynos8890_sbd_validate_global(struct exynos8890_sipc *sipc);
int exynos8890_sbd_validate_ring(struct exynos8890_sbd_ring *ring);
bool exynos8890_sbd_ring_empty(struct exynos8890_sbd_ring *ring);
bool exynos8890_sbd_ring_full(struct exynos8890_sbd_ring *ring);
unsigned int exynos8890_sbd_ring_count(struct exynos8890_sbd_ring *ring);
int exynos8890_sbd_tx(struct exynos8890_sbd_ring *ring, struct sk_buff *skb,
		      u8 tag_channel);
struct sk_buff *exynos8890_sbd_rx(struct exynos8890_sbd_ring *ring, gfp_t gfp,
				  u8 *tag_channel);
void exynos8890_sbd_reset_ring(struct exynos8890_sbd_ring *ring);
void exynos8890_sbd_reset_all(struct exynos8890_sipc *sipc);

/* WWAN and network presentation. */
int exynos8890_sipc_register_wwan(struct exynos8890_sipc *sipc);
void exynos8890_sipc_unregister_wwan(struct exynos8890_sipc *sipc);
int exynos8890_sipc_register_channel_port(struct exynos8890_sipc_channel *channel);
void exynos8890_sipc_unregister_channel_port(struct exynos8890_sipc_channel *channel);
int exynos8890_sipc_register_netdev(struct exynos8890_sipc_channel *channel);
void exynos8890_sipc_unregister_netdev(struct exynos8890_sipc_channel *channel);
void exynos8890_sipc_wwan_rx(struct exynos8890_sipc_channel *channel,
			    struct sk_buff *skb);

/* Endpoint lifecycle. */
int exynos8890_endpoint_register(struct exynos8890_sipc *sipc,
				const struct exynos8890_endpoint_config *config,
				struct exynos8890_endpoint **out);
void exynos8890_endpoint_unregister(struct exynos8890_endpoint *endpoint);
int exynos8890_endpoint_open(struct exynos8890_endpoint *endpoint);
void exynos8890_endpoint_close(struct exynos8890_endpoint *endpoint);
int exynos8890_endpoint_rx(struct exynos8890_endpoint *endpoint,
			  struct sk_buff *skb);
void exynos8890_endpoint_state_changed(struct exynos8890_endpoint *endpoint,
				      enum exynos8890_cp_state state);
struct exynos8890_endpoint *
exynos8890_endpoint_by_channel(struct exynos8890_sipc *sipc, u8 channel);

#endif /* _EXYNOS8890_SIPC_INTERNAL_H */
