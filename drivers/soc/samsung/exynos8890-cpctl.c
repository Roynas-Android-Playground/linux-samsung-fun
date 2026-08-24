// SPDX-License-Identifier: GPL-2.0-only
/* Samsung Exynos8890 SS310AP modem control plane. */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soc/samsung/exynos8890-cpctl.h>
#include <linux/mfd/syscon.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define EXYNOS8890_CP_SMC_ID			0x82000700
#define EXYNOS8890_CP_SMC_READ_CTRL		3
#define EXYNOS8890_CP_SMC_WRITE_CTRL		4
#define EXYNOS8890_CP_ET_DAC_SMC_ID		0x82001014
#define EXYNOS8890_CP_ET_DAC_COMMAND		0x2002
#define EXYNOS8890_CP_ET_DAC_INDEX		0x1c

#define EXYNOS8890_CP_CTRL_NS_OFFSET		0x0030
#define EXYNOS8890_CP_CTRL_S_OFFSET		0x0034
#define EXYNOS8890_CP_CENTRAL_SEQ_CFG		0x0280
#define EXYNOS8890_CP_RESET_AHEAD_PWR		0x1170
#define EXYNOS8890_CP_CLEANY_BUS_PWR		0x11cc
#define EXYNOS8890_CP_LOGIC_RESET_PWR		0x11d0
#define EXYNOS8890_CP_TCXO_GATE_PWR		0x11d4
#define EXYNOS8890_CP_RESET_ASB_PWR		0x11d8

#define EXYNOS8890_CP_PWRON			BIT(1)
#define EXYNOS8890_CP_RESET_SET			BIT(2)
#define EXYNOS8890_CP_START			BIT(3)
#define EXYNOS8890_CP_ACTIVE_REQ_CLR		BIT(6)
#define EXYNOS8890_CP_RESET_REQ_CLR		BIT(8)

#define EXYNOS8890_CP_ISSR_BASE			0x80
#define EXYNOS8890_CP_CAL_DST_OFFSET		0x1000
#define EXYNOS8890_CP_SHUTDOWN_TIMEOUT_MS	3000
#define EXYNOS8890_CP_BOOT_STATUS_RETRIES	100

struct exynos8890_cpctl {
	struct device *dev;
	struct regmap *pmu;
	struct regmap *mailbox_words;
	void __iomem *sysram;
	void __iomem *shmem_base;
	struct mutex command_lock;
	spinlock_t state_lock;
	spinlock_t irq_lock;
	struct blocking_notifier_head notifier;
	struct completion boot_complete;
	struct completion powerdown_complete;
	struct work_struct fail_work;
	struct work_struct watchdog_work;
	struct work_struct active_work;
	struct work_struct mailbox_work;
	unsigned long pending_doorbells;
	struct mbox_client mailbox_client[EXYNOS8890_CP_MBOX_CHANNELS];
	struct mbox_chan *mailbox_channel[EXYNOS8890_CP_MBOX_CHANNELS];
	struct exynos8890_cp_shmem_layout shmem;
	enum exynos8890_cp_state state;
	int fail_irq;
	int watchdog_irq;
	u32 board_revision;
	u32 ap_status;
	u32 cp_status;
	bool dual_sim;
	bool fail_irq_enabled;
	bool watchdog_irq_enabled;
	bool protocol_suspended;
	bool network_suspended;
	bool shutting_down;
};

static int exynos8890_cpctl_notify(struct exynos8890_cpctl *cpctl,
				  enum exynos8890_cp_event event,
				  enum exynos8890_cp_state old_state,
				  enum exynos8890_cp_crash_source source,
				  int error)
{
	struct exynos8890_cp_event_data data = {
		.event = event,
		.old_state = old_state,
		.new_state = exynos8890_cpctl_state(cpctl),
		.crash_source = source,
		.error = error,
	};

	if (exynos8890_cpctl_mbox_read(cpctl, EXYNOS8890_MBX_CP2AP_STATUS,
					&data.raw_status))
		data.raw_status = 0;

	blocking_notifier_call_chain(&cpctl->notifier, event, &data);
	return 0;
}

static bool exynos8890_cpctl_transition_valid(enum exynos8890_cp_state from,
					     enum exynos8890_cp_state to)
{
	if (from == to)
		return true;
	if (to == EXYNOS8890_CP_OFFLINE || to == EXYNOS8890_CP_STOPPING ||
	    to == EXYNOS8890_CP_FAULTED)
		return true;

	switch (from) {
	case EXYNOS8890_CP_OFFLINE:
		return to == EXYNOS8890_CP_POWERING ||
		       to == EXYNOS8890_CP_BOOTING;
	case EXYNOS8890_CP_POWERING:
		return to == EXYNOS8890_CP_BOOTING ||
		       to == EXYNOS8890_CP_CRASH_RESET;
	case EXYNOS8890_CP_BOOTING:
		return to == EXYNOS8890_CP_ONLINE ||
		       to == EXYNOS8890_CP_CRASH_RESET ||
		       to == EXYNOS8890_CP_CRASH_EXIT ||
		       to == EXYNOS8890_CP_CRASH_WATCHDOG;
	case EXYNOS8890_CP_ONLINE:
		return to == EXYNOS8890_CP_CRASH_RESET ||
		       to == EXYNOS8890_CP_CRASH_EXIT ||
		       to == EXYNOS8890_CP_CRASH_WATCHDOG;
	case EXYNOS8890_CP_CRASH_RESET:
	case EXYNOS8890_CP_CRASH_WATCHDOG:
		return to == EXYNOS8890_CP_POWERING ||
		       to == EXYNOS8890_CP_BOOTING ||
		       to == EXYNOS8890_CP_CRASH_EXIT;
	case EXYNOS8890_CP_CRASH_EXIT:
		return to == EXYNOS8890_CP_DUMPING ||
		       to == EXYNOS8890_CP_POWERING ||
		       to == EXYNOS8890_CP_BOOTING;
	case EXYNOS8890_CP_DUMPING:
		return to == EXYNOS8890_CP_CRASH_EXIT;
	case EXYNOS8890_CP_STOPPING:
		return false;
	case EXYNOS8890_CP_FAULTED:
		return to == EXYNOS8890_CP_POWERING ||
		       to == EXYNOS8890_CP_BOOTING;
	}
	return false;
}

