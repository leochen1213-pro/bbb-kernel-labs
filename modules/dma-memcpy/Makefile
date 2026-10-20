# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)
obj-m := dmamemcpy.o
dmamemcpy-y := dma-memcpy-x4.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
	cp dmamemcpy.ko dmamemcpy-x4.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f dmamemcpy-x4.ko
endif
