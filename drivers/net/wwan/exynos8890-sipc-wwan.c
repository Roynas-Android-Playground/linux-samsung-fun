// SPDX-License-Identifier: GPL-2.0-only
/* Exynos8890 SIPC WWAN, raw-IP netdev and endpoint presentation. */

#include <linux/err.h>
#include <linux/compat.h>
#include <linux/etherdevice.h>
#include <linux/exynos8890_cbd.h>
#include <linux/fs.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wwan.h>

#include "exynos8890-sipc-internal.h"

#define EXYNOS8890_DEFAULT_RX_QLEN	1024
#define EXYNOS8890_MAX_MULTIFRAME	SZ_1M
#define EXYNOS8890_SIPC5_CTL		BIT(0)
#define EXYNOS8890_SIPC5_MORE		BIT(7)
#define EXYNOS8890_SIPC5_ID_MASK	GENMASK(6, 0)

#define EP(_name, _ch, _fmt, _type, _flags, _app, _region, _txn, _txs, _rxn, _rxs) \
	{ .name = _name, .channel = _ch, .format = _fmt, .type = _type, \
	  .flags = _flags, .application = _app, .option_region = _region, \
	  .tx_entries = _txn, .tx_buffer_size = _txs, \
	  .rx_entries = _rxn, .rx_buffer_size = _rxs }

/* Exact effective herolte endpoint inventory; registration applies regions. */
static const struct exynos8890_endpoint_config exynos8890_herolte_endpoints[] = {
	EP("umts_ipc0", 235, EXYNOS8890_ENDPOINT_FMT, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "RIL", NULL, 16, 4096, 32, 4096),
	EP("umts_ipc1", 236, EXYNOS8890_ENDPOINT_FMT, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "RIL", NULL, 16, 4096, 32, 4096),
	EP("umts_rfs0", 245, EXYNOS8890_ENDPOINT_RFS, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "RFS", NULL, 16, 2048, 512, 2048),
	EP("umts_csd", 1, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "CSVT", NULL, 32, 2048, 64, 2048),
	EP("umts_router", 25, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "Data Router", NULL, 16, 2048, 16, 2048),
	EP("umts_dm0", 28, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD,
	   "DIAG", NULL, 16, 2048, 128, 2048),
	EP("rmnet0", 10, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet1", 11, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet2", 12, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet3", 13, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet4", 14, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet5", 15, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet6", 16, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	EP("rmnet7", 17, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_NET,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "NET", NULL, 0, 2048, 0, 2048),
	/*
	 * Real SBD ring ids (link_device_shmem.c init_ctrl_tables() reassigns
	 * QOS_HIPRIO/QOS_NORMAL to 10/11 - see exynos8890_channels[] in
	 * sipc-core.c), NOT the stale DT iod,id=0 both legacy nodes carry.
	 * Must match core.c's channel-table entries exactly or
	 * exynos8890_channel_by_id() below binds the wrong object.
	 */
	EP("multipdp_hiprio", 10, EXYNOS8890_ENDPOINT_MULTI_RAW,
	   EXYNOS8890_ENDPOINT_DUMMY,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "RIL", NULL, 256, 2048, 256, 2048),
	EP("multipdp", 11, EXYNOS8890_ENDPOINT_MULTI_RAW,
	   EXYNOS8890_ENDPOINT_DUMMY,
	   EXYNOS8890_ENDPOINT_F_SBD | EXYNOS8890_ENDPOINT_F_NO_LINK_HEADER,
	   "RIL", NULL, 512, 2048, 1024, 2048),
	/*
	 * umts_boot0 / umts_ramdump0 are NOT registered here: they are owned
	 * exclusively by exynos8890-cbd-compat.c (drivers/misc/), which
	 * registers those exact miscdevice names with the full legacy cbd
	 * ioctl set and reaches this channel via the public
	 * exynos8890_sipc_channel_get()/exynos8890_sipc_boot_write()/
	 * exynos8890_sipc_dump_read() API. Registering them here too would
	 * misc_register() a duplicate "umts_boot0"/"umts_ramdump0" name and
	 * fail one of the two drivers' probe.
	 */
	EP("smd4", 33, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD |
	   EXYNOS8890_ENDPOINT_F_OPTION_REGION,
	   NULL, "kor_skt", 16, 2048, 128, 2048),
	EP("umts_ciq0", 26, EXYNOS8890_ENDPOINT_RAW, EXYNOS8890_ENDPOINT_MISC,
	   EXYNOS8890_ENDPOINT_F_SIPC5 | EXYNOS8890_ENDPOINT_F_SBD |
	   EXYNOS8890_ENDPOINT_F_OPTION_REGION,
	   NULL, "usa_att", 16, 2048, 128, 2048),
};

#undef EP

static char *endpoint_region;
module_param(endpoint_region, charp, 0444);
MODULE_PARM_DESC(endpoint_region,
		 "Enable the matching optional herolte endpoint region");

static bool exynos8890_endpoint_hungup(struct exynos8890_endpoint *endpoint)
{
	enum exynos8890_cp_state state;

	if (READ_ONCE(endpoint->stopping))
		return true;
	if (endpoint->config.format == EXYNOS8890_ENDPOINT_DUMP)
		return false;
	if (endpoint->config.format != EXYNOS8890_ENDPOINT_BOOT &&
	    endpoint->config.format != EXYNOS8890_ENDPOINT_FMT)
		return false;

	state = exynos8890_cpctl_state(endpoint->sipc->cpctl);
	return state == EXYNOS8890_CP_CRASH_RESET ||
	       state == EXYNOS8890_CP_CRASH_EXIT ||
	       state == EXYNOS8890_CP_CRASH_WATCHDOG ||
	       state == EXYNOS8890_CP_FAULTED;
}

static bool exynos8890_endpoint_tx_ready(struct exynos8890_endpoint *endpoint)
{
	struct exynos8890_sipc_channel *channel = endpoint->channel;

	if (READ_ONCE(endpoint->stopping) || !channel || !READ_ONCE(channel->started))
		return false;
	if (!(endpoint->config.flags & EXYNOS8890_ENDPOINT_F_SBD) ||
	    !channel->tx.slot_count)
		return true;
	return !exynos8890_sbd_ring_full(&channel->tx);
}

static int exynos8890_endpoint_xmit(struct exynos8890_endpoint *endpoint,
				   struct sk_buff *skb)
{
	int ret;

	if (READ_ONCE(endpoint->stopping))
		return -ENODEV;
	if (!endpoint->channel)
		return -ENXIO;

	ret = exynos8890_transport_xmit(endpoint->sipc, endpoint, skb);
	if (ret == -ENOSPC)
		ret = -EAGAIN;
	return ret;
}

static int exynos8890_sipc_port_start(struct wwan_port *port)
{
	struct exynos8890_sipc_channel *channel = wwan_port_get_drvdata(port);
	int ret;

	if (!channel || !channel->endpoint)
		return -ENODEV;

	ret = exynos8890_endpoint_open(channel->endpoint);
	if (ret)
		return ret;

	mutex_lock(&channel->tx_lock);
	channel->started = true;
	mutex_unlock(&channel->tx_lock);
	if (!exynos8890_endpoint_tx_ready(channel->endpoint))
		wwan_port_txoff(port);
	return 0;
}

static void exynos8890_sipc_port_stop(struct wwan_port *port)
{
	struct exynos8890_sipc_channel *channel = wwan_port_get_drvdata(port);

	if (!channel || !channel->endpoint)
		return;
	mutex_lock(&channel->tx_lock);
	channel->started = false;
	mutex_unlock(&channel->tx_lock);
	wake_up_interruptible_all(&channel->tx_wait);
	exynos8890_endpoint_close(channel->endpoint);
}

static int exynos8890_sipc_port_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct exynos8890_sipc_channel *channel = wwan_port_get_drvdata(port);
	struct exynos8890_endpoint *endpoint;
	unsigned int len = skb->len;
	int ret;

	if (!channel || !(endpoint = channel->endpoint))
		return -ENODEV;
	if (!exynos8890_endpoint_tx_ready(endpoint)) {
		wwan_port_txoff(port);
		return -EAGAIN;
	}

	ret = exynos8890_endpoint_xmit(endpoint, skb);
	if (!ret) {
		atomic64_inc(&endpoint->tx_packets);
		atomic64_add(len, &endpoint->tx_bytes);
		if (!exynos8890_endpoint_tx_ready(endpoint))
			wwan_port_txoff(port);
	} else if (ret == -EAGAIN) {
		wwan_port_txoff(port);
	}
	return ret;
}