static int exynos8890_cpctl_transition(struct exynos8890_cpctl *cpctl,
				      enum exynos8890_cp_state to,
				      enum exynos8890_cp_event event,
				      enum exynos8890_cp_crash_source source,
				      int error)
{
	enum exynos8890_cp_state old;
	unsigned long flags;

	spin_lock_irqsave(&cpctl->state_lock, flags);
	old = cpctl->state;
	if (!exynos8890_cpctl_transition_valid(old, to)) {
		spin_unlock_irqrestore(&cpctl->state_lock, flags);
		return -EBUSY;
	}
	cpctl->state = to;
	spin_unlock_irqrestore(&cpctl->state_lock, flags);

	if (old == to && event == EXYNOS8890_CP_EVENT_STATE)
		return 0;
	return exynos8890_cpctl_notify(cpctl, event, old, source, error);
}

static bool exynos8890_cpctl_can_power_on(enum exynos8890_cp_state state)
{
	return state == EXYNOS8890_CP_OFFLINE ||
	       state == EXYNOS8890_CP_CRASH_RESET ||
	       state == EXYNOS8890_CP_CRASH_EXIT ||
	       state == EXYNOS8890_CP_CRASH_WATCHDOG ||
	       state == EXYNOS8890_CP_FAULTED;
}

static int exynos8890_cpctl_smc(u32 arg0, u32 arg1, u32 arg2, u32 arg3,
			       struct arm_smccc_res *result)
{
	if (!result)
		return -EINVAL;

	/* The Samsung firmware ABI requires completion of all accesses first. */
	asm volatile("dsb sy" ::: "memory");
	arm_smccc_smc(arg0, arg1, arg2, arg3, 0, 0, 0, 0, result);
	return 0;
}

static int exynos8890_cpctl_update_ctrl(struct exynos8890_cpctl *cpctl,
				       enum exynos8890_cp_ctrl_bank bank,
				       u32 set, u32 clear)
{
	u32 value;
	int ret;

	ret = exynos8890_cpctl_read_ctrl(cpctl, bank, &value);
	if (ret)
		return ret;
	return exynos8890_cpctl_write_ctrl(cpctl, bank,
					   (value | set) & ~clear);
}

static int exynos8890_cpctl_configure_powerdown(struct exynos8890_cpctl *cpctl)
{
	static const u32 registers[] = {
		EXYNOS8890_CP_CENTRAL_SEQ_CFG,
		EXYNOS8890_CP_RESET_AHEAD_PWR,
		EXYNOS8890_CP_LOGIC_RESET_PWR,
		EXYNOS8890_CP_RESET_ASB_PWR,
		EXYNOS8890_CP_TCXO_GATE_PWR,
		EXYNOS8890_CP_CLEANY_BUS_PWR,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = regmap_write(cpctl->pmu, registers[i], 0);
		if (ret)
			return ret;
	}
	return 0;
}

static int exynos8890_cpctl_copy_calibration(struct exynos8890_cpctl *cpctl)
{
	void *sysram;
	u16 calibration;
	int ret;

	sysram = kmalloc(EXYNOS8890_CP_SYSRAM_SIZE, GFP_KERNEL);
	if (!sysram)
		return -ENOMEM;
	memcpy_fromio(sysram, cpctl->sysram, EXYNOS8890_CP_SYSRAM_SIZE);
	memcpy_toio(cpctl->shmem_base, sysram, EXYNOS8890_CP_SYSRAM_SIZE);
	kfree(sysram);
	ret = exynos8890_cpctl_read_et_dac_cal(cpctl, &calibration);
	if (ret)
		return ret;
	return exynos8890_cpctl_mbox_write(cpctl,
					   EXYNOS8890_MBX_AP2CP_ET_DAC_CAL,
					   calibration);
}

static int exynos8890_cpctl_program_mailboxes(struct exynos8890_cpctl *cpctl)
{
	int ret;

	ret = exynos8890_cpctl_mbox_clear_all(cpctl);
	if (ret)
		return ret;
	return exynos8890_cpctl_mbox_write(cpctl,
					   EXYNOS8890_MBX_AP2CP_INFO_VALUE,
					   cpctl->board_revision |
					   ((u32)cpctl->dual_sim << 8));
}

static void exynos8890_cpctl_set_fail_irq(struct exynos8890_cpctl *cpctl,
					 bool enable)
{
	unsigned long flags;
	bool change;

	spin_lock_irqsave(&cpctl->irq_lock, flags);
	change = cpctl->fail_irq_enabled != enable;
	cpctl->fail_irq_enabled = enable;
	spin_unlock_irqrestore(&cpctl->irq_lock, flags);
	if (!change)
		return;
	if (enable)
		enable_irq(cpctl->fail_irq);
	else
		disable_irq(cpctl->fail_irq);
}

static void exynos8890_cpctl_set_watchdog_irq(struct exynos8890_cpctl *cpctl,
					     bool enable)
{
	unsigned long flags;
	bool change;

	spin_lock_irqsave(&cpctl->irq_lock, flags);
	change = cpctl->watchdog_irq_enabled != enable;
	cpctl->watchdog_irq_enabled = enable;
	spin_unlock_irqrestore(&cpctl->irq_lock, flags);
	if (!change)
		return;
	if (enable)
		enable_irq(cpctl->watchdog_irq);
	else
		disable_irq(cpctl->watchdog_irq);
}

static irqreturn_t exynos8890_cpctl_fail_irq(int irq, void *data)
{
	struct exynos8890_cpctl *cpctl = data;
	unsigned long flags;

	spin_lock_irqsave(&cpctl->irq_lock, flags);
	if (!cpctl->fail_irq_enabled) {
		spin_unlock_irqrestore(&cpctl->irq_lock, flags);
		return IRQ_NONE;
	}
	cpctl->fail_irq_enabled = false;
	disable_irq_nosync(irq);
	spin_unlock_irqrestore(&cpctl->irq_lock, flags);
	schedule_work(&cpctl->fail_work);
	return IRQ_HANDLED;
}

