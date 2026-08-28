/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos8890 idle-IP control - port of vendor Cronos_8890
 * include/soc/samsung/exynos-powermode.h IDLE_IP section.
 *
 * The PMU tracks 128 IP-block idle bits (4 registers at 0x03E0) and
 * matching mask registers (0x03F0). Before entering a system powerdown
 * the kernel programs the per-mode masks; a masked IP is ignored by the
 * central sequencer. Drivers register their block with
 * exynos_get_idle_ip_index() (DT "samsung,idle-ip-names" list position) and
 * report activity with exynos_update_ip_idle_status().
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_IDLE_IP_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_IDLE_IP_H

#define EXYNOS8890_IDLE_IP_REG_SIZE		32
#define EXYNOS8890_IDLE_IP_MAX_INDEX		127
#define EXYNOS8890_IDLE_IP_FIX_INDEX_COUNT	2
#define EXYNOS8890_IDLE_IP_MAX_CONFIGURABLE	\
	(EXYNOS8890_IDLE_IP_MAX_INDEX - EXYNOS8890_IDLE_IP_FIX_INDEX_COUNT)

#ifdef CONFIG_EXYNOS8890_PM

int exynos8890_get_idle_ip_index(const char *ip_name);
void exynos8890_update_ip_idle_status(int ip_index, int idle);
int exynos8890_check_idle_ip(void);

#else

static inline int exynos8890_get_idle_ip_index(const char *ip_name)
{
	return -ENODEV;
}

static inline void exynos8890_update_ip_idle_status(int ip_index, int idle) {}

static inline int exynos8890_check_idle_ip(void) { return 0; }

#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_IDLE_IP_H */
