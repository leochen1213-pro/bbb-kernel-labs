# SPDX-License-Identifier: GPL-2.0
# Bottom-half practice: build serial-bh-b1.c as serial.ko (module name "serial")
ifneq ($(KERNELRELEASE),)
obj-m := serial.o
serial-y := serial-bh-b3.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
	cp serial.ko serial-b3.ko

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f serial-b3.ko
endif
