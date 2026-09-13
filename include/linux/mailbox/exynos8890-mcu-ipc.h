/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MAILBOX_EXYNOS8890_MCU_IPC_H
#define __LINUX_MAILBOX_EXYNOS8890_MCU_IPC_H

struct mbox_chan;

/*
 * MCU_IPC-only receive epoch barrier; other providers return -EOPNOTSUPP.
 * The caller must own the requested channel and serialize pause/resume with
 * each other and with channel release. Pauses nest; balance each successful
 * pause before releasing the channel (shutdown also cancels outstanding
 * pauses). Never resume an old ownership after release/reacquisition.
 *
 * Pause revokes delivery and synchronizes the provider IRQ before returning.
 * Call in sleepable context without any lock needed by RX callbacks. Drain
 * client work after pause, without locks needed by that work. Final resume
 * discards only this channel's pending doorbell before republishing delivery;
 * it does not enable a channel disabled by shutdown or alter TX/PMU policy.
 */
int exynos8890_mbox_rx_pause(struct mbox_chan *chan);
int exynos8890_mbox_rx_resume(struct mbox_chan *chan);

#endif /* __LINUX_MAILBOX_EXYNOS8890_MCU_IPC_H */