static int exynos8890_sipc_port_tx_blocking(struct wwan_port *port,
					   struct sk_buff *skb)
{
	struct exynos8890_sipc_channel *channel = wwan_port_get_drvdata(port);
	int ret;

	if (!channel || !channel->endpoint)
		return -ENODEV;

	for (;;) {
		ret = wait_event_interruptible(channel->tx_wait,
			exynos8890_endpoint_tx_ready(channel->endpoint) ||
			READ_ONCE(channel->endpoint->stopping) ||
			!READ_ONCE(channel->started));
		if (ret)
			return ret;
		if (READ_ONCE(channel->endpoint->stopping) ||
		    !READ_ONCE(channel->started))
			return -ENODEV;

		ret = exynos8890_sipc_port_tx(port, skb);
		if (ret != -EAGAIN)
			return ret;
	}
}

static __poll_t exynos8890_sipc_port_tx_poll(struct wwan_port *port,
					    struct file *file,
					    poll_table *wait)
{
	struct exynos8890_sipc_channel *channel = wwan_port_get_drvdata(port);

	if (!channel || !channel->endpoint)
		return EPOLLERR | EPOLLHUP;
	poll_wait(file, &channel->tx_wait, wait);
	if (READ_ONCE(channel->endpoint->stopping) ||
	    !READ_ONCE(channel->started))
		return EPOLLERR | EPOLLHUP;
	if (exynos8890_endpoint_tx_ready(channel->endpoint))
		return EPOLLOUT | EPOLLWRNORM;
	return 0;
}

static const struct wwan_port_ops exynos8890_sipc_port_ops = {
	.start = exynos8890_sipc_port_start,
	.stop = exynos8890_sipc_port_stop,
	.tx = exynos8890_sipc_port_tx,
	.tx_blocking = exynos8890_sipc_port_tx_blocking,
	.tx_poll = exynos8890_sipc_port_tx_poll,
};

static struct exynos8890_endpoint *exynos8890_net_endpoint(struct net_device *netdev)
{
	return *(struct exynos8890_endpoint **)netdev_priv(netdev);
}

static int exynos8890_sipc_net_poll(struct napi_struct *napi, int budget)
{
	struct exynos8890_endpoint *endpoint =
		container_of(napi, struct exynos8890_endpoint, napi);
	struct net_device *netdev = endpoint->netdev;
	struct sk_buff *skb;
	int done = 0;

	while (done < budget && (skb = skb_dequeue(&endpoint->rx_queue))) {
		u8 version;
		unsigned int len = skb->len;

		if (unlikely(!len)) {
			atomic64_inc(&endpoint->rx_dropped);
			consume_skb(skb);
			done++;
			continue;
		}

		version = skb->data[0] >> 4;
		if (version == 4)
			skb->protocol = htons(ETH_P_IP);
		else if (version == 6)
			skb->protocol = htons(ETH_P_IPV6);
		else {
			atomic64_inc(&endpoint->rx_dropped);
			consume_skb(skb);
			done++;
			continue;
		}

		skb->dev = netdev;
		skb->pkt_type = PACKET_HOST;
		skb->ip_summed = CHECKSUM_NONE;
		skb_reset_network_header(skb);
		atomic64_inc(&endpoint->rx_packets);
		atomic64_add(len, &endpoint->rx_bytes);
		napi_gro_receive(napi, skb);
		done++;
	}

	if (done < budget)
		napi_complete_done(napi, done);
	return done;
}

