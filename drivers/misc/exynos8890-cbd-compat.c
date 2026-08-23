// SPDX-License-Identifier: GPL-2.0-only
/* Compatibility layer for the legacy Samsung cbd userspace ABI. */

#include <linux/build_bug.h>
#include <linux/compat.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/exynos8890_cbd.h>
#include <linux/soc/samsung/exynos8890-cpctl.h>
#include <linux/wwan/exynos8890-sipc.h>

#define EXYNOS8890_CBD_MAX_RX_QLEN	2048
#define EXYNOS8890_CBD_MAX_IO		SZ_1M
#define EXYNOS8890_CBD_SRINFO_SIZE	SZ_2K
#define EXYNOS8890_CBD_SHMEM_LINK	9

struct exynos8890_cbd;

enum exynos8890_cbd_endpoint_type {
	EXYNOS8890_CBD_ENDPOINT_BOOT,
	EXYNOS8890_CBD_ENDPOINT_DUMP,
};

struct exynos8890_cbd_endpoint_config {
	const char *name;
	enum exynos8890_cbd_endpoint_type type;
	u8 sipc_channel;
	umode_t mode;
};

static const struct exynos8890_cbd_endpoint_config exynos8890_cbd_configs[] = {
	{ "umts_boot0", EXYNOS8890_CBD_ENDPOINT_BOOT, 215, 0660 },
	{ "umts_ramdump0", EXYNOS8890_CBD_ENDPOINT_DUMP, 225, 0660 },
};

struct exynos8890_cbd_endpoint {
	struct exynos8890_cbd *cbd;
	struct miscdevice miscdev;
	struct exynos8890_sipc_channel *channel;
	struct mutex lock;
	wait_queue_head_t read_wait;
	struct sk_buff_head rx_queue;
	const struct exynos8890_cbd_endpoint_config *config;
	atomic_t opens;
	bool stopping;
};

struct exynos8890_cbd_file {
	struct exynos8890_cbd_endpoint *endpoint;
	loff_t offset;
	size_t dump_size;
	bool full_dump;
};

/* Native and compat cbd both use two 32-bit words for command 0x41. */
struct exynos8890_cbd_legacy_shmem_info {
	u32 base;
	u32 size;
};

struct exynos8890_cbd {
	struct device *dev;
	struct exynos8890_cpctl *cpctl;
	struct exynos8890_sipc *sipc;
	struct notifier_block cp_notifier;
	struct mutex ioctl_lock;
	struct rw_semaphore operation_sem;
	struct kref refcount;
	struct exynos8890_cbd_endpoint *endpoints;
	unsigned int endpoint_count;
	bool cp_notifier_registered;
	bool sipc_client_registered;
	bool registered;
	bool sim_state_known;
	bool sim_online;
};

static void exynos8890_cbd_free(struct kref *refcount)
{
	struct exynos8890_cbd *cbd =
		container_of(refcount, struct exynos8890_cbd, refcount);

	kfree(cbd->endpoints);
	kfree(cbd);
}

static struct exynos8890_cbd_endpoint *
exynos8890_cbd_find_endpoint(struct exynos8890_cbd *cbd, u8 channel)
{
	unsigned int i;

	for (i = 0; i < cbd->endpoint_count; i++)
		if (cbd->endpoints[i].config->sipc_channel == channel)
			return &cbd->endpoints[i];
	return NULL;
}

static void exynos8890_cbd_unregister_endpoints(struct exynos8890_cbd *cbd)
{
	unsigned int i;

	WRITE_ONCE(cbd->registered, false);
	for (i = 0; i < cbd->endpoint_count; i++) {
		struct exynos8890_cbd_endpoint *endpoint = &cbd->endpoints[i];

		mutex_lock(&endpoint->lock);
		endpoint->stopping = true;
		mutex_unlock(&endpoint->lock);
		wake_up_interruptible_all(&endpoint->read_wait);
	}

	for (i = cbd->endpoint_count; i-- > 0;)
		misc_deregister(&cbd->endpoints[i].miscdev);
}

static int exynos8890_cbd_register_endpoints(struct exynos8890_cbd *cbd)
{
	unsigned int i;
	int ret;

	for (i = 0; i < cbd->endpoint_count; i++) {
		struct exynos8890_cbd_endpoint *endpoint = &cbd->endpoints[i];

		ret = misc_register(&endpoint->miscdev);
		if (ret)
			goto unwind;
	}
	WRITE_ONCE(cbd->registered, true);
	return 0;

unwind:
	while (i--)
		misc_deregister(&cbd->endpoints[i].miscdev);
	return ret;
}

static struct exynos8890_cbd_endpoint *
exynos8890_cbd_endpoint_from_file(struct file *file)
{
	struct exynos8890_cbd_file *cfile = file->private_data;

	return cfile ? cfile->endpoint : NULL;
}

