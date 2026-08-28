// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Validity/Synaptics VFS7XXX fingerprint sensor driver.
 *
 * Ported from vendor's drivers/fingerprint/vfs7xxx.c (Cronos_8890), which
 * also supports a TEE-routed "secure" mode (ENABLE_SENSORS_FPRINT_SECURE)
 * where actual scan matching happens in a Trusted Application instead of
 * through this driver's raw SPI ioctls. That mode is never defined here -
 * this port has no TEE - so the raw-SPI, non-secure code path is what's
 * kept; the secure-only ioctls/fields/exynos_smc() PM calls vendor guards
 * behind that macro are dropped rather than carried as permanently-off
 * ifdef branches. GPIO handling is modernized to gpiod (vendor used the
 * legacy gpio_* API); wake_lock/wake_unlock (removed from mainline years
 * ago, and only used in the secure-mode path anyway) is not needed since
 * that path is gone.
 *
 * Also dropped for this first bring-up, each independent of the others and
 * addable later without touching what's here: the shared fingerprint_sysfs
 * class (drivers/fingerprint/fingerprint_sysfs.c in vendor - optional extra
 * /sys/class/fingerprint/ attributes, this driver's own /dev/vfsspi node
 * works without it), the multi-retry SPI-based sensor type/ID readback
 * (vfsspi_type_check() - replaced with the same simple vendor-detect GPIO
 * read the stock-kernel boot log already confirmed working), ET320
 * dualization (this unit's vendor-detect GPIO already confirmed VFS7XXX),
 * and the idle/sleep pinctrl state switching around IRQ enable/disable
 * (power optimization, not a correctness requirement - the DRDY pin stays
 * in one pinctrl state here).
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "vfs7xxx.h"

#define VFSSPI_DEV_NAME		"validity_fingerprint"
#define VFSSPI_DEFAULT_BUFFER_SIZE	(4096 * 6)
#define VFSSPI_MAX_BAUD_RATE	13000000
#define VFSSPI_SLOW_BAUD_RATE	13000000
#define VFSSPI_BAUD_RATE_COEF	1000
#define VFSSPI_DRDY_ACTIVE	1

struct vfsspi_device_data {
	dev_t devt;
	struct cdev cdev;
	struct spi_device *spi;
	struct list_head device_entry;
	struct mutex buffer_mutex;
	struct mutex kernel_lock;
	unsigned int is_opened;
	unsigned char *buffer;
	unsigned char *null_buffer;
	unsigned int current_spi_speed;

	struct gpio_desc *drdy_gpio;
	struct gpio_desc *sleep_gpio;
	struct gpio_desc *ldo_gpio;
	struct gpio_desc *vendor_gpio;
	int irq;
	spinlock_t irq_lock;
	atomic_t irq_enabled;
	bool ldo_onoff;

	struct task_struct *drdy_task;
	int user_pid;
	int signal_id;

	unsigned int orient;
};

static LIST_HEAD(vfsspi_device_list);
static DEFINE_MUTEX(vfsspi_device_list_mutex);
static struct class *vfsspi_device_class;

static const struct of_device_id vfsspi_match_table[] = {
	{ .compatible = "vfsspi,vfs7xxx" },
	{ }
};
MODULE_DEVICE_TABLE(of, vfsspi_match_table);

static int vfsspi_send_drdy_signal(struct vfsspi_device_data *data)
{
	if (!data->drdy_task) {
		pr_err("%s: no registered task\n", __func__);
		return -ENODEV;
	}

	return send_sig(data->signal_id, data->drdy_task, 0);
}

static inline ssize_t vfsspi_writeSync(struct vfsspi_device_data *data,
					size_t len)
{
	struct spi_transfer t = {
		.tx_buf = data->buffer,
		.rx_buf = data->null_buffer,
		.len = len,
		.speed_hz = data->current_spi_speed,
	};
	struct spi_message m;
	int status;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	status = spi_sync(data->spi, &m);

	return status == 0 ? m.actual_length : status;
}

static inline ssize_t vfsspi_readSync(struct vfsspi_device_data *data,
				       size_t len)
{
	struct spi_transfer t = {
		.tx_buf = data->null_buffer,
		.rx_buf = data->buffer,
		.len = len,
		.speed_hz = data->current_spi_speed,
	};
	struct spi_message m;
	int status;

	memset(data->null_buffer, 0, len);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	status = spi_sync(data->spi, &m);

	return status == 0 ? len : status;
}

static ssize_t vfsspi_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *fpos)
{
	struct vfsspi_device_data *data = filp->private_data;
	ssize_t status;

	if (!count || count > VFSSPI_DEFAULT_BUFFER_SIZE)
		return -EMSGSIZE;

	mutex_lock(&data->buffer_mutex);
	if (copy_from_user(data->buffer, buf, count))
		status = -EFAULT;
	else
		status = vfsspi_writeSync(data, count);
	mutex_unlock(&data->buffer_mutex);

	return status;
}

