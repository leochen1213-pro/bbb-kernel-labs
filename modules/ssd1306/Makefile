# Out-of-tree build against the BBB kernel tree:
#   make            → ssd1306.ko
#   make clean
ifneq ($(KERNELRELEASE),)
obj-m := ssd1306.o
else
KDIR          ?= $(HOME)/linux-kernel-bbb-labs/src/linux
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabi-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

.PHONY: all clean
endif