static int exynos8890_cbd_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct exynos8890_cbd_endpoint *endpoint =
		container_of(miscdev, struct exynos8890_cbd_endpoint, miscdev);
	struct exynos8890_cbd_file *cfile;

	if (!kref_get_unless_zero(&endpoint->cbd->refcount))
		return -ENODEV;

	mutex_lock(&endpoint->lock);
	if (endpoint->stopping || !READ_ONCE(endpoint->cbd->registered)) {
		mutex_unlock(&endpoint->lock);
		kref_put(&endpoint->cbd->refcount, exynos8890_cbd_free);
		return -ENODEV;
	}
	atomic_inc(&endpoint->opens);
	mutex_unlock(&endpoint->lock);

	cfile = kzalloc(sizeof(*cfile), GFP_KERNEL);
	if (!cfile) {
		atomic_dec(&endpoint->opens);
		kref_put(&endpoint->cbd->refcount, exynos8890_cbd_free);
		return -ENOMEM;
	}
	cfile->endpoint = endpoint;
	file->private_data = cfile;
	return nonseekable_open(inode, file);
}

static int exynos8890_cbd_release(struct inode *inode, struct file *file)
{
	struct exynos8890_cbd_file *cfile = file->private_data;
	struct exynos8890_cbd *cbd;

	if (!cfile)
		return 0;
	cbd = cfile->endpoint->cbd;
	atomic_dec(&cfile->endpoint->opens);
	kfree(cfile);
	file->private_data = NULL;
	kref_put(&cbd->refcount, exynos8890_cbd_free);
	return 0;
}

static ssize_t exynos8890_cbd_read_queue(struct file *file,
					struct exynos8890_cbd_endpoint *endpoint,
					char __user *buffer, size_t length)
{
	struct sk_buff *skb;
	size_t copied;
	int ret;

	for (;;) {
		skb = skb_dequeue(&endpoint->rx_queue);
		if (skb)
			break;
		if (READ_ONCE(endpoint->stopping))
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(endpoint->read_wait,
			READ_ONCE(endpoint->stopping) ||
			!skb_queue_empty(&endpoint->rx_queue));
		if (ret)
			return ret;
	}

	copied = min(length, skb->len);
	if (copy_to_user(buffer, skb->data, copied)) {
		skb_queue_head(&endpoint->rx_queue, skb);
		return -EFAULT;
	}
	if (copied != skb->len) {
		skb_pull(skb, copied);
		skb_queue_head(&endpoint->rx_queue, skb);
	} else {
		kfree_skb(skb);
	}
	return copied;
}