static ssize_t vfsspi_read(struct file *filp, char __user *buf,
			    size_t count, loff_t *fpos)
{
	struct vfsspi_device_data *data = filp->private_data;
	ssize_t status;

	if (!buf || !count || count > VFSSPI_DEFAULT_BUFFER_SIZE)
		return -EMSGSIZE;

	mutex_lock(&data->buffer_mutex);
	status = vfsspi_readSync(data, count);
	if (status > 0) {
		unsigned long missing = copy_to_user(buf, data->buffer, status);

		status = missing == status ? -EFAULT : status - missing;
	}
	mutex_unlock(&data->buffer_mutex);

	return status;
}

static int vfsspi_xfer(struct vfsspi_device_data *data,
			struct vfsspi_ioctl_transfer *tr)
{
	struct spi_transfer t = {
		.tx_buf = data->null_buffer,
		.rx_buf = data->buffer,
		.len = tr->len,
		.speed_hz = data->current_spi_speed,
	};
	struct spi_message m;
	int status;

	if (!tr->len || tr->len > VFSSPI_DEFAULT_BUFFER_SIZE)
		return -EMSGSIZE;

	if (tr->tx_buffer &&
	    copy_from_user(data->null_buffer, tr->tx_buffer, tr->len))
		return -EFAULT;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	status = spi_sync(data->spi, &m);
	if (status)
		return status;

	if (tr->rx_buffer) {
		unsigned long missing = copy_to_user(tr->rx_buffer, data->buffer,
						      tr->len);
		tr->len -= missing;
	}

	return 0;
}

static int vfsspi_rw_spi_message(struct vfsspi_device_data *data,
				  unsigned long arg)
{
	struct vfsspi_ioctl_transfer *tr;
	int status;

	tr = memdup_user((void __user *)arg, sizeof(*tr));
	if (IS_ERR(tr))
		return PTR_ERR(tr);

	status = vfsspi_xfer(data, tr);
	if (!status && copy_to_user((void __user *)arg, tr, sizeof(*tr)))
		status = -EFAULT;

	kfree(tr);
	return status;
}

static int vfsspi_set_clk(struct vfsspi_device_data *data, unsigned long arg)
{
	unsigned short clock;

	if (copy_from_user(&clock, (void __user *)arg, sizeof(clock)))
		return -EFAULT;

	switch (clock) {
	case 0:
	case 0xffff:
		data->current_spi_speed = VFSSPI_MAX_BAUD_RATE;
		break;
	default:
		data->current_spi_speed = min_t(unsigned int,
						 clock * VFSSPI_BAUD_RATE_COEF,
						 VFSSPI_MAX_BAUD_RATE);
		break;
	}
	data->spi->max_speed_hz = data->current_spi_speed;

	return 0;
}

static int vfsspi_register_drdy_signal(struct vfsspi_device_data *data,
					unsigned long arg)
{
	struct vfsspi_ioctl_register_signal sig;

	if (copy_from_user(&sig, (void __user *)arg, sizeof(sig)))
		return -EFAULT;

	data->user_pid = sig.user_pid;
	data->signal_id = sig.signal_id;

	rcu_read_lock();
	data->drdy_task = pid_task(find_pid_ns(data->user_pid, &init_pid_ns),
				    PIDTYPE_PID);
	if (!data->drdy_task) {
		rcu_read_unlock();
		return -ENODEV;
	}
	get_task_struct(data->drdy_task);
	rcu_read_unlock();

	return 0;
}