static int exynos8890_sipc_net_open(struct net_device *netdev)
{
	struct exynos8890_endpoint *endpoint = exynos8890_net_endpoint(netdev);
	int ret;

	ret = exynos8890_endpoint_open(endpoint);
	if (ret)
		return ret;

	napi_enable(&endpoint->napi);
	WRITE_ONCE(endpoint->napi_enabled, true);
	netif_start_queue(netdev);
	if (exynos8890_sipc_link_state(endpoint->sipc) == EXYNOS8890_SIPC_LINK_IPC)
		netif_carrier_on(netdev);
	else
		netif_carrier_off(netdev);
	return 0;
}

static int exynos8890_sipc_net_stop(struct net_device *netdev)
{
	struct exynos8890_endpoint *endpoint = exynos8890_net_endpoint(netdev);

	netif_stop_queue(netdev);
	netif_carrier_off(netdev);
	WRITE_ONCE(endpoint->napi_enabled, false);
	napi_disable(&endpoint->napi);
	skb_queue_purge(&endpoint->rx_queue);
	exynos8890_endpoint_close(endpoint);
	return 0;
}

static netdev_tx_t exynos8890_sipc_net_xmit(struct sk_buff *skb,
					   struct net_device *netdev)
{
	struct exynos8890_endpoint *endpoint = exynos8890_net_endpoint(netdev);
	unsigned int len = skb->len;
	int ret;

	if (unlikely(!exynos8890_endpoint_tx_ready(endpoint))) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}

	ret = exynos8890_endpoint_xmit(endpoint, skb);
	if (!ret) {
		atomic64_inc(&endpoint->tx_packets);
		atomic64_add(len, &endpoint->tx_bytes);
		if (!exynos8890_endpoint_tx_ready(endpoint))
			netif_stop_queue(netdev);
		return NETDEV_TX_OK;
	}
	if (ret == -EAGAIN) {
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}

	atomic64_inc(&endpoint->tx_dropped);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void exynos8890_sipc_net_get_stats64(struct net_device *netdev,
					    struct rtnl_link_stats64 *stats)
{
	struct exynos8890_endpoint *endpoint = exynos8890_net_endpoint(netdev);

	stats->tx_packets = atomic64_read(&endpoint->tx_packets);
	stats->tx_bytes = atomic64_read(&endpoint->tx_bytes);
	stats->tx_dropped = atomic64_read(&endpoint->tx_dropped);
	stats->rx_packets = atomic64_read(&endpoint->rx_packets);
	stats->rx_bytes = atomic64_read(&endpoint->rx_bytes);
	stats->rx_dropped = atomic64_read(&endpoint->rx_dropped);
}

static const struct net_device_ops exynos8890_sipc_netdev_ops = {
	.ndo_open = exynos8890_sipc_net_open,
	.ndo_stop = exynos8890_sipc_net_stop,
	.ndo_start_xmit = exynos8890_sipc_net_xmit,
	.ndo_get_stats64 = exynos8890_sipc_net_get_stats64,
};

static void exynos8890_sipc_net_setup(struct net_device *netdev)
{
	netdev->netdev_ops = &exynos8890_sipc_netdev_ops;
	netdev->header_ops = NULL;
	netdev->type = ARPHRD_RAWIP;
	netdev->hard_header_len = 0;
	netdev->addr_len = 0;
	netdev->flags = IFF_POINTOPOINT | IFF_NOARP;
	netdev->mtu = ETH_DATA_LEN;
	netdev->min_mtu = 68;
	netdev->max_mtu = 65535;
	netdev->tx_queue_len = 1000;
	netdev->features = NETIF_F_SG | NETIF_F_FRAGLIST;
	netdev->hw_features = netdev->features;
	netdev->needs_free_netdev = true;
}

static int exynos8890_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct exynos8890_endpoint *endpoint =
		container_of(miscdev, struct exynos8890_endpoint, miscdev);
	int ret;

	ret = exynos8890_endpoint_open(endpoint);
	if (ret)
		return ret;
	file->private_data = endpoint;
	return stream_open(inode, file);
}

static int exynos8890_misc_release(struct inode *inode, struct file *file)
{
	struct exynos8890_endpoint *endpoint = file->private_data;

	exynos8890_endpoint_close(endpoint);
	return 0;
}

static ssize_t exynos8890_misc_read(struct file *file, char __user *buffer,
				   size_t length, loff_t *offset)
{
	struct exynos8890_endpoint *endpoint = file->private_data;
	struct sk_buff *skb;
	ssize_t ret;
	size_t copied;

	if (!length)
		return 0;
	ret = wait_event_interruptible(endpoint->read_wait,
		!skb_queue_empty(&endpoint->rx_queue) ||
		exynos8890_endpoint_hungup(endpoint));
	if (ret)
		return ret;

	mutex_lock(&endpoint->read_lock);
	skb = skb_dequeue(&endpoint->rx_queue);
	if (!skb) {
		ret = exynos8890_endpoint_hungup(endpoint) ? 0 : -EAGAIN;
		goto out_unlock;
	}

	copied = min(length, (size_t)skb->len);
	if (copy_to_user(buffer, skb->data, copied)) {
		skb_queue_head(&endpoint->rx_queue, skb);
		ret = -EFAULT;
		goto out_unlock;
	}

	skb_pull(skb, copied);
	if (skb->len)
		skb_queue_head(&endpoint->rx_queue, skb);
	else
		consume_skb(skb);
	ret = copied;

out_unlock:
	mutex_unlock(&endpoint->read_lock);
	return ret;
}