static ssize_t exynos8890_cbd_read(struct file *file, char __user *buffer,
				  size_t length, loff_t *offset)
{
	struct exynos8890_cbd_file *cfile = file->private_data;
	struct exynos8890_cbd_endpoint *endpoint;
	void *data;
	ssize_t ret;

	if (!cfile || !length)
		return cfile ? 0 : -ENODEV;
	endpoint = cfile->endpoint;
	if (READ_ONCE(endpoint->stopping))
		return -ENODEV;

	if (!cfile->full_dump)
		return exynos8890_cbd_read_queue(file, endpoint, buffer, length);

	if (cfile->offset >= cfile->dump_size)
		return 0;
	length = min_t(size_t, length, EXYNOS8890_CBD_MAX_IO);
	length = min_t(size_t, length, cfile->dump_size - cfile->offset);
	data = kvmalloc(length, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	down_read(&endpoint->cbd->operation_sem);
	if (READ_ONCE(endpoint->stopping)) {
		ret = -ENODEV;
		goto unlock_dump;
	}
	ret = exynos8890_sipc_dump_read(endpoint->cbd->sipc, data, length,
					cfile->offset);
	if (ret > 0) {
		if (copy_to_user(buffer, data, ret))
			ret = -EFAULT;
		else
			cfile->offset += ret;
	}
unlock_dump:
	up_read(&endpoint->cbd->operation_sem);
	kvfree(data);
	return ret;
}

static ssize_t exynos8890_cbd_write(struct file *file,
				   const char __user *buffer,
				   size_t length, loff_t *offset)
{
	struct exynos8890_cbd_endpoint *endpoint =
		exynos8890_cbd_endpoint_from_file(file);
	struct sk_buff *skb;
	void *data;
	ssize_t ret;

	if (!endpoint || READ_ONCE(endpoint->stopping))
		return -ENODEV;
	if (!length)
		return 0;
	if (length > EXYNOS8890_CBD_MAX_IO)
		return -EMSGSIZE;

	down_read(&endpoint->cbd->operation_sem);
	if (READ_ONCE(endpoint->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (endpoint->config->type == EXYNOS8890_CBD_ENDPOINT_BOOT) {
		data = memdup_user(buffer, length);
		if (IS_ERR(data)) {
			ret = PTR_ERR(data);
			goto out;
		}
		ret = exynos8890_sipc_boot_write(endpoint->cbd->sipc, data,
						 length, *offset);
		kfree(data);
		if (ret > 0)
			*offset += ret;
		goto out;
	}

	skb = alloc_skb(length, GFP_KERNEL);
	if (!skb) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(skb_put(skb, length), buffer, length)) {
		kfree_skb(skb);
		ret = -EFAULT;
		goto out;
	}
	ret = exynos8890_sipc_send(endpoint->channel, skb,
				   file->f_flags & O_NONBLOCK);
	if (ret < 0) {
		kfree_skb(skb);
		goto out;
	}
	/* The transport owns skb after a successful enqueue. */
	ret = length;
out:
	up_read(&endpoint->cbd->operation_sem);
	return ret;
}

static int exynos8890_cbd_legacy_state(enum exynos8890_cp_state state)
{
	switch (state) {
	case EXYNOS8890_CP_OFFLINE:
	case EXYNOS8890_CP_POWERING:
	case EXYNOS8890_CP_STOPPING:
		return EXYNOS8890_CBD_STATE_OFFLINE;
	case EXYNOS8890_CP_CRASH_RESET:
		return EXYNOS8890_CBD_STATE_CRASH_RESET;
	case EXYNOS8890_CP_CRASH_EXIT:
	case EXYNOS8890_CP_DUMPING:
	case EXYNOS8890_CP_FAULTED:
		return EXYNOS8890_CBD_STATE_CRASH_EXIT;
	case EXYNOS8890_CP_BOOTING:
		return EXYNOS8890_CBD_STATE_BOOTING;
	case EXYNOS8890_CP_ONLINE:
		return EXYNOS8890_CBD_STATE_ONLINE;
	case EXYNOS8890_CP_CRASH_WATCHDOG:
		return EXYNOS8890_CBD_STATE_CRASH_WATCHDOG;
	}
	return EXYNOS8890_CBD_STATE_CRASH_EXIT;
}

static __poll_t exynos8890_cbd_poll(struct file *file, poll_table *wait)
{
	struct exynos8890_cbd_endpoint *endpoint =
		exynos8890_cbd_endpoint_from_file(file);
	struct exynos8890_cbd_file *cfile = file->private_data;
	struct exynos8890_cp_status status;
	__poll_t mask = 0;

	if (!endpoint)
		return EPOLLERR | EPOLLHUP;
	poll_wait(file, &endpoint->read_wait, wait);
	if (READ_ONCE(endpoint->stopping))
		return EPOLLERR | EPOLLHUP;
	if (!skb_queue_empty(&endpoint->rx_queue) ||
	    (cfile->full_dump && cfile->offset < cfile->dump_size))
		mask |= EPOLLIN | EPOLLRDNORM;
	down_read(&endpoint->cbd->operation_sem);
	if (READ_ONCE(endpoint->stopping)) {
		up_read(&endpoint->cbd->operation_sem);
		return EPOLLERR | EPOLLHUP;
	}
	/*
	 * Legacy misc_poll() reports POLLHUP on the boot iodev whenever CP is
	 * in a crash/reset state, which is how cbd notices an unrecoverable
	 * CP without having to poll IOCTL_MODEM_STATUS. Preserve that for the
	 * umts_boot0-equivalent endpoint; the dump endpoint keeps behaving
	 * like legacy's IPC_DUMP case, which only reports readable data.
	 */
	if (endpoint->config->type == EXYNOS8890_CBD_ENDPOINT_BOOT &&
	    !exynos8890_cpctl_get_status(endpoint->cbd->cpctl, &status)) {
		int legacy_state = exynos8890_cbd_legacy_state(status.state);

		if (legacy_state == EXYNOS8890_CBD_STATE_CRASH_RESET ||
		    legacy_state == EXYNOS8890_CBD_STATE_CRASH_EXIT ||
		    legacy_state == EXYNOS8890_CBD_STATE_CRASH_WATCHDOG) {
			up_read(&endpoint->cbd->operation_sem);
			return EPOLLHUP;
		}
	}
	if (exynos8890_sipc_link_state(endpoint->cbd->sipc) !=
	    EXYNOS8890_SIPC_LINK_FAULTED)
		mask |= EPOLLOUT | EPOLLWRNORM;
	else
		mask |= EPOLLERR;
	up_read(&endpoint->cbd->operation_sem);
	return mask;
}

static long exynos8890_cbd_ioctl_power(struct exynos8890_cbd *cbd,
				      unsigned int command)
{
	switch (command) {
	case EXYNOS8890_CBD_IOCTL_MODEM_ON:
		return exynos8890_cpctl_power_on(cbd->cpctl);
	case EXYNOS8890_CBD_IOCTL_MODEM_OFF:
		exynos8890_sipc_stop(cbd->sipc);
		return exynos8890_cpctl_power_off(cbd->cpctl);
	case EXYNOS8890_CBD_IOCTL_MODEM_RESET:
		exynos8890_sipc_stop(cbd->sipc);
		return exynos8890_cpctl_reset(cbd->cpctl);
	default:
		return -ENOTTY;
	}
}

static long exynos8890_cbd_ioctl_boot(struct exynos8890_cbd *cbd,
				     unsigned int command,
				     unsigned long argument)
{
	struct exynos8890_cbd_firmware firmware;
	struct exynos8890_cp_boot_image image = {};
	struct exynos8890_cp_shmem_layout layout;
	void *data;
	size_t region_size;
	int ret;

	switch (command) {
	case EXYNOS8890_CBD_IOCTL_BOOT_ON:
		ret = exynos8890_sipc_set_link_state(cbd->sipc,
						      EXYNOS8890_SIPC_LINK_BOOT);
		return ret ?: exynos8890_cpctl_release(cbd->cpctl);
	case EXYNOS8890_CBD_IOCTL_BOOT_OFF:
		ret = exynos8890_sipc_start(cbd->sipc);
		if (ret)
			return ret;
		ret = exynos8890_sipc_set_link_state(cbd->sipc,
						      EXYNOS8890_SIPC_LINK_IPC);
		return ret ?: exynos8890_cpctl_complete_boot(cbd->cpctl);
	case EXYNOS8890_CBD_IOCTL_BOOT_DONE:
		return exynos8890_cpctl_complete_boot(cbd->cpctl);
	case EXYNOS8890_CBD_IOCTL_DL_START:
		ret = exynos8890_sipc_reset(cbd->sipc);
		return ret ?: exynos8890_sipc_set_link_state(cbd->sipc,
						EXYNOS8890_SIPC_LINK_BOOT);
	case EXYNOS8890_CBD_IOCTL_FW_UPDATE:
		return exynos8890_sipc_start(cbd->sipc);
	case EXYNOS8890_CBD_IOCTL_XMIT_BOOT:
		break;
	default:
		return -ENOTTY;
	}

	if (copy_from_user(&firmware, (void __user *)argument, sizeof(firmware)))
		return -EFAULT;
	if (!firmware.binary && firmware.length)
		return -EINVAL;
	ret = exynos8890_cpctl_get_shmem_layout(cbd->cpctl, &layout);
	if (ret)
		return ret;
	region_size = firmware.mode ? layout.size : layout.boot_size;
	if (!region_size || firmware.size > region_size ||
	    firmware.length > region_size ||
	    firmware.main_offset > region_size - firmware.length)
		return -EINVAL;
	data = vmemdup_user(u64_to_user_ptr(firmware.binary), firmware.length);
	if (IS_ERR(data))
		return PTR_ERR(data);
	image.data = data;
	image.size = firmware.length;
	image.boot_offset = firmware.boot_offset;
	image.main_offset = firmware.main_offset;
	image.mode = firmware.mode ? EXYNOS8890_CP_BOOT_DUMP :
				     EXYNOS8890_CP_BOOT_NORMAL;
	ret = exynos8890_cpctl_prepare_boot(cbd->cpctl, &image);
	kvfree(data);
	return ret;
}

static long exynos8890_cbd_ioctl_security(struct exynos8890_cbd *cbd,
					 unsigned long argument)
{
	struct exynos8890_cbd_security_request user_request;
	struct exynos8890_cp_security_request request = {};
	struct exynos8890_cp_shmem_layout layout;
	phys_addr_t ipc_base;
	int ret;

	if (copy_from_user(&user_request, (void __user *)argument,
			   sizeof(user_request)))
		return -EFAULT;
	if (user_request.mode > EXYNOS8890_CBD_BOOT_REINIT)
		return -EINVAL;
	request.mode = user_request.mode;
	switch (user_request.mode) {
	case EXYNOS8890_CBD_BOOT_NORMAL:
		request.boot_size = user_request.boot_size;
		request.main_size = user_request.main_size;
		break;
	case EXYNOS8890_CBD_BOOT_DUMP:
		ret = exynos8890_cpctl_get_shmem_layout(cbd->cpctl, &layout);
		if (ret)
			return ret;
		if (check_add_overflow(layout.base, (phys_addr_t)layout.ipc_offset,
				       &ipc_base) || upper_32_bits(ipc_base))
			return -EOVERFLOW;
		request.boot_size = user_request.boot_size;
		request.main_size = lower_32_bits(ipc_base);
		break;
	case EXYNOS8890_CBD_BOOT_REINIT:
		break;
	}
	return exynos8890_cpctl_security_request(cbd->cpctl, &request);
}

static long exynos8890_cbd_ioctl_srinfo(struct exynos8890_cbd *cbd,
				       unsigned int command,
				       unsigned long argument)
{
	struct exynos8890_cbd_srinfo __user *info = (void __user *)argument;
	u32 count;
	void *data;
	int ret;

	if (get_user(count, &info->size))
		return -EFAULT;
	count = min_t(u32, count, EXYNOS8890_CBD_SRINFO_SIZE);
	data = kvmalloc(count ?: 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (command == EXYNOS8890_CBD_IOCTL_GET_SRINFO) {
		ret = exynos8890_sipc_get_srinfo(cbd->sipc, data, count);
		if (!ret && (put_user(count, &info->size) ||
			     copy_to_user(info->data, data, count)))
			ret = -EFAULT;
	} else {
		if (copy_from_user(data, info->data, count))
			ret = -EFAULT;
		else
			ret = exynos8890_sipc_set_srinfo(cbd->sipc, data, count);
	}
	kvfree(data);
	return ret;
}

static int exynos8890_cbd_put_ulong(unsigned long argument,
				   unsigned long value)
{
#ifdef CONFIG_COMPAT
	if (in_compat_syscall()) {
		compat_ulong_t compat_value = value;

		if (value != compat_value)
			return -EOVERFLOW;
		return copy_to_user(compat_ptr(argument), &compat_value,
				    sizeof(compat_value)) ? -EFAULT : 0;
	}
#endif
	return copy_to_user((void __user *)argument, &value, sizeof(value)) ?
		-EFAULT : 0;
}

static long exynos8890_cbd_ioctl_status(struct exynos8890_cbd *cbd)
{
	struct exynos8890_cp_status status;
	int ret;

	ret = exynos8890_cpctl_get_status(cbd->cpctl, &status);
	if (ret)
		return ret;

	/*
	 * Legacy one-shot SIM attach/detach reporting: RIL polls
	 * IOCTL_MODEM_STATUS to learn about SIM hot swaps.  Report a
	 * transition exactly once (edge-triggered) and otherwise fall
	 * back to the normal CP state translation, matching
	 * modem_io_device.c's mc->sim_state.changed handling. Caller
	 * holds cbd->ioctl_lock, so this is race-free against itself.
	 */
	if (!cbd->sim_state_known) {
		cbd->sim_state_known = true;
		cbd->sim_online = status.sim_online;
	} else if (status.sim_online != cbd->sim_online) {
		cbd->sim_online = status.sim_online;
		return status.sim_online ? EXYNOS8890_CBD_STATE_SIM_ATTACH :
					   EXYNOS8890_CBD_STATE_SIM_DETACH;
	}

	return exynos8890_cbd_legacy_state(status.state);
}

static long exynos8890_cbd_ioctl_full_dump(struct file *file,
					  unsigned long argument)
{
	struct exynos8890_cbd_file *cfile = file->private_data;
	struct exynos8890_cp_shmem_layout layout;
	unsigned long size;
	int ret;

	ret = exynos8890_cpctl_get_shmem_layout(cfile->endpoint->cbd->cpctl,
						&layout);
	if (ret)
		return ret;
	size = layout.size;
	ret = exynos8890_cbd_put_ulong(argument, size);
	if (ret)
		return ret;
	cfile->offset = 0;
	cfile->dump_size = layout.size;
	cfile->full_dump = true;
	return 0;
}

static long exynos8890_cbd_ioctl_log_dump(struct file *file,
					 unsigned long argument)
{
	struct exynos8890_cbd_endpoint *endpoint =
		exynos8890_cbd_endpoint_from_file(file);
	struct exynos8890_sipc_stats stats;
	struct sk_buff *skb;
	unsigned long size = sizeof(stats);
	int ret;

	ret = exynos8890_sipc_get_stats(endpoint->cbd->sipc, &stats);
	if (ret)
		return ret;
	ret = exynos8890_cbd_put_ulong(argument, size);
	if (ret)
		return ret;
	if (skb_queue_len(&endpoint->rx_queue) >= EXYNOS8890_CBD_MAX_RX_QLEN)
		return -ENOSPC;
	skb = alloc_skb(sizeof(stats), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	memcpy(skb_put(skb, sizeof(stats)), &stats, sizeof(stats));
	skb_queue_tail(&endpoint->rx_queue, skb);
	wake_up_interruptible(&endpoint->read_wait);
	return 0;
}

static long exynos8890_cbd_ioctl(struct file *file, unsigned int command,
				unsigned long argument)
{
	struct exynos8890_cbd_endpoint *endpoint =
		exynos8890_cbd_endpoint_from_file(file);
	struct exynos8890_cbd *cbd;
	struct exynos8890_cp_shmem_layout layout;
	struct exynos8890_cbd_legacy_shmem_info info = {};
	int tx_link;
	long ret;

	if (!endpoint)
		return -ENODEV;
	cbd = endpoint->cbd;
	mutex_lock(&cbd->ioctl_lock);
	if (!READ_ONCE(cbd->registered) || READ_ONCE(endpoint->stopping)) {
		ret = -ENODEV;
		goto out;
	}

	switch (command) {
	case EXYNOS8890_CBD_IOCTL_MODEM_ON:
	case EXYNOS8890_CBD_IOCTL_MODEM_OFF:
	case EXYNOS8890_CBD_IOCTL_MODEM_RESET:
		ret = exynos8890_cbd_ioctl_power(cbd, command);
		break;
	case EXYNOS8890_CBD_IOCTL_BOOT_ON:
	case EXYNOS8890_CBD_IOCTL_BOOT_OFF:
	case EXYNOS8890_CBD_IOCTL_BOOT_DONE:
	case EXYNOS8890_CBD_IOCTL_DL_START:
	case EXYNOS8890_CBD_IOCTL_FW_UPDATE:
	case EXYNOS8890_CBD_IOCTL_XMIT_BOOT:
		ret = exynos8890_cbd_ioctl_boot(cbd, command, argument);
		break;
	case EXYNOS8890_CBD_IOCTL_MODEM_STATUS:
		ret = exynos8890_cbd_ioctl_status(cbd);
		break;
	case EXYNOS8890_CBD_IOCTL_PROTOCOL_SUSPEND:
	case EXYNOS8890_CBD_IOCTL_PROTOCOL_RESUME:
		ret = exynos8890_cpctl_set_protocol_suspended(cbd->cpctl,
			command == EXYNOS8890_CBD_IOCTL_PROTOCOL_SUSPEND);
		break;
	case EXYNOS8890_CBD_IOCTL_NET_SUSPEND:
	case EXYNOS8890_CBD_IOCTL_NET_RESUME:
		ret = exynos8890_cpctl_set_network_suspended(cbd->cpctl,
			command == EXYNOS8890_CBD_IOCTL_NET_SUSPEND);
		break;
	case EXYNOS8890_CBD_IOCTL_DUMP_START:
	case EXYNOS8890_CBD_IOCTL_RAMDUMP_START:
		ret = exynos8890_sipc_set_link_state(cbd->sipc,
						      EXYNOS8890_SIPC_LINK_DUMP);
		if (!ret)
			ret = exynos8890_cpctl_start_dump(cbd->cpctl);
		break;
	case EXYNOS8890_CBD_IOCTL_RAMDUMP_STOP:
		ret = exynos8890_cpctl_finish_dump(cbd->cpctl);
		break;
	case EXYNOS8890_CBD_IOCTL_FORCE_CRASH:
		ret = exynos8890_cpctl_force_crash(cbd->cpctl);
		break;
	case EXYNOS8890_CBD_IOCTL_CP_UPLOAD: {
		/*
		 * Legacy semantics: cbd asks for a full AP panic (not just a
		 * CP-side crash) so the bootloader/coredump path collects a
		 * joint AP+CP post-mortem. Copy the complete payload before
		 * doing anything else, and never hand caller-controlled bytes
		 * to panic() as a format string.
		 */
		static const char cp_crash_tag[] = "CP Crash ";
		char crash_info[sizeof(cp_crash_tag) - 1 + 512 + 1];

		memcpy(crash_info, cp_crash_tag, sizeof(cp_crash_tag) - 1);
		if (argument) {
			if (copy_from_user(crash_info + sizeof(cp_crash_tag) - 1,
					   (void __user *)argument, 512)) {
				ret = -EFAULT;
				break;
			}
		} else {
			memset(crash_info + sizeof(cp_crash_tag) - 1, 0, 512);
		}
		crash_info[sizeof(crash_info) - 1] = '\0';
		dev_emerg(cbd->dev, "CP upload requested: %s\n", crash_info);
		panic("%s", crash_info);
		break;
	}
	case EXYNOS8890_CBD_IOCTL_LINK_RESET:
		ret = exynos8890_sipc_reset(cbd->sipc);
		break;
	case EXYNOS8890_CBD_IOCTL_SET_TX_LINK:
		if (copy_from_user(&tx_link, (void __user *)argument,
				   sizeof(tx_link)))
			ret = -EFAULT;
		else
			ret = tx_link == EXYNOS8890_CBD_SHMEM_LINK ? 0 : -ENODEV;
		break;
	case EXYNOS8890_CBD_IOCTL_GET_SHMEM_INFO:
		ret = exynos8890_cpctl_get_shmem_layout(cbd->cpctl, &layout);
		if (!ret) {
			if (upper_32_bits(layout.base)) {
				ret = -EOVERFLOW;
				break;
			}
			info.base = lower_32_bits(layout.base);
			info.size = layout.size;
			if (copy_to_user((void __user *)argument, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;
	case EXYNOS8890_CBD_IOCTL_GET_SRINFO:
	case EXYNOS8890_CBD_IOCTL_SET_SRINFO:
		ret = exynos8890_cbd_ioctl_srinfo(cbd, command, argument);
		break;
	case EXYNOS8890_CBD_IOCTL_LOG_DUMP:
		ret = exynos8890_cbd_ioctl_log_dump(file, argument);
		break;
	case EXYNOS8890_CBD_IOCTL_SECURITY_REQ:
		ret = exynos8890_cbd_ioctl_security(cbd, argument);
		break;
	case EXYNOS8890_CBD_IOCTL_SHMEM_FULL_DUMP:
		ret = exynos8890_cbd_ioctl_full_dump(file, argument);
		break;
	case EXYNOS8890_CBD_IOCTL_DUMP_UPDATE:
		ret = exynos8890_sipc_poll_rx(cbd->sipc, 64);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
out:
	mutex_unlock(&cbd->ioctl_lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long exynos8890_cbd_compat_ioctl(struct file *file,
				       unsigned int command,
				       unsigned long argument)
{
	return exynos8890_cbd_ioctl(file, command,
				    (unsigned long)compat_ptr(argument));
}
#endif

static const struct file_operations exynos8890_cbd_fops = {
	.owner = THIS_MODULE,
	.open = exynos8890_cbd_open,
	.release = exynos8890_cbd_release,
	.read = exynos8890_cbd_read,
	.write = exynos8890_cbd_write,
	.poll = exynos8890_cbd_poll,
	.unlocked_ioctl = exynos8890_cbd_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = exynos8890_cbd_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static int exynos8890_cbd_cp_event(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct exynos8890_cbd *cbd =
		container_of(nb, struct exynos8890_cbd, cp_notifier);
	unsigned int i;

	if (!READ_ONCE(cbd->registered))
		return NOTIFY_DONE;
	for (i = 0; i < cbd->endpoint_count; i++)
		wake_up_interruptible_all(&cbd->endpoints[i].read_wait);
	return NOTIFY_OK;
}

static void exynos8890_cbd_link_state_changed(void *data,
				     enum exynos8890_sipc_link_state state)
{
	struct exynos8890_cbd *cbd = data;
	unsigned int i;

	if (!READ_ONCE(cbd->registered))
		return;
	for (i = 0; i < cbd->endpoint_count; i++)
		wake_up_interruptible_all(&cbd->endpoints[i].read_wait);
}

static void exynos8890_cbd_rx_frame(void *data, u8 channel,
				   struct sk_buff *skb)
{
	struct exynos8890_cbd *cbd = data;
	struct exynos8890_cbd_endpoint *endpoint;

	if (!READ_ONCE(cbd->registered)) {
		kfree_skb(skb);
		return;
	}
	endpoint = exynos8890_cbd_find_endpoint(cbd, channel);
	if (!endpoint || READ_ONCE(endpoint->stopping)) {
		kfree_skb(skb);
		return;
	}
	if (endpoint->config->type != EXYNOS8890_CBD_ENDPOINT_DUMP &&
	    skb_queue_len(&endpoint->rx_queue) >= EXYNOS8890_CBD_MAX_RX_QLEN) {
		kfree_skb(skb);
		return;
	}
	skb_queue_tail(&endpoint->rx_queue, skb);
	wake_up_interruptible(&endpoint->read_wait);
}

static void exynos8890_cbd_tx_space(void *data, u8 channel)
{
	struct exynos8890_cbd *cbd = data;
	struct exynos8890_cbd_endpoint *endpoint;

	if (!READ_ONCE(cbd->registered))
		return;
	endpoint = exynos8890_cbd_find_endpoint(cbd, channel);
	if (endpoint)
		wake_up_interruptible(&endpoint->read_wait);
}

static void exynos8890_cbd_transport_fault(void *data, int error)
{
	struct exynos8890_cbd *cbd = data;
	unsigned int i;

	if (!READ_ONCE(cbd->registered))
		return;
	for (i = 0; i < cbd->endpoint_count; i++)
		wake_up_interruptible_all(&cbd->endpoints[i].read_wait);
}

static const struct exynos8890_sipc_client_ops exynos8890_cbd_sipc_ops = {
	.link_state_changed = exynos8890_cbd_link_state_changed,
	.rx_frame = exynos8890_cbd_rx_frame,
	.tx_space_available = exynos8890_cbd_tx_space,
	.transport_fault = exynos8890_cbd_transport_fault,
};

static int exynos8890_cbd_probe(struct platform_device *pdev)
{
	struct exynos8890_cbd *cbd;
	unsigned int i;
	int ret;

	static_assert(sizeof(struct exynos8890_cbd_firmware) == 28);
	static_assert(sizeof(struct exynos8890_cbd_security_request) == 16);
	static_assert(offsetof(struct exynos8890_cbd_srinfo, data) == 4);
	static_assert(sizeof(struct exynos8890_cbd_legacy_shmem_info) == 8);
	static_assert(EXYNOS8890_CBD_IOCTL_MODEM_ON == 0x6f19);
	static_assert(EXYNOS8890_CBD_IOCTL_RAMDUMP_STOP == 0x6fcf);

	cbd = kzalloc(sizeof(*cbd), GFP_KERNEL);
	if (!cbd)
		return -ENOMEM;
	cbd->dev = &pdev->dev;
	mutex_init(&cbd->ioctl_lock);
	init_rwsem(&cbd->operation_sem);
	kref_init(&cbd->refcount);
	cbd->endpoint_count = ARRAY_SIZE(exynos8890_cbd_configs);
	cbd->endpoints = kcalloc(cbd->endpoint_count, sizeof(*cbd->endpoints),
				 GFP_KERNEL);
	if (!cbd->endpoints) {
		ret = -ENOMEM;
		goto put_cbd;
	}

	cbd->cpctl = exynos8890_cpctl_get(&pdev->dev);
	if (IS_ERR(cbd->cpctl)) {
		ret = PTR_ERR(cbd->cpctl);
		cbd->cpctl = NULL;
		goto put_cbd;
	}
	cbd->sipc = exynos8890_sipc_get(&pdev->dev);
	if (IS_ERR(cbd->sipc)) {
		ret = PTR_ERR(cbd->sipc);
		cbd->sipc = NULL;
		goto put_cpctl;
	}

	for (i = 0; i < cbd->endpoint_count; i++) {
		struct exynos8890_cbd_endpoint *endpoint = &cbd->endpoints[i];

		endpoint->cbd = cbd;
		endpoint->config = &exynos8890_cbd_configs[i];
		mutex_init(&endpoint->lock);
		init_waitqueue_head(&endpoint->read_wait);
		skb_queue_head_init(&endpoint->rx_queue);
		atomic_set(&endpoint->opens, 0);
		endpoint->miscdev.minor = MISC_DYNAMIC_MINOR;
		endpoint->miscdev.name = endpoint->config->name;
		endpoint->miscdev.fops = &exynos8890_cbd_fops;
		endpoint->miscdev.parent = &pdev->dev;
		endpoint->miscdev.mode = endpoint->config->mode;
		endpoint->channel = exynos8890_sipc_channel_get(cbd->sipc,
						 endpoint->config->sipc_channel);
		if (IS_ERR(endpoint->channel)) {
			ret = PTR_ERR(endpoint->channel);
			endpoint->channel = NULL;
			goto put_channels;
		}
	}

	cbd->cp_notifier.notifier_call = exynos8890_cbd_cp_event;
	ret = exynos8890_cpctl_register_notifier(cbd->cpctl, &cbd->cp_notifier);
	if (ret)
		goto put_channels;
	cbd->cp_notifier_registered = true;
	ret = exynos8890_sipc_register_client(cbd->sipc,
					      &exynos8890_cbd_sipc_ops, cbd);
	if (ret)
		goto unregister_notifier;
	cbd->sipc_client_registered = true;
	ret = exynos8890_cbd_register_endpoints(cbd);
	if (ret)
		goto unregister_client;

	platform_set_drvdata(pdev, cbd);
	return 0;

unregister_client:
	exynos8890_sipc_unregister_client(cbd->sipc, &exynos8890_cbd_sipc_ops,
					  cbd);
	cbd->sipc_client_registered = false;
unregister_notifier:
	exynos8890_cpctl_unregister_notifier(cbd->cpctl, &cbd->cp_notifier);
	cbd->cp_notifier_registered = false;
put_channels:
	while (i--)
		exynos8890_sipc_channel_put(cbd->endpoints[i].channel);
	exynos8890_sipc_put(cbd->sipc);
	cbd->sipc = NULL;
put_cpctl:
	exynos8890_cpctl_put(cbd->cpctl);
	cbd->cpctl = NULL;
put_cbd:
	kref_put(&cbd->refcount, exynos8890_cbd_free);
	return ret;
}

static void exynos8890_cbd_remove(struct platform_device *pdev)
{
	struct exynos8890_cbd *cbd = platform_get_drvdata(pdev);
	unsigned int i;

	if (!cbd)
		return;
	platform_set_drvdata(pdev, NULL);
	exynos8890_cbd_unregister_endpoints(cbd);

	/* Drain an ioctl which passed the registered check before revocation. */
	mutex_lock(&cbd->ioctl_lock);
	mutex_unlock(&cbd->ioctl_lock);
	if (cbd->sipc_client_registered) {
		exynos8890_sipc_unregister_client(cbd->sipc,
						  &exynos8890_cbd_sipc_ops, cbd);
		cbd->sipc_client_registered = false;
	}
	if (cbd->cp_notifier_registered) {
		exynos8890_cpctl_unregister_notifier(cbd->cpctl,
						    &cbd->cp_notifier);
		cbd->cp_notifier_registered = false;
	}
	down_write(&cbd->operation_sem);
	for (i = 0; i < cbd->endpoint_count; i++) {
		skb_queue_purge(&cbd->endpoints[i].rx_queue);
		if (cbd->endpoints[i].channel) {
			exynos8890_sipc_channel_put(cbd->endpoints[i].channel);
			cbd->endpoints[i].channel = NULL;
		}
	}
	exynos8890_sipc_put(cbd->sipc);
	cbd->sipc = NULL;
	exynos8890_cpctl_put(cbd->cpctl);
	cbd->cpctl = NULL;
	up_write(&cbd->operation_sem);
	kref_put(&cbd->refcount, exynos8890_cbd_free);
}

static const struct of_device_id exynos8890_cbd_of_match[] = {
	{ .compatible = "samsung,exynos8890-cbd-compat" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cbd_of_match);

static struct platform_driver exynos8890_cbd_driver = {
	.probe = exynos8890_cbd_probe,
	.remove = exynos8890_cbd_remove,
	.driver = {
		.name = "exynos8890-cbd-compat",
		.of_match_table = exynos8890_cbd_of_match,
	},
};
module_platform_driver(exynos8890_cbd_driver);

MODULE_DESCRIPTION("Exynos8890 legacy Samsung cbd compatibility");
MODULE_LICENSE("GPL");
