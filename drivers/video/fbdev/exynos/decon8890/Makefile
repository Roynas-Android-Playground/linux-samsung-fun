# SPDX-License-Identifier: GPL-2.0
#
# Exynos8890 DECON/DSIM/VPP display driver, ported from vendor
# drivers/video/fbdev/exynos/decon_8890/. Only the herolte configuration is
# wired here (single DSI, S6E3HA3 dynamic-backlight panel) - vendor's
# Makefile selects between many board/panel variants this port doesn't need.

obj-$(CONFIG_EXYNOS8890_DECON) += decon8890.o vpp8890.o dsim8890.o

decon8890-y := decon_core.o decon_f.o decon_s.o decon_t.o decon_dsi.o \
	       decon_helper.o decon_bts.o decon_reg_8890.o

vpp8890-y := vpp_drv.o vpp_reg_8890.o

dsim8890-y := dsim_drv.o dsim_reg_8890.o

obj-$(CONFIG_EXYNOS8890_DECON) += panels/