static void vfsspi_enable_irq(struct vfsspi_device_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->irq_lock, flags);
	if (atomic_cmpxchg(&data->irq_enabled, 0, 1) == 0)
		enable_irq(data->irq);
	spin_unlock_irqrestore(&data->irq_lock, flags);
}

static void vfsspi_disable_irq(struct vfsspi_device_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->irq_lock, flags);
	if (atomic_cmpxchg(&data->irq_enabled, 1, 0) == 1)
		disable_irq_nosync(data->irq);
	spin_unlock_irqrestore(&data->irq_lock, flags);
}

static irqreturn_t vfsspi_irq(int irq, void *context)
{
	struct vfsspi_device_data *data = context;

	/*
	 * Edge-triggered IRQ replay: disabling it while the edge already
	 * happened re-fires it at enable time, so confirm DRDY is actually
	 * asserted rather than trusting every call is a fresh event.
	 */
	if (gpiod_get_value(data->drdy_gpio) != VFSSPI_DRDY_ACTIVE)
		return IRQ_HANDLED;

	spin_lock(&data->irq_lock);
	if (atomic_cmpxchg(&data->irq_enabled, 1, 0) == 1) {
		disable_irq_nosync(data->irq);
		spin_unlock(&data->irq_lock);
		vfsspi_send_drdy_signal(data);
	} else {
		spin_unlock(&data->irq_lock);
	}

	return IRQ_HANDLED;
}

static int vfsspi_set_drdy_int(struct vfsspi_device_data *data,
				unsigned long arg)
{
	unsigned short enable;

	if (copy_from_user(&enable, (void __user *)arg, sizeof(enable)))
		return -EFAULT;

	if (!enable) {
		vfsspi_disable_irq(data);
		return 0;
	}

	vfsspi_enable_irq(data);
	/* DRDY may have asserted before we re-enabled the IRQ. */
	if (gpiod_get_value(data->drdy_gpio) == VFSSPI_DRDY_ACTIVE)
		vfsspi_send_drdy_signal(data);

	return 0;
}

static void vfsspi_ldo_onoff(struct vfsspi_device_data *data, bool on)
{
	if (!data->ldo_gpio)
		return;

	gpiod_set_value(data->ldo_gpio, on);
	data->ldo_onoff = on;
}

static void vfsspi_hard_reset(struct vfsspi_device_data *data)
{
	gpiod_set_value(data->sleep_gpio, 0);
	mdelay(1);
	gpiod_set_value(data->sleep_gpio, 1);
	mdelay(5);
}

static void vfsspi_suspend_sensor(struct vfsspi_device_data *data)
{
	gpiod_set_value(data->sleep_gpio, 0);
}

static void vfsspi_ioctl_power_on(struct vfsspi_device_data *data)
{
	if (!data->ldo_onoff)
		vfsspi_ldo_onoff(data, true);
}

static void vfsspi_ioctl_power_off(struct vfsspi_device_data *data)
{
	if (data->ldo_onoff) {
		vfsspi_ldo_onoff(data, false);
		gpiod_set_value(data->sleep_gpio, 0);
	}
}

static long vfsspi_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct vfsspi_device_data *data = filp->private_data;
	int status = 0;

	if (_IOC_TYPE(cmd) != VFSSPI_IOCTL_MAGIC)
		return -ENOTTY;

	mutex_lock(&data->buffer_mutex);
	switch (cmd) {
	case VFSSPI_IOCTL_DEVICE_RESET:
		vfsspi_hard_reset(data);
		break;
	case VFSSPI_IOCTL_DEVICE_SUSPEND:
		vfsspi_suspend_sensor(data);
		break;
	case VFSSPI_IOCTL_RW_SPI_MESSAGE:
		status = vfsspi_rw_spi_message(data, arg);
		break;
	case VFSSPI_IOCTL_SET_CLK:
		status = vfsspi_set_clk(data, arg);
		break;
	case VFSSPI_IOCTL_CHECK_DRDY:
		status = gpiod_get_value(data->drdy_gpio) == VFSSPI_DRDY_ACTIVE;
		break;
	case VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL:
		status = vfsspi_register_drdy_signal(data, arg);
		break;
	case VFSSPI_IOCTL_SET_DRDY_INT:
		status = vfsspi_set_drdy_int(data, arg);
		break;
	case VFSSPI_IOCTL_POWER_ON:
		vfsspi_ioctl_power_on(data);
		break;
	case VFSSPI_IOCTL_POWER_OFF:
		vfsspi_ioctl_power_off(data);
		break;
	case VFSSPI_IOCTL_GET_SENSOR_ORIENT:
		if (copy_to_user((void __user *)arg, &data->orient,
				  sizeof(data->orient)))
			status = -EFAULT;
		break;
	default:
		status = -ENOTTY;
		break;
	}
	mutex_unlock(&data->buffer_mutex);

	return status;
}

