# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)
obj-m := leo_adxl345.o
leo_adxl345-y := adxl345.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
