# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)
# NEW (3.6): one core module + one module per bus
obj-m := leo_adxl345_core.o leo_adxl345_spi.o leo_adxl345_i2c.o
leo_adxl345_core-y := adxl345_core.o
leo_adxl345_spi-y  := adxl345_spi.o
leo_adxl345_i2c-y  := adxl345_i2c.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
