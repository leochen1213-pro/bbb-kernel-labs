# Native on the BBB:          make
# Cross from the VM:          make CROSS_COMPILE=arm-linux-gnueabi-
#
# Linked statically: the VM toolchain's glibc (2.39) is newer than the one
# in the Bootlin BusyBox rootfs, so a dynamic binary fails with
# "version `GLIBC_2.34' not found".
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -Wall -Wextra -O2
LDFLAGS := -static

oled_temp: oled_temp.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f oled_temp

.PHONY: clean