static ssize_t exynos8890_misc_write(struct file *file,
				    const char __user *buffer,
				    size_t length, loff_t *offset)
{
	struct exynos8890_endpoint *endpoint = file->private_data;
	struct exynos8890_sipc_channel *channel = endpoint->channel;
	struct sk_buff *skb;
	int ret;

	if (!length)
		return 0;
	if (!channel)
		return -ENXIO;
	if (endpoint->config.tx_buffer_size &&
	    length > endpoint->config.tx_buffer_size)
		return -EMSGSIZE;

	if (channel->config.link_header) {
		/*
		 * Userspace writes bare payload (matching legacy misc_write());
		 * this driver, like modem_io_device.c's sipc5_build_config()/
		 * sipc5_build_header(), owns prefixing the SIPC5 link header
		 * the CP expects on the wire.
		 */
		void *payload;

		payload = memdup_user(buffer, length);
		if (IS_ERR(payload))
			return PTR_ERR(payload);
		skb = exynos8890_sipc_build_frame(channel, payload, length,
						  GFP_KERNEL);
		kfree(payload);
		if (IS_ERR(skb))
			return PTR_ERR(skb);
	} else {
		skb = alloc_skb(length, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;
		if (copy_from_user(skb_put(skb, length), buffer, length)) {
			kfree_skb(skb);
			return -EFAULT;
		}
	}

	for (;;) {
		ret = wait_event_interruptible(channel->tx_wait,
			exynos8890_endpoint_tx_ready(endpoint) ||
			READ_ONCE(endpoint->stopping));
		if (ret)
			break;
		if (READ_ONCE(endpoint->stopping)) {
			ret = -ENODEV;
			break;
		}
		ret = exynos8890_endpoint_xmit(endpoint, skb);
		if (ret != -EAGAIN)
			break;
	}
	if (ret) {
		kfree_skb(skb);
		atomic64_inc(&endpoint->tx_dropped);
		return ret;
	}

	atomic64_inc(&endpoint->tx_packets);
	atomic64_add(length, &endpoint->tx_bytes);
	return length;
}

static __poll_t exynos8890_misc_poll(struct file *file, poll_table *wait)
{
	struct exynos8890_endpoint *endpoint = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &endpoint->read_wait, wait);
	if (endpoint->channel)
		poll_wait(file, &endpoint->channel->tx_wait, wait);
	if (!skb_queue_empty(&endpoint->rx_queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (exynos8890_endpoint_hungup(endpoint))
		mask |= EPOLLHUP;
	else if (exynos8890_endpoint_tx_ready(endpoint))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

/*
 * Samsung's RIL calls IOCTL_MODEM_STATUS before every read on umts_ipc0 and
 * only proceeds when it answers STATE_ONLINE, so without this the transport
 * below never gets used no matter how well it works. The command numbers are
 * shared with the boot endpoint - it is one ABI, split across two drivers.
 *
 * SIM attach/detach is deliberately not reported here. It is edge-triggered,
 * so reporting it would replace one STATE_ONLINE answer with a state the RIL
 * reads as "not online" and cost a read cycle; the boot endpoint already
 * delivers those transitions.
 */
static long exynos8890_misc_ioctl(struct file *file, unsigned int command,
				  unsigned long argument)
{
	struct exynos8890_endpoint *endpoint = file->private_data;
	struct exynos8890_cp_status status;
	int ret;

	switch (command) {
	case EXYNOS8890_CBD_IOCTL_MODEM_STATUS:
		ret = exynos8890_cpctl_get_status(endpoint->sipc->cpctl,
						  &status);
		if (ret)
			return ret;
		return exynos8890_cp_state_to_legacy(status.state);
	case EXYNOS8890_CBD_IOCTL_NET_SUSPEND:
	case EXYNOS8890_CBD_IOCTL_NET_RESUME:
		return exynos8890_cpctl_set_network_suspended(
			endpoint->sipc->cpctl,
			command == EXYNOS8890_CBD_IOCTL_NET_SUSPEND);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations exynos8890_misc_fops = {
	.owner = THIS_MODULE,
	.open = exynos8890_misc_open,
	.release = exynos8890_misc_release,
	.read = exynos8890_misc_read,
	.write = exynos8890_misc_write,
	.poll = exynos8890_misc_poll,
	.unlocked_ioctl = exynos8890_misc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static ssize_t waketime_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", jiffies_to_msecs(READ_ONCE(endpoint->wake_time)));
}

static ssize_t waketime_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;
	WRITE_ONCE(endpoint->wake_time, msecs_to_jiffies(value));
	return count;
}
static DEVICE_ATTR_RW(waketime);

static ssize_t loopback_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(endpoint->loopback));
}

static ssize_t loopback_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;
	WRITE_ONCE(endpoint->loopback, value);
	return count;
}
static DEVICE_ATTR_RW(loopback);

static ssize_t txlink_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(endpoint->txlink));
}

static ssize_t txlink_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);
	int value;
	int ret;

	ret = kstrtoint(buf, 0, &value);
	if (ret)
		return ret;
	WRITE_ONCE(endpoint->txlink, value);
	return count;
}
static DEVICE_ATTR_RW(txlink);

static struct attribute *exynos8890_dummy_attrs[] = {
	&dev_attr_waketime.attr,
	&dev_attr_loopback.attr,
	&dev_attr_txlink.attr,
	NULL,
};
ATTRIBUTE_GROUPS(exynos8890_dummy);

