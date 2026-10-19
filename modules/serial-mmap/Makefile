# SPDX-License-Identifier: GPL-2.0
# mmap practice: build serial-mmap-p3.c as serial.ko (module name "serial") + the test apps
ifneq ($(KERNELRELEASE),)
obj-m := serial.o
serial-y := serial-mmap-p3.o
ccflags-y := -I$(src)
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux
CC_ARM := arm-linux-gnueabi-gcc

all: mmap_regs watch_stats vbuf_test
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules
	cp serial.ko serial-p3.ko

mmap_regs: mmap_regs.c
	$(CC_ARM) -static -O2 -Wall -o $@ $<

watch_stats: watch_stats.c serial_mmap_uapi.h
	$(CC_ARM) -static -O2 -Wall -I. -o $@ $<

vbuf_test: vbuf_test.c serial_mmap_uapi.h
	$(CC_ARM) -static -O2 -Wall -I. -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f serial-p3.ko mmap_regs watch_stats vbuf_test
endif
