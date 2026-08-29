/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_SAMSUNG_EXYNOS8890_CPCTL_H
#define _LINUX_SOC_SAMSUNG_EXYNOS8890_CPCTL_H

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/exynos8890_cbd.h>
#include <linux/notifier.h>
#include <linux/types.h>

struct exynos8890_cpctl;

#define EXYNOS8890_CP_SHMEM_BASE		0xf0000000ULL
#define EXYNOS8890_CP_SHMEM_SIZE		0x08800000U
#define EXYNOS8890_CP_IPC_OFFSET		0x08000000U
#define EXYNOS8890_CP_IPC_SIZE		0x00800000U
#define EXYNOS8890_CP_SYSRAM_BASE	0x020c7800ULL
#define EXYNOS8890_CP_SYSRAM_SIZE	0x800U
#define EXYNOS8890_CP_MBOX_CHANNELS	16
#define EXYNOS8890_CP_MBOX_WORDS		64

/* Exact Samsung firmware CP-control ABI. */
#define EXYNOS8890_CP_SMC		0x82000700U
#define EXYNOS8890_CP_SMC_READ		0x3U
#define EXYNOS8890_CP_SMC_WRITE		0x4U
#define EXYNOS8890_CP_CTRL_PWRON		BIT(1)
#define EXYNOS8890_CP_CTRL_RESET_SET	BIT(2)
#define EXYNOS8890_CP_CTRL_START		BIT(3)
#define EXYNOS8890_CP_CTRL_ACTIVE_REQ_EN	BIT(5)
#define EXYNOS8890_CP_CTRL_ACTIVE_REQ_CLR BIT(6)
#define EXYNOS8890_CP_CTRL_RESET_REQ_EN	BIT(7)
#define EXYNOS8890_CP_CTRL_RESET_REQ_CLR	BIT(8)

enum exynos8890_cp_state {
	EXYNOS8890_CP_OFFLINE,
	EXYNOS8890_CP_POWERING,
	EXYNOS8890_CP_BOOTING,
	EXYNOS8890_CP_ONLINE,
	EXYNOS8890_CP_CRASH_RESET,
	EXYNOS8890_CP_CRASH_EXIT,
	EXYNOS8890_CP_CRASH_WATCHDOG,
	EXYNOS8890_CP_DUMPING,
	EXYNOS8890_CP_STOPPING,
	EXYNOS8890_CP_FAULTED,
};

/*
 * Translate to the state numbering Samsung's RIL expects back from
 * IOCTL_MODEM_STATUS. Anything it does not recognise has to look like a
 * crash rather than like silence, or the RIL waits forever.
 */
static inline enum exynos8890_cbd_modem_state
exynos8890_cp_state_to_legacy(enum exynos8890_cp_state state)
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

enum exynos8890_cp_boot_mode {
	EXYNOS8890_CP_BOOT_NORMAL,
	EXYNOS8890_CP_BOOT_DUMP,
	EXYNOS8890_CP_BOOT_REINIT,
};

enum exynos8890_cp_event {
	EXYNOS8890_CP_EVENT_POWER_ON,
	EXYNOS8890_CP_EVENT_POWER_OFF,
	EXYNOS8890_CP_EVENT_RESET,
	EXYNOS8890_CP_EVENT_IMAGE_LOADED,
	EXYNOS8890_CP_EVENT_SECURITY_ACCEPTED,
	EXYNOS8890_CP_EVENT_START_BOOTLOADER,
	EXYNOS8890_CP_EVENT_INIT_START,
	EXYNOS8890_CP_EVENT_PHONE_START,
	EXYNOS8890_CP_EVENT_BOOT_TIMEOUT,
	EXYNOS8890_CP_EVENT_MAILBOX,
	EXYNOS8890_CP_EVENT_STATE,
	EXYNOS8890_CP_EVENT_PHONE_ACTIVE,
	EXYNOS8890_CP_EVENT_BOOT_COMPLETE,
	EXYNOS8890_CP_EVENT_FAIL,
	EXYNOS8890_CP_EVENT_WATCHDOG,
	EXYNOS8890_CP_EVENT_DUMP_READY,
	EXYNOS8890_CP_EVENT_SHUTDOWN,
};