static int vfsspi_open(struct inode *inode, struct file *filp)
{
	struct vfsspi_device_data *data = NULL, *entry;
	int status = -ENXIO;

	mutex_lock(&vfsspi_device_list_mutex);
	list_for_each_entry(entry, &vfsspi_device_list, device_entry) {
		if (entry->devt == inode->i_rdev) {
			data = entry;
			status = 0;
			break;
		}
	}
	if (status)
		goto out_unlock;

	mutex_lock(&data->kernel_lock);
	if (data->is_opened) {
		status = -EBUSY;
		goto out_kernel_unlock;
	}

	if (!data->ldo_onoff) {
		vfsspi_ldo_onoff(data, true);
		msleep(100);
	}

	data->null_buffer = kmalloc(VFSSPI_DEFAULT_BUFFER_SIZE, GFP_KERNEL);
	data->buffer = kmalloc(VFSSPI_DEFAULT_BUFFER_SIZE, GFP_KERNEL);
	if (!data->null_buffer || !data->buffer) {
		kfree(data->null_buffer);
		kfree(data->buffer);
		data->null_buffer = NULL;
		data->buffer = NULL;
		status = -ENOMEM;
		goto out_kernel_unlock;
	}

	data->is_opened = 1;
	data->user_pid = 0;
	filp->private_data = data;
	stream_open(inode, filp);

out_kernel_unlock:
	mutex_unlock(&data->kernel_lock);
out_unlock:
	mutex_unlock(&vfsspi_device_list_mutex);
	return status;
}

static int vfsspi_release(struct inode *inode, struct file *filp)
{
	struct vfsspi_device_data *data = filp->private_data;

	mutex_lock(&vfsspi_device_list_mutex);
	filp->private_data = NULL;
	data->is_opened = 0;
	kfree(data->buffer);
	kfree(data->null_buffer);
	data->buffer = NULL;
	data->null_buffer = NULL;
	if (data->drdy_task) {
		put_task_struct(data->drdy_task);
		data->drdy_task = NULL;
	}
	if (data->ldo_onoff)
		vfsspi_ldo_onoff(data, false);
	mutex_unlock(&vfsspi_device_list_mutex);

	return 0;
}

static const struct file_operations vfsspi_fops = {
	.owner = THIS_MODULE,
	.write = vfsspi_write,
	.read = vfsspi_read,
	.unlocked_ioctl = vfsspi_ioctl,
	.open = vfsspi_open,
	.release = vfsspi_release,
};

static int vfsspi_vendor_check(struct vfsspi_device_data *data)
{
	int value;

	if (!data->vendor_gpio)
		return 0;

	vfsspi_ldo_onoff(data, true);
	msleep(10);
	value = gpiod_get_value(data->vendor_gpio);
	vfsspi_ldo_onoff(data, false);

	dev_info(&data->spi->dev, "vendor check: value=%d\n", value);

	return value;
}