static void exynos8890_dummy_release(struct device *dev)
{
	struct exynos8890_endpoint *endpoint = dev_get_drvdata(dev);

	complete(&endpoint->dummy_released);
}

/*
 * exynos8890_channels[] (sipc-core.c) deliberately reuses ids 10/11 across
 * two entries each: a ring-less rmnetN/rmnet1 alias (tx_slots==rx_slots==0,
 * carried on multipdp_hiprio's ring - see exynos8890_sipc_channel::carrier)
 * and the real multipdp_hiprio/multipdp ring object that owns actual SBD
 * slots. @want_ring_owner picks the latter (needed when binding the
 * multipdp_hiprio/multipdp DUMMY endpoints below); otherwise the first
 * match by id is returned, matching every other (unique-id) lookup and
 * exynos8890_sipc_channel_get()'s own first-match behaviour in sipc-core.c.
 */
static struct exynos8890_sipc_channel *
exynos8890_channel_by_id(struct exynos8890_sipc *sipc, u8 id,
			 bool want_ring_owner)
{
	struct exynos8890_sipc_channel *first = NULL;
	unsigned int i;

	for (i = 0; i < sipc->channel_count; i++) {
		struct exynos8890_sipc_channel *channel = &sipc->channels[i];

		if (channel->config.id != id)
			continue;
		if (!want_ring_owner)
			return channel;
		if (channel->config.tx_slots || channel->config.rx_slots)
			return channel;
		if (!first)
			first = channel;
	}
	return first;
}

static enum wwan_port_type
exynos8890_endpoint_port_type(const struct exynos8890_endpoint *endpoint)
{
	switch (endpoint->config.format) {
	case EXYNOS8890_ENDPOINT_BOOT:
		return WWAN_PORT_FASTBOOT;
	case EXYNOS8890_ENDPOINT_DUMP:
		return WWAN_PORT_FIREHOSE;
	case EXYNOS8890_ENDPOINT_RAW:
		return WWAN_PORT_QCDM;
	default:
		return WWAN_PORT_QMI;
	}
}

int exynos8890_sipc_register_channel_port(struct exynos8890_sipc_channel *channel)
{
	struct wwan_port_caps caps = {};
	struct exynos8890_endpoint *endpoint;

	if (!channel || !(endpoint = channel->endpoint))
		return -EINVAL;
	if (channel->port)
		return -EALREADY;

	caps.headroom_len = channel->config.headroom;
	caps.frag_len = endpoint->config.tx_buffer_size ?:
		channel->config.tx_buffer_size;
	channel->port = wwan_create_port(channel->sipc->dev,
		exynos8890_endpoint_port_type(endpoint),
		&exynos8890_sipc_port_ops, &caps, channel);
	if (IS_ERR(channel->port)) {
		int ret = PTR_ERR(channel->port);

		channel->port = NULL;
		return ret;
	}
	return 0;
}

void exynos8890_sipc_unregister_channel_port(struct exynos8890_sipc_channel *channel)
{
	struct wwan_port *port;

	if (!channel)
		return;
	port = xchg(&channel->port, NULL);
	if (port)
		wwan_remove_port(port);
}

int exynos8890_sipc_register_netdev(struct exynos8890_sipc_channel *channel)
{
	struct exynos8890_endpoint *endpoint;
	struct net_device *netdev;
	int ret;

	if (!channel || !(endpoint = channel->endpoint))
		return -EINVAL;
	if (channel->netdev || endpoint->netdev)
		return -EALREADY;

	netdev = alloc_netdev(sizeof(endpoint), endpoint->config.name,
			      NET_NAME_USER, exynos8890_sipc_net_setup);
	if (!netdev)
		return -ENOMEM;
	SET_NETDEV_DEV(netdev, channel->sipc->dev);
	*(struct exynos8890_endpoint **)netdev_priv(netdev) = endpoint;
	endpoint->netdev = netdev;
	channel->netdev = netdev;
	netif_napi_add(netdev, &endpoint->napi, exynos8890_sipc_net_poll);
	netif_carrier_off(netdev);

	ret = register_netdev(netdev);
	if (ret) {
		netif_napi_del(&endpoint->napi);
		channel->netdev = NULL;
		endpoint->netdev = NULL;
		free_netdev(netdev);
		return ret;
	}
	return 0;
}

void exynos8890_sipc_unregister_netdev(struct exynos8890_sipc_channel *channel)
{
	struct exynos8890_endpoint *endpoint;
	struct net_device *netdev;

	if (!channel)
		return;
	endpoint = channel->endpoint;
	netdev = xchg(&channel->netdev, NULL);
	if (!netdev)
		return;
	unregister_netdev(netdev);
	if (endpoint) {
		endpoint->netdev = NULL;
		netif_napi_del(&endpoint->napi);
		skb_queue_purge(&endpoint->rx_queue);
	}
	free_netdev(netdev);
}

/*
 * Strip the SIPC5 link header (4/5/6 bytes, matching modem_v1's
 * sipc5_get_hdr_len()/rx_frame_with_link_header()) from @pskb in place, and
 * gather multi-frame fragments (link-layer config CTL bit set - not the
 * SBD-ring-level "multipdp" PDP mux, an unrelated, unfortunately similarly
 * named concept) into one reassembled skb.
 *
 * Only channels carrying a SIPC5 header at all
 * (exynos8890_sipc_channel_config::link_header, e.g. umts_ipc0/umts_rfs0)
 * reach the parsing below; raw-IP channels (rmnetN, multipdp_hiprio's own
 * ring) have no such header and this is a no-op for them.
 */
