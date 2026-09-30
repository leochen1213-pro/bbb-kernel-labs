ifneq ($(KERNELRELEASE),)
obj-m := hello_param.o
else
KDIR := $(HOME)/linux-kernel-bbb-labs/src/linux
all:
	$(MAKE) -C $(KDIR) M=$$PWD modules
clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
endif
