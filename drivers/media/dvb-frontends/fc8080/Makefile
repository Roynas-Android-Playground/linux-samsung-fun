# SPDX-License-Identifier: GPL-2.0

fc8080-objs := bbm.o dmbdrv_wrap_fc8080.o fc8080_bb.o fc8080_isr.o \
	       fc8080_tun.o fci_hal.o fci_hpi.o fci_oal.o fci_tun.o \
	       fic.o ficdecoder.o
fc8080-$(CONFIG_TDMB_SPI) += fc8080_spi.o
fc8080-$(CONFIG_TDMB_EBI) += fc8080_ppi.o
fc8080-$(CONFIG_TDMB_I2C) += fc8080_i2c.o

obj-$(CONFIG_DVB_FC8080) += fc8080.o

ccflags-y += -I $(srctree)/drivers/media/dvb-frontends/fc8080