static irqreturn_t exynos8890_cpctl_watchdog_irq(int irq, void *data)
{
	struct exynos8890_cpctl *cpctl = data;
	unsigned long flags;

	spin_lock_irqsave(&cpctl->irq_lock, flags);
	if (!cpctl->watchdog_irq_enabled) {
		spin_unlock_irqrestore(&cpctl->irq_lock, flags);
		return IRQ_NONE;
	}
	cpctl->watchdog_irq_enabled = false;
	disable_irq_nosync(irq);
	spin_unlock_irqrestore(&cpctl->irq_lock, flags);
	schedule_work(&cpctl->watchdog_work);
	return IRQ_HANDLED;
}

static void exynos8890_cpctl_fail_work(struct work_struct *work)
{
	struct exynos8890_cpctl *cpctl =
		container_of(work, struct exynos8890_cpctl, fail_work);
	int ret;

	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_active_clear(cpctl);
	exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_CRASH_RESET,
				    EXYNOS8890_CP_EVENT_FAIL,
				    EXYNOS8890_CP_CRASH_FAIL_IRQ, ret);
	mutex_unlock(&cpctl->command_lock);
}

static void exynos8890_cpctl_watchdog_work(struct work_struct *work)
{
	struct exynos8890_cpctl *cpctl =
		container_of(work, struct exynos8890_cpctl, watchdog_work);
	int ret;

	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_reset_request_clear(cpctl);
	exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_CRASH_WATCHDOG,
				    EXYNOS8890_CP_EVENT_WATCHDOG,
				    EXYNOS8890_CP_CRASH_WATCHDOG_IRQ, ret);
	mutex_unlock(&cpctl->command_lock);
}

static void exynos8890_cpctl_active_work(struct work_struct *work)
{
	struct exynos8890_cpctl *cpctl =
		container_of(work, struct exynos8890_cpctl, active_work);
	u32 active;
	bool powered;
	int ret;

	mutex_lock(&cpctl->command_lock);
	/*
	 * The vendor handler only ever acts when CP reports itself
	 * inactive while still powered on (-> OFFLINE).  CP becoming
	 * active, and CP reporting inactive while already unpowered, are
	 * both silently ignored -- do not synthesize a notification for
	 * either case.
	 */
	ret = exynos8890_cpctl_read_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
					 &cpctl->cp_status);
	powered = !ret && !!(cpctl->cp_status & EXYNOS8890_CP_PWRON);
	ret = exynos8890_cpctl_mbox_read(cpctl,
					 EXYNOS8890_MBX_CP2AP_ACTIVE, &active);
	if (!ret && !active && powered) {
		exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_OFFLINE,
					    EXYNOS8890_CP_EVENT_PHONE_ACTIVE,
					    EXYNOS8890_CP_CRASH_COMMAND, 0);
		complete_all(&cpctl->powerdown_complete);
	}
	mutex_unlock(&cpctl->command_lock);
}

static void exynos8890_cpctl_mailbox_work(struct work_struct *work)
{
	struct exynos8890_cpctl *cpctl =
		container_of(work, struct exynos8890_cpctl, mailbox_work);
	unsigned long pending = xchg(&cpctl->pending_doorbells, 0);
	unsigned int channel;

	for_each_set_bit(channel, &pending, EXYNOS8890_CP_MBOX_CHANNELS) {
		struct exynos8890_cp_event_data data = {
			.event = EXYNOS8890_CP_EVENT_MAILBOX,
			.old_state = exynos8890_cpctl_state(cpctl),
			.new_state = exynos8890_cpctl_state(cpctl),
			.raw_status = channel,
		};

		blocking_notifier_call_chain(&cpctl->notifier,
					     EXYNOS8890_CP_EVENT_MAILBOX,
					     &data);
		if (channel == EXYNOS8890_DB_CP2AP_ACTIVE)
			schedule_work(&cpctl->active_work);
	}
}

static void exynos8890_cpctl_mailbox_rx(struct mbox_client *client, void *message)
{
	struct exynos8890_cpctl *cpctl = dev_get_drvdata(client->dev);
	unsigned int channel = client - cpctl->mailbox_client;

	if (channel >= EXYNOS8890_CP_MBOX_CHANNELS)
		return;
	set_bit(channel, &cpctl->pending_doorbells);
	schedule_work(&cpctl->mailbox_work);
}

struct exynos8890_cpctl *exynos8890_cpctl_get(struct device *consumer)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct exynos8890_cpctl *cpctl;

	if (!consumer || !consumer->of_node)
		return ERR_PTR(-EINVAL);

	if (device_is_compatible(consumer, "sec_modem,modem_pdata")) {
		pdev = to_platform_device(consumer);
		get_device(consumer);
	} else {
		node = of_parse_phandle(consumer->of_node, "samsung,cpctl", 0);
		if (!node)
			return ERR_PTR(-ENODEV);
		pdev = of_find_device_by_node(node);
		of_node_put(node);
		if (!pdev)
			return ERR_PTR(-EPROBE_DEFER);
	}

	cpctl = platform_get_drvdata(pdev);
	if (!cpctl || READ_ONCE(cpctl->shutting_down)) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	if (consumer != &pdev->dev &&
	    !device_link_add(consumer, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&pdev->dev);
		return ERR_PTR(-ENOMEM);
	}
	return cpctl;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_get);

void exynos8890_cpctl_put(struct exynos8890_cpctl *cpctl)
{
	if (cpctl)
		put_device(cpctl->dev);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_put);

struct device *exynos8890_cpctl_device(struct exynos8890_cpctl *cpctl)
{
	return cpctl ? cpctl->dev : NULL;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_device);

int exynos8890_cpctl_register_notifier(struct exynos8890_cpctl *cpctl,
				      struct notifier_block *nb)
{
	if (!cpctl || !nb)
		return -EINVAL;
	if (READ_ONCE(cpctl->shutting_down))
		return -ENODEV;
	return blocking_notifier_chain_register(&cpctl->notifier, nb);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_register_notifier);

int exynos8890_cpctl_unregister_notifier(struct exynos8890_cpctl *cpctl,
					struct notifier_block *nb)
{
	if (!cpctl || !nb)
		return -EINVAL;
	return blocking_notifier_chain_unregister(&cpctl->notifier, nb);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_unregister_notifier);

enum exynos8890_cp_state exynos8890_cpctl_state(struct exynos8890_cpctl *cpctl)
{
	unsigned long flags;
	enum exynos8890_cp_state state;