static int vfsspi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct vfsspi_device_data *data;
	int status;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	data->current_spi_speed = VFSSPI_SLOW_BAUD_RATE;
	spin_lock_init(&data->irq_lock);
	mutex_init(&data->buffer_mutex);
	mutex_init(&data->kernel_lock);
	INIT_LIST_HEAD(&data->device_entry);

	data->drdy_gpio = devm_gpiod_get(dev, "drdy", GPIOD_IN);
	if (IS_ERR(data->drdy_gpio))
		return dev_err_probe(dev, PTR_ERR(data->drdy_gpio),
				      "failed to get drdy-gpios\n");

	data->sleep_gpio = devm_gpiod_get(dev, "sleep", GPIOD_OUT_LOW);
	if (IS_ERR(data->sleep_gpio))
		return dev_err_probe(dev, PTR_ERR(data->sleep_gpio),
				      "failed to get sleep-gpios\n");

	data->ldo_gpio = devm_gpiod_get_optional(dev, "ldo", GPIOD_OUT_LOW);
	if (IS_ERR(data->ldo_gpio))
		return dev_err_probe(dev, PTR_ERR(data->ldo_gpio),
				      "failed to get ldo-gpios\n");

	data->vendor_gpio = devm_gpiod_get_optional(dev, "vendor", GPIOD_IN);
	if (IS_ERR(data->vendor_gpio))
		return dev_err_probe(dev, PTR_ERR(data->vendor_gpio),
				      "failed to get vendor-gpios\n");

	data->irq = gpiod_to_irq(data->drdy_gpio);
	if (data->irq < 0)
		return data->irq;

	status = devm_request_irq(dev, data->irq, vfsspi_irq,
				   IRQF_TRIGGER_RISING, "vfsspi", data);
	if (status)
		return dev_err_probe(dev, status, "failed to request irq\n");
	disable_irq(data->irq);

	vfsspi_vendor_check(data);

	spi->bits_per_word = 8;
	spi->max_speed_hz = VFSSPI_SLOW_BAUD_RATE;
	spi->mode = SPI_MODE_0;
	status = spi_setup(spi);
	if (status)
		return dev_err_probe(dev, status, "spi_setup failed\n");

	status = alloc_chrdev_region(&data->devt, 0, 1, VFSSPI_DEV_NAME);
	if (status)
		return status;

	cdev_init(&data->cdev, &vfsspi_fops);
	data->cdev.owner = THIS_MODULE;
	status = cdev_add(&data->cdev, data->devt, 1);
	if (status)
		goto err_chrdev;

	if (!vfsspi_device_class) {
		vfsspi_device_class = class_create(VFSSPI_DEV_NAME);
		if (IS_ERR(vfsspi_device_class)) {
			status = PTR_ERR(vfsspi_device_class);
			vfsspi_device_class = NULL;
			goto err_cdev;
		}
	}

	dev = device_create(vfsspi_device_class, &spi->dev, data->devt, data,
			     "vfsspi");
	if (IS_ERR(dev)) {
		status = PTR_ERR(dev);
		goto err_cdev;
	}

	mutex_lock(&vfsspi_device_list_mutex);
	list_add(&data->device_entry, &vfsspi_device_list);
	mutex_unlock(&vfsspi_device_list_mutex);

	spi_set_drvdata(spi, data);
	dev_info(&spi->dev, "probe successful\n");

	return 0;

err_cdev:
	cdev_del(&data->cdev);
err_chrdev:
	unregister_chrdev_region(data->devt, 1);
	return status;
}

static void vfsspi_remove(struct spi_device *spi)
{
	struct vfsspi_device_data *data = spi_get_drvdata(spi);

	mutex_lock(&vfsspi_device_list_mutex);
	list_del(&data->device_entry);
	mutex_unlock(&vfsspi_device_list_mutex);

	device_destroy(vfsspi_device_class, data->devt);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devt, 1);
}

static int vfsspi_pm_suspend(struct device *dev)
{
	struct vfsspi_device_data *data = spi_get_drvdata(to_spi_device(dev));

	vfsspi_ioctl_power_off(data);

	return 0;
}

static int vfsspi_pm_resume(struct device *dev)
{
	struct vfsspi_device_data *data = spi_get_drvdata(to_spi_device(dev));

	vfsspi_ioctl_power_on(data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(vfsspi_pm_ops, vfsspi_pm_suspend,
				 vfsspi_pm_resume);

static struct spi_driver vfsspi_spi_driver = {
	.driver = {
		.name = VFSSPI_DEV_NAME,
		.of_match_table = vfsspi_match_table,
		.pm = &vfsspi_pm_ops,
	},
	.probe = vfsspi_probe,
	.remove = vfsspi_remove,
};
module_spi_driver(vfsspi_spi_driver);

MODULE_DESCRIPTION("Validity/Synaptics VFS7XXX fingerprint sensor driver");
MODULE_LICENSE("GPL");
