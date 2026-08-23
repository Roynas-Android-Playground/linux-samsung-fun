# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux WWAN device drivers.
#

obj-$(CONFIG_WWAN) += wwan.o
wwan-objs += wwan_core.o

obj-$(CONFIG_WWAN_HWSIM) += wwan_hwsim.o

exynos8890-sipc-y := exynos8890-sipc-core.o exynos8890-sipc-sbd.o \
			exynos8890-sipc-wwan.o
obj-$(CONFIG_WWAN_EXYNOS8890_SIPC) += exynos8890-sipc.o

obj-$(CONFIG_MHI_WWAN_CTRL) += mhi_wwan_ctrl.o
obj-$(CONFIG_MHI_WWAN_MBIM) += mhi_wwan_mbim.o
obj-$(CONFIG_QCOM_BAM_DMUX) += qcom_bam_dmux.o
obj-$(CONFIG_RPMSG_WWAN_CTRL) += rpmsg_wwan_ctrl.o
obj-$(CONFIG_IOSM) += iosm/
obj-$(CONFIG_MTK_T7XX) += t7xx/
