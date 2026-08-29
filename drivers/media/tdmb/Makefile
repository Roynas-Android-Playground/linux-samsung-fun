# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the T-DMB glue driver
#

obj-$(CONFIG_TDMB) += tdmb.o tdmb_data.o
obj-$(CONFIG_TDMB_SPI) += tdmb_spi.o
obj-$(CONFIG_TDMB_FC8080) += tdmb_port_fc8080.o

ccflags-y += -I $(srctree)/drivers/media/dvb-frontends/fc8080
ccflags-y += -I $(srctree)/drivers/media/tdmb
