# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)
# kbuild 這一層：模組名稱 = serial，原始碼 = serial-debug-k2.c
obj-m := serial.o
serial-y := serial-debug-k3.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
	cp serial.ko serial-k3.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f serial-k3.ko
endif