static int exynos8890_endpoint_complete_multiframe(
	struct exynos8890_endpoint *endpoint, struct sk_buff **pskb)
{
	struct exynos8890_sipc_channel *channel = endpoint->channel;
	struct sk_buff_head *fragments;
	struct sk_buff *skb = *pskb;
	struct sk_buff *part, *joined;
	size_t frame_length, header_length, raw_length;
	unsigned long flags;
	unsigned int total = 0;
	u8 wire_channel, control, id;
	bool more;
	int ret;

	if (!channel || !channel->config.link_header)
		return 0;

	ret = exynos8890_sipc_parse_frame(endpoint->sipc, skb->data, skb->len,
					  &frame_length, &header_length,
					  &wire_channel);
	if (ret)
		return -EBADMSG;
	if (frame_length > skb->len || wire_channel != channel->config.id)
		return -EBADMSG;

	/*
	 * @frame_length includes trailing alignment padding (like
	 * link_device_memory_sbd.c's own wire length); recover the true,
	 * unpadded frame length (header + payload only, matching legacy's
	 * sipc5_get_frame_len()) directly from the wire so padding never
	 * leaks into delivered payload.
	 */
	raw_length = exynos8890_sipc5_has_extended_length(skb->data) ?
		get_unaligned_le32(skb->data + 2) :
		get_unaligned_le16(skb->data + 2);
	if (raw_length < header_length || raw_length > skb->len)
		return -EBADMSG;

	if (!exynos8890_sipc5_is_multiframe(skb->data)) {
		skb_trim(skb, raw_length);
		skb_pull(skb, header_length);
		return 0;
	}

	control = skb->data[4];
	id = control & EXYNOS8890_SIPC5_ID_MASK;
	more = control & EXYNOS8890_SIPC5_MORE;
	skb_trim(skb, raw_length);
	skb_pull(skb, header_length);
	fragments = &endpoint->multiframe[id];

	spin_lock_irqsave(&endpoint->multiframe_lock, flags);
	if (more) {
		if (skb_queue_len(fragments) >= EXYNOS8890_DEFAULT_RX_QLEN) {
			__skb_queue_purge(fragments);
			spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
			return -ENOBUFS;
		}
		__skb_queue_tail(fragments, skb);
		spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
		*pskb = NULL;
		return 1;
	}

	skb_queue_walk(fragments, part) {
		if (check_add_overflow(total, part->len, &total)) {
			__skb_queue_purge(fragments);
			spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
			return -EOVERFLOW;
		}
	}
	if (check_add_overflow(total, skb->len, &total) ||
	    total > EXYNOS8890_MAX_MULTIFRAME) {
		__skb_queue_purge(fragments);
		spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
		return -EMSGSIZE;
	}
	if (skb_queue_empty(fragments)) {
		spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
		return 0;
	}

	joined = alloc_skb(total, GFP_ATOMIC);
	if (!joined) {
		__skb_queue_purge(fragments);
		spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
		return -ENOMEM;
	}
	while ((part = __skb_dequeue(fragments))) {
		skb_put_data(joined, part->data, part->len);
		consume_skb(part);
	}
	skb_put_data(joined, skb->data, skb->len);
	spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
	consume_skb(skb);
	*pskb = joined;
	return 0;
}

int exynos8890_endpoint_rx(struct exynos8890_endpoint *endpoint,
			  struct sk_buff *skb)
{
	unsigned int limit;
	int ret;

	if (!endpoint || !skb) {
		consume_skb(skb);
		return -EINVAL;
	}
	if (READ_ONCE(endpoint->stopping)) {
		consume_skb(skb);
		return -ENODEV;
	}

	ret = exynos8890_endpoint_complete_multiframe(endpoint, &skb);
	if (ret < 0) {
		atomic64_inc(&endpoint->rx_dropped);
		consume_skb(skb);
		return ret;
	}
	if (!skb)
		return 0;

	if (READ_ONCE(endpoint->loopback)) {
		ret = exynos8890_endpoint_xmit(endpoint, skb);
		if (ret)
			consume_skb(skb);
		return ret;
	}

	limit = endpoint->config.rx_entries ?:
		EXYNOS8890_DEFAULT_RX_QLEN;
	if (!(endpoint->config.flags & EXYNOS8890_ENDPOINT_F_NO_RXQ_LIMIT) &&
	    skb_queue_len(&endpoint->rx_queue) >= limit) {
		atomic64_inc(&endpoint->rx_dropped);
		consume_skb(skb);
		return -ENOBUFS;
	}

	if (endpoint->wakeup)
		__pm_wakeup_event(endpoint->wakeup,
			jiffies_to_msecs(READ_ONCE(endpoint->wake_time)));
	skb_queue_tail(&endpoint->rx_queue, skb);
	if (endpoint->config.type == EXYNOS8890_ENDPOINT_NET) {
		if (READ_ONCE(endpoint->napi_enabled))
			napi_schedule(&endpoint->napi);
		else {
			skb = skb_dequeue_tail(&endpoint->rx_queue);
			if (skb) {
				atomic64_inc(&endpoint->rx_dropped);
				consume_skb(skb);
			}
			return -ENETDOWN;
		}
	} else {
		wake_up_interruptible(&endpoint->read_wait);
	}
	return 0;
}

void exynos8890_sipc_wwan_rx(struct exynos8890_sipc_channel *channel,
			    struct sk_buff *skb)
{
	if (!channel || !skb) {
		consume_skb(skb);
		return;
	}
	if (channel->port) {
		wwan_port_rx(channel->port, skb);
		return;
	}
	if (channel->endpoint) {
		exynos8890_endpoint_rx(channel->endpoint, skb);
		return;
	}
	consume_skb(skb);
}