enum exynos8890_cp_crash_source {
	EXYNOS8890_CP_CRASH_COMMAND,
	EXYNOS8890_CP_CRASH_FAIL_IRQ,
	EXYNOS8890_CP_CRASH_WATCHDOG_IRQ,
	EXYNOS8890_CP_CRASH_FORCED,
	EXYNOS8890_CP_CRASH_BAD_FRAME,
	EXYNOS8890_CP_CRASH_BAD_CHANNEL,
	EXYNOS8890_CP_CRASH_TRANSPORT,
	EXYNOS8890_CP_CRASH_TIMEOUT,
};

enum exynos8890_cp_doorbell {
	EXYNOS8890_DB_AP2CP_MSG = 0,
	EXYNOS8890_DB_AP2CP_WAKEUP = 1,
	EXYNOS8890_DB_AP2CP_STATUS = 2,
	EXYNOS8890_DB_AP2CP_ACTIVE = 3,
	EXYNOS8890_DB_CP2AP_MSG = 0,
	EXYNOS8890_DB_CP2AP_WAKEUP = 1,
	EXYNOS8890_DB_CP2AP_STATUS = 2,
	EXYNOS8890_DB_CP2AP_PERF = 3,
	EXYNOS8890_DB_CP2AP_ACTIVE = 4,
	EXYNOS8890_DB_CP2AP_PERF_CPU = 5,
	EXYNOS8890_DB_CP2AP_PERF_MIF = 6,
	EXYNOS8890_DB_CP2AP_PERF_INT = 7,
	EXYNOS8890_DB_CP2AP_WAKELOCK = 8,
	EXYNOS8890_DB_CP2AP_PCIE_L1SS = 9,
};

enum exynos8890_cp_mbox_word {
	EXYNOS8890_MBX_AP2CP_MSG = 0,
	EXYNOS8890_MBX_CP2AP_MSG = 1,
	EXYNOS8890_MBX_AP2CP_WAKEUP = 2,
	EXYNOS8890_MBX_CP2AP_WAKEUP = 3,
	EXYNOS8890_MBX_AP2CP_STATUS = 4,
	EXYNOS8890_MBX_CP2AP_STATUS = 5,
	EXYNOS8890_MBX_AP2CP_ACTIVE = 6,
	EXYNOS8890_MBX_CP2AP_DVFSREQ = 7,
	EXYNOS8890_MBX_CP2AP_WAKELOCK = 8,
	EXYNOS8890_MBX_CP2AP_ACTIVE = 9,
	EXYNOS8890_MBX_CP2AP_DVFSREQ_CPU = 10,
	EXYNOS8890_MBX_CP2AP_DVFSREQ_MIF = 11,
	EXYNOS8890_MBX_CP2AP_DVFSREQ_INT = 12,
	EXYNOS8890_MBX_AP2CP_MIF_VALUE = 13,
	EXYNOS8890_MBX_AP2CP_SEC = 14,
	EXYNOS8890_MBX_AP2CP_USEC = 15,
	EXYNOS8890_MBX_CP2AP_PCIE_L1SS = 17,
	EXYNOS8890_MBX_AP2CP_ET_DAC_CAL = 32,
	EXYNOS8890_MBX_AP2CP_INFO_VALUE = 33,
	EXYNOS8890_MBX_AP2CP_LOCK_VALUE = 39,
};

enum exynos8890_cp_ctrl_bank {
	EXYNOS8890_CP_CTRL_SECURE,
	EXYNOS8890_CP_CTRL_NONSECURE,
};

struct exynos8890_cp_event_data {
	enum exynos8890_cp_event event;
	enum exynos8890_cp_state old_state;
	enum exynos8890_cp_state new_state;
	u32 raw_status;
	enum exynos8890_cp_crash_source crash_source;
	int error;
};

struct exynos8890_cp_boot_image {
	const void *data;
	size_t size;
	u32 boot_offset;
	u32 main_offset;
	enum exynos8890_cp_boot_mode mode;
};

struct exynos8890_cp_security_request {
	enum exynos8890_cp_boot_mode mode;
	u32 boot_size;
	u32 main_size;
	u32 image_size;
};

struct exynos8890_cp_shmem_layout {
	phys_addr_t base;
	size_t size;
	u32 ipc_offset;
	u32 ipc_size;
	u32 boot_size;
};

struct exynos8890_cp_status {
	enum exynos8890_cp_state state;
	u32 secure_ctrl;
	u32 nonsecure_ctrl;
	u32 ap_status;
	u32 cp_status;
	bool sim_online;
	bool dual_sim;
};