	if (!cpctl)
		return EXYNOS8890_CP_FAULTED;
	spin_lock_irqsave(&cpctl->state_lock, flags);
	state = cpctl->state;
	spin_unlock_irqrestore(&cpctl->state_lock, flags);
	return state;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_state);

int exynos8890_cpctl_get_status(struct exynos8890_cpctl *cpctl,
			       struct exynos8890_cp_status *status)
{
	int ret;

	if (!cpctl || !status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	status->state = exynos8890_cpctl_state(cpctl);
	ret = exynos8890_cpctl_read_ctrl(cpctl, EXYNOS8890_CP_CTRL_SECURE,
					 &status->secure_ctrl);
	if (ret)
		return ret;
	ret = exynos8890_cpctl_read_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
					 &status->nonsecure_ctrl);
	if (ret)
		return ret;
	ret = exynos8890_cpctl_mbox_read(cpctl, EXYNOS8890_MBX_AP2CP_ACTIVE,
					 &status->ap_status);
	if (ret)
		return ret;
	ret = exynos8890_cpctl_mbox_read(cpctl, EXYNOS8890_MBX_CP2AP_STATUS,
					 &status->cp_status);
	if (ret)
		return ret;
	status->sim_online = !!status->cp_status;
	status->dual_sim = cpctl->dual_sim;
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_get_status);

int exynos8890_cpctl_get_shmem_layout(struct exynos8890_cpctl *cpctl,
				     struct exynos8890_cp_shmem_layout *layout)
{
	if (!cpctl || !layout)
		return -EINVAL;
	*layout = cpctl->shmem;
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_get_shmem_layout);

int exynos8890_cpctl_power_on(struct exynos8890_cpctl *cpctl)
{
	bool on;
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->shutting_down) {
		ret = -ENODEV;
		goto out;
	}
	if (!exynos8890_cpctl_can_power_on(cpctl->state)) {
		ret = -EBUSY;
		goto out;
	}
	ret = exynos8890_cpctl_read_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
					 &cpctl->cp_status);
	if (ret)
		goto fault;
	on = !!(cpctl->cp_status & EXYNOS8890_CP_PWRON);
	ret = exynos8890_cpctl_program_mailboxes(cpctl);
	if (ret)
		goto fault;
	ret = exynos8890_cpctl_copy_calibration(cpctl);
	if (ret)
		goto fault;
	/*
	 * Only declare the AP side ready once the calibration data and
	 * board info are actually in place, and strictly before CP is
	 * powered/released so it never observes a half-written mailbox.
	 */
	cpctl->ap_status = 1;
	ret = exynos8890_cpctl_mbox_write(cpctl, EXYNOS8890_MBX_AP2CP_ACTIVE, 1);
	if (ret)
		goto fault;
	if (!on) {
		ret = exynos8890_cpctl_update_ctrl(cpctl,
						 EXYNOS8890_CP_CTRL_NONSECURE,
						 EXYNOS8890_CP_PWRON, 0);
		if (ret)
			goto fault;
	}
	ret = exynos8890_cpctl_update_ctrl(cpctl, EXYNOS8890_CP_CTRL_SECURE,
					   EXYNOS8890_CP_START, 0);
	if (ret)
		goto fault;
	/* Let the power rail/clocks settle before anything else proceeds. */
	msleep(300);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_POWERING,
					  EXYNOS8890_CP_EVENT_POWER_ON,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	goto out;
fault:
	exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_FAULTED,
				    EXYNOS8890_CP_EVENT_STATE,
				    EXYNOS8890_CP_CRASH_COMMAND, ret);
out:
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_power_on);

int exynos8890_cpctl_power_off(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_update_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
					   0, EXYNOS8890_CP_PWRON);
	if (!ret) {
		exynos8890_cpctl_set_fail_irq(cpctl, false);
		exynos8890_cpctl_set_watchdog_irq(cpctl, false);
		ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_OFFLINE,
						  EXYNOS8890_CP_EVENT_POWER_OFF,
						  EXYNOS8890_CP_CRASH_COMMAND, 0);
	}
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_power_off);

int exynos8890_cpctl_reset(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->state == EXYNOS8890_CP_STOPPING) {
		ret = -EBUSY;
		goto out;
	}
	ret = exynos8890_cpctl_configure_powerdown(cpctl);
	if (!ret)
		ret = exynos8890_cpctl_update_ctrl(cpctl,
						 EXYNOS8890_CP_CTRL_NONSECURE,
						 EXYNOS8890_CP_RESET_SET, 0);
	if (!ret) {
		usleep_range(80, 100);
		exynos8890_cpctl_set_fail_irq(cpctl, false);
		exynos8890_cpctl_set_watchdog_irq(cpctl, false);
		ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_OFFLINE,
						  EXYNOS8890_CP_EVENT_RESET,
						  EXYNOS8890_CP_CRASH_COMMAND, 0);
	}
out:
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_reset);

int exynos8890_cpctl_release(struct exynos8890_cpctl *cpctl)
{
	unsigned int attempts = EXYNOS8890_CP_BOOT_STATUS_RETRIES;
	u32 cp_status;
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->state != EXYNOS8890_CP_POWERING) {
		ret = -EBUSY;
		goto out;
	}
	ret = exynos8890_cpctl_update_ctrl(cpctl, EXYNOS8890_CP_CTRL_SECURE,
					   EXYNOS8890_CP_START, 0);
	if (ret)
		goto out;
	reinit_completion(&cpctl->boot_complete);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_BOOTING,
					  EXYNOS8890_CP_EVENT_START_BOOTLOADER,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	if (ret)
		goto out;

	/*
	 * Wait for the CP bootloader to post its status word before
	 * arming crash detection, matching the vendor driver's ~2s poll
	 * (100 attempts, 10-20ms apart) for CP2AP_STATUS to go non-zero.
	 */
	for (;;) {
		ret = exynos8890_cpctl_mbox_read(cpctl,
						 EXYNOS8890_MBX_CP2AP_STATUS,
						 &cp_status);
		if (ret)
			goto out;
		if (cp_status)
			break;
		if (--attempts == 0) {
			ret = -ETIMEDOUT;
			exynos8890_cpctl_notify(cpctl,
						EXYNOS8890_CP_EVENT_BOOT_TIMEOUT,
						EXYNOS8890_CP_BOOTING,
						EXYNOS8890_CP_CRASH_TIMEOUT, ret);
			goto out;
		}
		usleep_range(10000, 20000);
	}

	exynos8890_cpctl_set_watchdog_irq(cpctl, false);
	exynos8890_cpctl_set_fail_irq(cpctl, true);