static bool exynos8890_init_end_ready(struct exynos8890_sipc *sipc)
{
	struct exynos8890_endpoint *endpoint;
	bool fmt = false, rfs = false;
	unsigned int i;

	for (i = 0; i < sipc->endpoint_count; i++) {
		endpoint = &sipc->endpoints[i];
		if (!endpoint->registered || !atomic_read(&endpoint->open_count))
			continue;
		if (endpoint->config.format == EXYNOS8890_ENDPOINT_FMT &&
		    endpoint->config.channel == 235)
			fmt = true;
		else if (endpoint->config.format == EXYNOS8890_ENDPOINT_RFS &&
			 endpoint->config.channel == 245)
			rfs = true;
	}
	return fmt && rfs;
}

int exynos8890_endpoint_open(struct exynos8890_endpoint *endpoint)
{
	int ret = 0;

	if (!endpoint)
		return -EINVAL;
	mutex_lock(&endpoint->open_lock);
	if (endpoint->stopping || !endpoint->registered) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!atomic_read(&endpoint->open_count)) {
		ret = exynos8890_transport_open_channel(endpoint->sipc, endpoint);
		if (ret)
			goto out_unlock;
		if (endpoint->channel)
			WRITE_ONCE(endpoint->channel->started, true);
	}
	atomic_inc(&endpoint->open_count);

	if ((endpoint->config.channel == 235 || endpoint->config.channel == 245) &&
	    exynos8890_init_end_ready(endpoint->sipc)) {
		ret = exynos8890_sipc_send_command(endpoint->sipc,
			EXYNOS8890_SIPC_CMD_INIT_END);
		if (ret) {
			if (atomic_dec_and_test(&endpoint->open_count)) {
				if (endpoint->channel)
					WRITE_ONCE(endpoint->channel->started, false);
				exynos8890_transport_close_channel(endpoint->sipc,
					endpoint);
			}
		}
	}

out_unlock:
	mutex_unlock(&endpoint->open_lock);
	return ret;
}

static void exynos8890_endpoint_purge_multiframe(
	struct exynos8890_endpoint *endpoint)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&endpoint->multiframe_lock, flags);
	for (i = 0; i < ARRAY_SIZE(endpoint->multiframe); i++)
		__skb_queue_purge(&endpoint->multiframe[i]);
	spin_unlock_irqrestore(&endpoint->multiframe_lock, flags);
}

void exynos8890_endpoint_close(struct exynos8890_endpoint *endpoint)
{
	if (!endpoint)
		return;
	mutex_lock(&endpoint->open_lock);
	if (WARN_ON(!atomic_read(&endpoint->open_count)))
		goto out_unlock;
	if (atomic_dec_and_test(&endpoint->open_count)) {
		if (endpoint->channel)
			WRITE_ONCE(endpoint->channel->started, false);
		exynos8890_transport_close_channel(endpoint->sipc, endpoint);
		skb_queue_purge(&endpoint->rx_queue);
		exynos8890_endpoint_purge_multiframe(endpoint);
		wake_up_interruptible_all(&endpoint->read_wait);
		wake_up_all(&endpoint->close_wait);
	}

out_unlock:
	mutex_unlock(&endpoint->open_lock);
}

void exynos8890_endpoint_state_changed(struct exynos8890_endpoint *endpoint,
				      enum exynos8890_cp_state state)
{
	if (!endpoint || !endpoint->registered)
		return;

	if (endpoint->netdev) {
		if (state == EXYNOS8890_CP_ONLINE) {
			netif_carrier_on(endpoint->netdev);
			if (exynos8890_endpoint_tx_ready(endpoint))
				netif_wake_queue(endpoint->netdev);
		} else {
			netif_carrier_off(endpoint->netdev);
			netif_stop_queue(endpoint->netdev);
		}
	}
	if (endpoint->channel) {
		wake_up_interruptible_all(&endpoint->channel->tx_wait);
		if (endpoint->channel->port &&
		    state == EXYNOS8890_CP_ONLINE &&
		    exynos8890_endpoint_tx_ready(endpoint))
			wwan_port_txon(endpoint->channel->port);
	}
	wake_up_interruptible_all(&endpoint->read_wait);
}

struct exynos8890_endpoint *
exynos8890_endpoint_by_channel(struct exynos8890_sipc *sipc, u8 channel)
{
	unsigned int i;

	if (!sipc)
		return NULL;
	for (i = 0; i < sipc->endpoint_count; i++)
		if (sipc->endpoints[i].registered &&
		    sipc->endpoints[i].config.channel == channel)
			return &sipc->endpoints[i];
	return NULL;
}

int exynos8890_endpoint_register(struct exynos8890_sipc *sipc,
				const struct exynos8890_endpoint_config *config,
				struct exynos8890_endpoint **out)
{
	struct exynos8890_endpoint *endpoint = NULL;
	unsigned int i;
	int ret;

	if (!sipc || !config || !config->name || !out || !sipc->endpoints)
		return -EINVAL;
	for (i = 0; i < sipc->endpoint_count; i++)
		if (!sipc->endpoints[i].config.name) {
			endpoint = &sipc->endpoints[i];
			break;
		}
	if (!endpoint)
		return -ENOSPC;

	endpoint->sipc = sipc;
	endpoint->config = *config;
	endpoint->channel = exynos8890_channel_by_id(sipc, config->channel,
		config->type == EXYNOS8890_ENDPOINT_DUMMY);
	if (config->type != EXYNOS8890_ENDPOINT_DUMMY && !endpoint->channel) {
		ret = -ENODEV;
		goto err_clear;
	}
	if (endpoint->channel && endpoint->channel->endpoint &&
	    config->type != EXYNOS8890_ENDPOINT_DUMMY) {
		ret = -EEXIST;
		goto err_clear;
	}

