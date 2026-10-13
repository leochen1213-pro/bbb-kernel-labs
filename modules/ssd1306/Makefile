# Out-of-tree build against the BBB kernel tree, plus the user-space tool:
#   make            → ssd1306.ko + ssd1306_lock.ko + user/oled_temp
#   make module     → both .ko files only
#   make user       → user/oled_temp only
#   make deploy     → scp the .ko files and oled_temp to the board
#   make clean
ifneq ($(KERNELRELEASE),)
# Kbuild pass (invoked from the kernel tree): only the module lives here
obj-m := ssd1306.o ssd1306_lock.o   # ssd1306_lock: locking experiments only
else
KDIR          ?= $(HOME)/linux-kernel-bbb-labs/src/linux
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabi-
BOARD         ?= root@192.168.50.100
BOARD_DIR     ?= /root

KMAKE := $(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

all: module user

module:
	$(KMAKE) modules

user:
	$(MAKE) -C user CROSS_COMPILE=$(CROSS_COMPILE)

deploy: all
	scp -O ssd1306.ko ssd1306_lock.ko user/oled_temp $(BOARD):$(BOARD_DIR)/

clean:
	$(KMAKE) clean
	$(MAKE) -C user clean

.PHONY: all module user deploy clean
endif