out:
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_release);

int exynos8890_cpctl_shutdown(struct exynos8890_cpctl *cpctl)
{
	unsigned long timeout;
	int ret = 0;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->state == EXYNOS8890_CP_OFFLINE)
		goto power_off;
	reinit_completion(&cpctl->powerdown_complete);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_STOPPING,
					  EXYNOS8890_CP_EVENT_SHUTDOWN,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	mutex_unlock(&cpctl->command_lock);
	if (ret)
		return ret;
	timeout = wait_for_completion_timeout(&cpctl->powerdown_complete,
			msecs_to_jiffies(EXYNOS8890_CP_SHUTDOWN_TIMEOUT_MS));
	mutex_lock(&cpctl->command_lock);
	if (!timeout)
		ret = -ETIMEDOUT;
power_off:
	if (exynos8890_cpctl_update_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
						 0, EXYNOS8890_CP_PWRON) && !ret)
		ret = -EIO;
	exynos8890_cpctl_set_fail_irq(cpctl, false);
	exynos8890_cpctl_set_watchdog_irq(cpctl, false);
	exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_OFFLINE,
				    EXYNOS8890_CP_EVENT_POWER_OFF,
				    EXYNOS8890_CP_CRASH_COMMAND, ret);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_shutdown);

int exynos8890_cpctl_force_crash(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_CRASH_EXIT,
					  EXYNOS8890_CP_EVENT_FAIL,
					  EXYNOS8890_CP_CRASH_FORCED, 0);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_force_crash);

int exynos8890_cpctl_active_clear(struct exynos8890_cpctl *cpctl)
{
	if (!cpctl)
		return -EINVAL;
	return exynos8890_cpctl_update_ctrl(cpctl,
					    EXYNOS8890_CP_CTRL_NONSECURE,
					    EXYNOS8890_CP_ACTIVE_REQ_CLR, 0);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_active_clear);

int exynos8890_cpctl_reset_request_clear(struct exynos8890_cpctl *cpctl)
{
	if (!cpctl)
		return -EINVAL;
	return exynos8890_cpctl_update_ctrl(cpctl,
					    EXYNOS8890_CP_CTRL_NONSECURE,
					    EXYNOS8890_CP_RESET_REQ_CLR, 0);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_reset_request_clear);

int exynos8890_cpctl_read_et_dac_cal(struct exynos8890_cpctl *cpctl,
				     u16 *calibration)
{
	struct arm_smccc_res result;
	s32 status;
	int ret;

	if (!cpctl || !calibration)
		return -EINVAL;
	ret = exynos8890_cpctl_smc(EXYNOS8890_CP_ET_DAC_SMC_ID, 0,
				   EXYNOS8890_CP_ET_DAC_COMMAND,
				   EXYNOS8890_CP_ET_DAC_INDEX, &result);
	if (ret)
		return ret;
	status = (s32)result.a0;
	if (status > 0)
		return -EIO;
	if (status < 0)
		return status;
	*calibration = FIELD_GET(GENMASK_ULL(31, 16), result.a2);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_read_et_dac_cal);

int exynos8890_cpctl_start_dump(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_DUMPING,
					  EXYNOS8890_CP_EVENT_DUMP_READY,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	if (!ret)
		ret = exynos8890_cpctl_update_ctrl(cpctl,
						 EXYNOS8890_CP_CTRL_SECURE,
						 EXYNOS8890_CP_START, 0);
	if (!ret)
		ret = exynos8890_cpctl_mbox_write(cpctl,
						 EXYNOS8890_MBX_AP2CP_STATUS, 1);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_start_dump);

int exynos8890_cpctl_finish_dump(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_CRASH_EXIT,
					  EXYNOS8890_CP_EVENT_STATE,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_finish_dump);

int exynos8890_cpctl_prepare_boot(struct exynos8890_cpctl *cpctl,
				 const struct exynos8890_cp_boot_image *image)
{
	size_t region_size;
	void __iomem *region;
	int ret;

	if (!cpctl || !image || (!image->data && image->size))
		return -EINVAL;
	if (image->mode > EXYNOS8890_CP_BOOT_REINIT)
		return -EINVAL;
	if (!image->size)
		return -EINVAL;
	region = cpctl->shmem_base;
	region_size = cpctl->shmem.boot_size;
	if (image->mode == EXYNOS8890_CP_BOOT_DUMP) {
		region += cpctl->shmem.ipc_offset;
		region_size = cpctl->shmem.ipc_size;
	}
	if (image->boot_offset > region_size ||
	    image->size > region_size - image->boot_offset ||
	    image->main_offset > image->size)
		return -EOVERFLOW;

	mutex_lock(&cpctl->command_lock);
	if (cpctl->shutting_down) {
		ret = -ENODEV;
		goto out;
	}
	memcpy_toio(region + image->boot_offset, image->data, image->size);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_BOOTING,
					  EXYNOS8890_CP_EVENT_IMAGE_LOADED,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
out:
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_prepare_boot);