	atomic_set(&endpoint->open_count, 0);
	mutex_init(&endpoint->open_lock);
	mutex_init(&endpoint->read_lock);
	init_waitqueue_head(&endpoint->close_wait);
	init_waitqueue_head(&endpoint->read_wait);
	skb_queue_head_init(&endpoint->rx_queue);
	spin_lock_init(&endpoint->multiframe_lock);
	for (i = 0; i < ARRAY_SIZE(endpoint->multiframe); i++)
		skb_queue_head_init(&endpoint->multiframe[i]);
	endpoint->wake_time = msecs_to_jiffies(500);
	endpoint->wakeup = wakeup_source_register(sipc->dev, config->name);
	if (!endpoint->wakeup) {
		ret = -ENOMEM;
		goto err_clear;
	}
	if (endpoint->channel)
		endpoint->channel->endpoint = endpoint;

	switch (config->type) {
	case EXYNOS8890_ENDPOINT_MISC:
		endpoint->miscdev.minor = MISC_DYNAMIC_MINOR;
		endpoint->miscdev.name = config->name;
		endpoint->miscdev.fops = &exynos8890_misc_fops;
		endpoint->miscdev.parent = sipc->dev;
		ret = misc_register(&endpoint->miscdev);
		break;
	case EXYNOS8890_ENDPOINT_NET:
		ret = exynos8890_sipc_register_netdev(endpoint->channel);
		break;
	case EXYNOS8890_ENDPOINT_DUMMY:
		init_completion(&endpoint->dummy_released);
		device_initialize(&endpoint->dummy_dev);
		endpoint->dummy_dev.parent = sipc->dev;
		endpoint->dummy_dev.release = exynos8890_dummy_release;
		endpoint->dummy_dev.groups = exynos8890_dummy_groups;
		dev_set_drvdata(&endpoint->dummy_dev, endpoint);
		ret = dev_set_name(&endpoint->dummy_dev, "%s", config->name);
		if (!ret)
			ret = device_add(&endpoint->dummy_dev);
		if (ret) {
			put_device(&endpoint->dummy_dev);
			wait_for_completion(&endpoint->dummy_released);
		} else {
			endpoint->dummy_registered = true;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret)
		goto err_channel;

	endpoint->registered = true;
	*out = endpoint;
	return 0;

err_channel:
	if (endpoint->channel && endpoint->channel->endpoint == endpoint)
		endpoint->channel->endpoint = NULL;
	wakeup_source_unregister(endpoint->wakeup);
	endpoint->wakeup = NULL;
err_clear:
	memset(endpoint, 0, sizeof(*endpoint));
	return ret;
}

void exynos8890_endpoint_unregister(struct exynos8890_endpoint *endpoint)
{
	if (!endpoint || !endpoint->registered)
		return;

	mutex_lock(&endpoint->open_lock);
	endpoint->stopping = true;
	mutex_unlock(&endpoint->open_lock);
	wake_up_interruptible_all(&endpoint->read_wait);
	if (endpoint->channel)
		wake_up_interruptible_all(&endpoint->channel->tx_wait);

	switch (endpoint->config.type) {
	case EXYNOS8890_ENDPOINT_MISC:
		misc_deregister(&endpoint->miscdev);
		break;
	case EXYNOS8890_ENDPOINT_NET:
		exynos8890_sipc_unregister_netdev(endpoint->channel);
		break;
	case EXYNOS8890_ENDPOINT_DUMMY:
		if (endpoint->dummy_registered) {
			endpoint->dummy_registered = false;
			device_unregister(&endpoint->dummy_dev);
			wait_for_completion(&endpoint->dummy_released);
		}
		break;
	}

	wait_event(endpoint->close_wait, !atomic_read(&endpoint->open_count));
	skb_queue_purge(&endpoint->rx_queue);
	exynos8890_endpoint_purge_multiframe(endpoint);
	if (endpoint->channel && endpoint->channel->endpoint == endpoint)
		endpoint->channel->endpoint = NULL;
	wakeup_source_unregister(endpoint->wakeup);
	endpoint->wakeup = NULL;
	endpoint->registered = false;
}

int exynos8890_sipc_register_wwan(struct exynos8890_sipc *sipc)
{
	struct exynos8890_endpoint *endpoint;
	unsigned int i;
	int ret;

	if (!sipc || sipc->endpoints)
		return -EINVAL;
	sipc->endpoint_count = ARRAY_SIZE(exynos8890_herolte_endpoints);
	sipc->endpoints = kcalloc(sipc->endpoint_count,
				 sizeof(*sipc->endpoints), GFP_KERNEL);
	if (!sipc->endpoints) {
		sipc->endpoint_count = 0;
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(exynos8890_herolte_endpoints); i++) {
		const struct exynos8890_endpoint_config *config =
			&exynos8890_herolte_endpoints[i];

		if ((config->flags & EXYNOS8890_ENDPOINT_F_OPTION_REGION) &&
		    (!endpoint_region || strcmp(endpoint_region,
					    config->option_region)))
			continue;
		ret = exynos8890_endpoint_register(sipc, config, &endpoint);
		if (ret)
			goto err_unregister;
	}
	return 0;

err_unregister:
	while (i--)
		exynos8890_endpoint_unregister(&sipc->endpoints[i]);
	kfree(sipc->endpoints);
	sipc->endpoints = NULL;
	sipc->endpoint_count = 0;
	return ret;
}

void exynos8890_sipc_unregister_wwan(struct exynos8890_sipc *sipc)
{
	unsigned int i;

	if (!sipc || !sipc->endpoints)
		return;
	for (i = sipc->endpoint_count; i > 0; i--)
		exynos8890_endpoint_unregister(&sipc->endpoints[i - 1]);
	kfree(sipc->endpoints);
	sipc->endpoints = NULL;
	sipc->endpoint_count = 0;
}

MODULE_DESCRIPTION("Exynos8890 SIPC WWAN and endpoint presentation");
MODULE_LICENSE("GPL");
