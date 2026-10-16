ifneq ($(KERNELRELEASE),)
obj-m := drvbroken.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