int exynos8890_cpctl_security_request(struct exynos8890_cpctl *cpctl,
				     const struct exynos8890_cp_security_request *request)
{
	struct arm_smccc_res result;
	u32 param2, param3;
	s32 status;
	int ret;

	if (!cpctl || !request)
		return -EINVAL;
	if (request->mode > EXYNOS8890_CP_BOOT_REINIT)
		return -EINVAL;
	if (request->boot_size > cpctl->shmem.boot_size ||
	    request->main_size > cpctl->shmem.size ||
	    request->image_size > cpctl->shmem.size)
		return -EOVERFLOW;

	switch (request->mode) {
	case EXYNOS8890_CP_BOOT_NORMAL:
		param2 = request->boot_size;
		param3 = request->main_size;
		break;
	case EXYNOS8890_CP_BOOT_DUMP:
		param2 = request->boot_size;
		param3 = lower_32_bits(cpctl->shmem.base + cpctl->shmem.ipc_offset);
		break;
	case EXYNOS8890_CP_BOOT_REINIT:
		param2 = 0;
		param3 = 0;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_smc(EXYNOS8890_CP_SMC_ID, request->mode,
				   param2, param3, &result);
	if (!ret) {
		status = (s32)result.a0;
		if (status > 0)
			ret = -EIO;
		else if (status < 0)
			ret = status;
	}
	if (!ret)
		exynos8890_cpctl_notify(cpctl,
					 EXYNOS8890_CP_EVENT_SECURITY_ACCEPTED,
					 exynos8890_cpctl_state(cpctl),
					 EXYNOS8890_CP_CRASH_COMMAND, 0);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_security_request);

int exynos8890_cpctl_complete_boot(struct exynos8890_cpctl *cpctl)
{
	int ret;

	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	exynos8890_cpctl_set_watchdog_irq(cpctl, true);
	exynos8890_cpctl_set_fail_irq(cpctl, false);
	ret = exynos8890_cpctl_transition(cpctl, EXYNOS8890_CP_ONLINE,
					  EXYNOS8890_CP_EVENT_BOOT_COMPLETE,
					  EXYNOS8890_CP_CRASH_COMMAND, 0);
	if (!ret)
		complete_all(&cpctl->boot_complete);
	mutex_unlock(&cpctl->command_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_complete_boot);

int exynos8890_cpctl_set_protocol_suspended(struct exynos8890_cpctl *cpctl,
					    bool suspended)
{
	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->state != EXYNOS8890_CP_ONLINE) {
		mutex_unlock(&cpctl->command_lock);
		return -EBUSY;
	}
	cpctl->protocol_suspended = suspended;
	mutex_unlock(&cpctl->command_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_set_protocol_suspended);

int exynos8890_cpctl_set_network_suspended(struct exynos8890_cpctl *cpctl,
					   bool suspended)
{
	if (!cpctl)
		return -EINVAL;
	mutex_lock(&cpctl->command_lock);
	if (cpctl->state != EXYNOS8890_CP_ONLINE) {
		mutex_unlock(&cpctl->command_lock);
		return -EBUSY;
	}
	cpctl->network_suspended = suspended;
	mutex_unlock(&cpctl->command_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_set_network_suspended);

int exynos8890_cpctl_read_ctrl(struct exynos8890_cpctl *cpctl,
			      enum exynos8890_cp_ctrl_bank bank, u32 *value)
{
	struct arm_smccc_res result;
	u32 packed;
	int ret;

	if (!cpctl || !value || bank > EXYNOS8890_CP_CTRL_NONSECURE)
		return -EINVAL;
	ret = exynos8890_cpctl_smc(EXYNOS8890_CP_SMC_ID,
				   EXYNOS8890_CP_SMC_READ_CTRL, 0, bank, &result);
	if (ret)
		return ret;
	packed = (u32)result.a0;
	if (packed & GENMASK(15, 0))
		return -EIO;
	*value = packed >> 16;
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_read_ctrl);

int exynos8890_cpctl_write_ctrl(struct exynos8890_cpctl *cpctl,
			       enum exynos8890_cp_ctrl_bank bank, u32 value)
{
	struct arm_smccc_res result;
	s32 status;
	int ret;

	if (!cpctl || bank > EXYNOS8890_CP_CTRL_NONSECURE)
		return -EINVAL;
	ret = exynos8890_cpctl_smc(EXYNOS8890_CP_SMC_ID,
				   EXYNOS8890_CP_SMC_WRITE_CTRL,
				   value, bank, &result);
	if (ret)
		return ret;
	status = (s32)result.a0;
	if (status > 0)
		return -EIO;
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_write_ctrl);

int exynos8890_cpctl_mbox_read(struct exynos8890_cpctl *cpctl,
			      enum exynos8890_cp_mbox_word word, u32 *value)
{
	if (!cpctl || !value || (unsigned int)word >= EXYNOS8890_CP_MBOX_WORDS)
		return -EINVAL;
	return regmap_read(cpctl->mailbox_words,
			   EXYNOS8890_CP_ISSR_BASE + (unsigned int)word * 4,
			   value);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_mbox_read);

int exynos8890_cpctl_mbox_write(struct exynos8890_cpctl *cpctl,
			       enum exynos8890_cp_mbox_word word, u32 value)
{
	if (!cpctl || (unsigned int)word >= EXYNOS8890_CP_MBOX_WORDS)
		return -EINVAL;
	return regmap_write(cpctl->mailbox_words,
			    EXYNOS8890_CP_ISSR_BASE + (unsigned int)word * 4,
			    value);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_mbox_write);

int exynos8890_cpctl_mbox_clear_all(struct exynos8890_cpctl *cpctl)
{
	unsigned int word;
	int ret;

	if (!cpctl)
		return -EINVAL;
	for (word = 0; word < EXYNOS8890_CP_MBOX_WORDS; word++) {
		ret = regmap_write(cpctl->mailbox_words,
				   EXYNOS8890_CP_ISSR_BASE + word * 4, 0);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_mbox_clear_all);

int exynos8890_cpctl_ring(struct exynos8890_cpctl *cpctl,
			 enum exynos8890_cp_doorbell doorbell)
{
	int ret;

	if (!cpctl || (unsigned int)doorbell >= EXYNOS8890_CP_MBOX_CHANNELS)
		return -EINVAL;
	if (!cpctl->mailbox_channel[doorbell])
		return -ENODEV;
	ret = mbox_send_message(cpctl->mailbox_channel[doorbell], NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(cpctl->mailbox_channel[doorbell], 0);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_ring);

int exynos8890_cpctl_write_and_ring(struct exynos8890_cpctl *cpctl,
				   enum exynos8890_cp_mbox_word word,
				   u32 value,
				   enum exynos8890_cp_doorbell doorbell)
{
	int ret;

	ret = exynos8890_cpctl_mbox_write(cpctl, word, value);
	if (ret)
		return ret;
	wmb();
	return exynos8890_cpctl_ring(cpctl, doorbell);
}
EXPORT_SYMBOL_GPL(exynos8890_cpctl_write_and_ring);

static int exynos8890_cpctl_parse_shmem(struct exynos8890_cpctl *cpctl)
{
	struct device_node *node;
	struct reserved_mem *rmem;

	node = of_parse_phandle(cpctl->dev->of_node, "memory-region", 0);
	if (!node)
		return -ENODEV;
	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem)
		return -EPROBE_DEFER;
	if (rmem->base != EXYNOS8890_CP_SHMEM_BASE ||
	    rmem->size != EXYNOS8890_CP_SHMEM_SIZE)
		return -EINVAL;

	cpctl->shmem.base = rmem->base;
	cpctl->shmem.size = rmem->size;
	cpctl->shmem.ipc_offset = EXYNOS8890_CP_IPC_OFFSET;
	cpctl->shmem.ipc_size = EXYNOS8890_CP_IPC_SIZE;
	cpctl->shmem.boot_size = EXYNOS8890_CP_IPC_OFFSET;
	cpctl->shmem_base = devm_ioremap(cpctl->dev,
			rmem->base + EXYNOS8890_CP_IPC_OFFSET +
			EXYNOS8890_CP_CAL_DST_OFFSET,
			EXYNOS8890_CP_SYSRAM_SIZE);
	if (!cpctl->shmem_base)
		return -ENOMEM;
	return 0;
}

static int exynos8890_cpctl_parse_board(struct exynos8890_cpctl *cpctl)
{
	struct gpio_descs *revision;
	unsigned int i;
	int value;

	revision = devm_gpiod_get_array(cpctl->dev, "revision", GPIOD_IN);
	if (IS_ERR(revision))
		return PTR_ERR(revision);
	if (revision->ndescs != 4)
		return -EINVAL;
	for (i = 0; i < revision->ndescs; i++) {
		value = gpiod_get_value_cansleep(revision->desc[i]);
		if (value < 0)
			return value;
		cpctl->board_revision |= value << i;
	}
	cpctl->dual_sim = of_property_read_bool(cpctl->dev->of_node,
						"samsung,dual-sim");
	return 0;
}

static int exynos8890_cpctl_get_mailboxes(struct exynos8890_cpctl *cpctl)
{
	unsigned int i;
	int ret;

	cpctl->mailbox_words = syscon_regmap_lookup_by_phandle(cpctl->dev->of_node,
						      "samsung,mbox-syscon");
	if (IS_ERR(cpctl->mailbox_words))
		return PTR_ERR(cpctl->mailbox_words);
	for (i = 0; i < EXYNOS8890_CP_MBOX_CHANNELS; i++) {
		struct mbox_client *client = &cpctl->mailbox_client[i];

		client->dev = cpctl->dev;
		client->rx_callback = exynos8890_cpctl_mailbox_rx;
		client->tx_block = false;
		client->knows_txdone = true;
		cpctl->mailbox_channel[i] = mbox_request_channel(client, i);
		if (IS_ERR(cpctl->mailbox_channel[i])) {
			ret = PTR_ERR(cpctl->mailbox_channel[i]);
			cpctl->mailbox_channel[i] = NULL;
			goto free_channels;
		}
	}
	return 0;

free_channels:
	while (i--)
		mbox_free_channel(cpctl->mailbox_channel[i]);
	return ret;
}

static void exynos8890_cpctl_put_mailboxes(struct exynos8890_cpctl *cpctl)
{
	unsigned int i;

	for (i = 0; i < EXYNOS8890_CP_MBOX_CHANNELS; i++) {
		if (cpctl->mailbox_channel[i]) {
			mbox_free_channel(cpctl->mailbox_channel[i]);
			cpctl->mailbox_channel[i] = NULL;
		}
	}
}

static int exynos8890_cpctl_write_time(struct exynos8890_cpctl *cpctl)
{
	struct timespec64 now;
	int ret;

	ktime_get_real_ts64(&now);
	ret = exynos8890_cpctl_mbox_write(cpctl, EXYNOS8890_MBX_AP2CP_SEC,
					  lower_32_bits(now.tv_sec));
	if (ret)
		return ret;
	return exynos8890_cpctl_mbox_write(cpctl, EXYNOS8890_MBX_AP2CP_USEC,
					   now.tv_nsec / NSEC_PER_USEC);
}

static int exynos8890_cpctl_suspend(struct device *dev)
{
	struct exynos8890_cpctl *cpctl = dev_get_drvdata(dev);
	int ret;

	if (!cpctl)
		return 0;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_write_time(cpctl);
	if (!ret) {
		cpctl->ap_status = 0;
		ret = exynos8890_cpctl_write_and_ring(cpctl,
						 EXYNOS8890_MBX_AP2CP_ACTIVE, 0,
						 EXYNOS8890_DB_AP2CP_ACTIVE);
	}
	mutex_unlock(&cpctl->command_lock);
	return ret;
}

static int exynos8890_cpctl_resume(struct device *dev)
{
	struct exynos8890_cpctl *cpctl = dev_get_drvdata(dev);
	int ret;

	if (!cpctl)
		return 0;
	mutex_lock(&cpctl->command_lock);
	ret = exynos8890_cpctl_write_time(cpctl);
	if (!ret) {
		cpctl->ap_status = 1;
		ret = exynos8890_cpctl_write_and_ring(cpctl,
						 EXYNOS8890_MBX_AP2CP_ACTIVE, 1,
						 EXYNOS8890_DB_AP2CP_ACTIVE);
	}
	mutex_unlock(&cpctl->command_lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(exynos8890_cpctl_pm_ops,
				       exynos8890_cpctl_suspend,
				       exynos8890_cpctl_resume);

static int exynos8890_cpctl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos8890_cpctl *cpctl;
	u32 ctrl;
	int ret;

	cpctl = devm_kzalloc(dev, sizeof(*cpctl), GFP_KERNEL);
	if (!cpctl)
		return -ENOMEM;
	cpctl->dev = dev;
	mutex_init(&cpctl->command_lock);
	spin_lock_init(&cpctl->state_lock);
	spin_lock_init(&cpctl->irq_lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&cpctl->notifier);
	init_completion(&cpctl->boot_complete);
	init_completion(&cpctl->powerdown_complete);
	INIT_WORK(&cpctl->fail_work, exynos8890_cpctl_fail_work);
	INIT_WORK(&cpctl->watchdog_work, exynos8890_cpctl_watchdog_work);
	INIT_WORK(&cpctl->active_work, exynos8890_cpctl_active_work);
	INIT_WORK(&cpctl->mailbox_work, exynos8890_cpctl_mailbox_work);
	cpctl->state = EXYNOS8890_CP_OFFLINE;
	platform_set_drvdata(pdev, cpctl);

	cpctl->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "samsung,pmu");
	if (IS_ERR(cpctl->pmu))
		return dev_err_probe(dev, PTR_ERR(cpctl->pmu),
				     "failed to acquire PMU regmap\n");
	cpctl->sysram = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpctl->sysram))
		return dev_err_probe(dev, PTR_ERR(cpctl->sysram),
				     "failed to map SYSRAM\n");
	ret = exynos8890_cpctl_parse_shmem(cpctl);
	if (ret)
		return dev_err_probe(dev, ret, "invalid modem reserved memory\n");
	ret = exynos8890_cpctl_parse_board(cpctl);
	if (ret)
		return dev_err_probe(dev, ret, "invalid board revision GPIOs\n");
	ret = exynos8890_cpctl_get_mailboxes(cpctl);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire modem mailboxes\n");

	cpctl->fail_irq = platform_get_irq(pdev, 0);
	if (cpctl->fail_irq < 0) {
		ret = cpctl->fail_irq;
		goto put_mailboxes;
	}
	ret = devm_request_irq(dev, cpctl->fail_irq, exynos8890_cpctl_fail_irq,
			       IRQF_NO_SUSPEND | IRQF_NO_AUTOEN, "cp_fail", cpctl);
	if (ret)
		goto put_mailboxes;
	cpctl->watchdog_irq = platform_get_irq(pdev, 1);
	if (cpctl->watchdog_irq < 0) {
		ret = cpctl->watchdog_irq;
		goto put_mailboxes;
	}
	ret = devm_request_irq(dev, cpctl->watchdog_irq,
			       exynos8890_cpctl_watchdog_irq,
			       IRQF_NO_SUSPEND | IRQF_NO_AUTOEN, "cp_wdt", cpctl);
	if (ret)
		goto put_mailboxes;

	/*
	 * Start from the vendor-defined quiescent control state. The vendor
	 * driver never does this eagerly at probe — exynos_cp_reset()/
	 * exynos_cp_init() only run later, on demand, from the modem_on/
	 * modem_reset ops triggered by userspace — so treat a failure here
	 * as diagnostic rather than fatal: the SMC ID, sub-command numbers,
	 * and secure/non-secure bank encoding all match the vendor driver's
	 * pmu-cp.c exactly, so a failure this early is more likely a
	 * probe-time prerequisite the secure firmware expects than a wrong
	 * argument, and must not prevent the rest of the driver (which the
	 * real modem_on/reset path depends on) from binding.
	 */
	ret = exynos8890_cpctl_read_ctrl(cpctl, EXYNOS8890_CP_CTRL_NONSECURE,
					 &ctrl);
	if (!ret)
		ret = exynos8890_cpctl_write_ctrl(cpctl,
				EXYNOS8890_CP_CTRL_NONSECURE,
				ctrl & ~(EXYNOS8890_CP_RESET_SET |
					 EXYNOS8890_CP_PWRON));
	if (!ret)
		ret = exynos8890_cpctl_read_ctrl(cpctl,
				EXYNOS8890_CP_CTRL_SECURE, &ctrl);
	if (!ret)
		ret = exynos8890_cpctl_write_ctrl(cpctl,
				EXYNOS8890_CP_CTRL_SECURE,
				ctrl & ~EXYNOS8890_CP_START);
	if (ret)
		dev_warn(dev,
			"failed to reach quiescent CP control state: %d\n",
			ret);

	dev_info(dev, "SS310AP control plane ready (board %u, %s SIM)\n",
		 cpctl->board_revision, cpctl->dual_sim ? "dual" : "single");
	return 0;

put_mailboxes:
	exynos8890_cpctl_put_mailboxes(cpctl);
	return dev_err_probe(dev, ret, "failed to initialize CP control plane\n");
}

static void exynos8890_cpctl_remove(struct platform_device *pdev)
{
	struct exynos8890_cpctl *cpctl = platform_get_drvdata(pdev);

	WRITE_ONCE(cpctl->shutting_down, true);
	exynos8890_cpctl_shutdown(cpctl);
	exynos8890_cpctl_set_fail_irq(cpctl, false);
	exynos8890_cpctl_set_watchdog_irq(cpctl, false);
	cancel_work_sync(&cpctl->mailbox_work);
	cancel_work_sync(&cpctl->active_work);
	cancel_work_sync(&cpctl->fail_work);
	cancel_work_sync(&cpctl->watchdog_work);
	exynos8890_cpctl_put_mailboxes(cpctl);
	platform_set_drvdata(pdev, NULL);
}

static void exynos8890_cpctl_platform_shutdown(struct platform_device *pdev)
{
	struct exynos8890_cpctl *cpctl = platform_get_drvdata(pdev);

	if (!cpctl)
		return;
	WRITE_ONCE(cpctl->shutting_down, true);
	exynos8890_cpctl_shutdown(cpctl);
	exynos8890_cpctl_set_fail_irq(cpctl, false);
	exynos8890_cpctl_set_watchdog_irq(cpctl, false);
	cancel_work_sync(&cpctl->mailbox_work);
	cancel_work_sync(&cpctl->active_work);
	cancel_work_sync(&cpctl->fail_work);
	cancel_work_sync(&cpctl->watchdog_work);
}

static const struct of_device_id exynos8890_cpctl_of_match[] = {
	{ .compatible = "samsung,exynos8890-ss310ap-cpctl" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_cpctl_of_match);

static struct platform_driver exynos8890_cpctl_driver = {
	.probe = exynos8890_cpctl_probe,
	.remove = exynos8890_cpctl_remove,
	.shutdown = exynos8890_cpctl_platform_shutdown,
	.driver = {
		.name = "exynos8890-ss310ap-cpctl",
		.of_match_table = exynos8890_cpctl_of_match,
		.pm = pm_sleep_ptr(&exynos8890_cpctl_pm_ops),
	},
};
module_platform_driver(exynos8890_cpctl_driver);

MODULE_DESCRIPTION("Exynos8890 SS310AP control plane");
MODULE_LICENSE("GPL");