/*
 * Error contract
 * --------------
 * All functions returning int use 0 for success.  -EINVAL means a NULL
 * mandatory argument, an out-of-range bank/word/doorbell/mode, or malformed
 * image metadata.  -EOVERFLOW means an otherwise well-formed image/security
 * range does not fit the fixed reserved-memory geometry.  -EBUSY means the
 * requested operation is not legal in the current CP state.  -ENODEV means
 * the supplier is shutting down or an acquired mailbox channel disappeared;
 * get() returns -EPROBE_DEFER while a referenced supplier has not completed
 * probe, and -ENODEV when its samsung,cpctl phandle is absent.
 *
 * PMU/syscon, mailbox, GPIO and allocation failures are returned unchanged.
 * A non-zero low half of a packed CP-control read result and a positive
 * firmware write/security status become -EIO.  Negative security/ET-DAC
 * firmware status is returned unchanged.  shutdown() returns -ETIMEDOUT when
 * CP does not acknowledge powerdown within three seconds, after forcibly
 * clearing CP_PWRON.  Notifier return values never veto completed hardware or
 * state publication.
 *
 * get() takes a supplier-device reference and installs a consumer/supplier
 * device link; every successful get() must be paired with put().  State
 * notifiers run in process context and never under the state spinlock.  Raw
 * mailbox word writes do not ring; write_and_ring() publishes the word, issues
 * a write barrier, and then sends the selected generic-mailbox doorbell.
 */
struct exynos8890_cpctl *exynos8890_cpctl_get(struct device *consumer);
void exynos8890_cpctl_put(struct exynos8890_cpctl *cpctl);
struct device *exynos8890_cpctl_device(struct exynos8890_cpctl *cpctl);

int exynos8890_cpctl_register_notifier(struct exynos8890_cpctl *cpctl,
				      struct notifier_block *nb);
int exynos8890_cpctl_unregister_notifier(struct exynos8890_cpctl *cpctl,
					struct notifier_block *nb);

enum exynos8890_cp_state
exynos8890_cpctl_state(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_get_status(struct exynos8890_cpctl *cpctl,
			       struct exynos8890_cp_status *status);
int exynos8890_cpctl_get_shmem_layout(struct exynos8890_cpctl *cpctl,
				     struct exynos8890_cp_shmem_layout *layout);

int exynos8890_cpctl_power_on(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_power_off(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_reset(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_release(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_shutdown(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_force_crash(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_active_clear(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_reset_request_clear(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_read_et_dac_cal(struct exynos8890_cpctl *cpctl,
				     u16 *calibration);
int exynos8890_cpctl_start_dump(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_finish_dump(struct exynos8890_cpctl *cpctl);

int exynos8890_cpctl_prepare_boot(struct exynos8890_cpctl *cpctl,
				 const struct exynos8890_cp_boot_image *image);
int exynos8890_cpctl_security_request(struct exynos8890_cpctl *cpctl,
				     const struct exynos8890_cp_security_request *request);
int exynos8890_cpctl_complete_boot(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_set_protocol_suspended(struct exynos8890_cpctl *cpctl,
					    bool suspended);
int exynos8890_cpctl_set_network_suspended(struct exynos8890_cpctl *cpctl,
					   bool suspended);

int exynos8890_cpctl_read_ctrl(struct exynos8890_cpctl *cpctl,
			      enum exynos8890_cp_ctrl_bank bank, u32 *value);
int exynos8890_cpctl_write_ctrl(struct exynos8890_cpctl *cpctl,
			       enum exynos8890_cp_ctrl_bank bank, u32 value);
int exynos8890_cpctl_mbox_read(struct exynos8890_cpctl *cpctl,
			      enum exynos8890_cp_mbox_word word, u32 *value);
int exynos8890_cpctl_mbox_write(struct exynos8890_cpctl *cpctl,
			       enum exynos8890_cp_mbox_word word, u32 value);
int exynos8890_cpctl_mbox_clear_all(struct exynos8890_cpctl *cpctl);
int exynos8890_cpctl_ring(struct exynos8890_cpctl *cpctl,
			 enum exynos8890_cp_doorbell doorbell);
int exynos8890_cpctl_write_and_ring(struct exynos8890_cpctl *cpctl,
				   enum exynos8890_cp_mbox_word word,
				   u32 value,
				   enum exynos8890_cp_doorbell doorbell);

#endif /* _LINUX_SOC_SAMSUNG_EXYNOS8890_CPCTL_H */
