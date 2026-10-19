// SPDX-License-Identifier: GPL-2.0
/*
 * mmap practice - P1 user-space test
 *
 *   mmap_regs <dev>            dump a few UART registers through mmap()
 *   mmap_regs <dev> <char>     also send <char> by writing THR directly
 *   mmap_regs <dev> -big       try to map 2 pages (the driver must refuse)
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -Wall -o mmap_regs mmap_regs.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

/* 8250 register index * 4 (the OMAP UART registers are 32-bit spaced) */
#define OFF_THR		(0x00 * 4)	/* write: transmit holding register */
#define OFF_IER		(0x01 * 4)
#define OFF_LSR		(0x05 * 4)
#define OFF_MDR1	(0x08 * 4)
#define OFF_SCR		(0x10 * 4)
#define LSR_THRE	0x20

static uint32_t rd(volatile uint8_t *base, unsigned int off)
{
	return *(volatile uint32_t *)(base + off);
}

static void wr(volatile uint8_t *base, unsigned int off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
}

int main(int argc, char *argv[])
{
	size_t len = 4096;
	volatile uint8_t *regs;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <dev> [char | -big]\n", argv[0]);
		return 1;
	}
	if (argc > 2 && !strcmp(argv[2], "-big"))
		len = 8192;

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	regs = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (regs == MAP_FAILED) {
		printf("mmap(%zu bytes) failed: %s\n", len, strerror(errno));
		close(fd);
		return 1;
	}

	printf("mapped %zu bytes at user address %p\n", len, (void *)regs);
	printf("IER  = 0x%02x\n", rd(regs, OFF_IER) & 0xff);
	printf("LSR  = 0x%02x\n", rd(regs, OFF_LSR) & 0xff);
	printf("MDR1 = 0x%02x\n", rd(regs, OFF_MDR1) & 0xff);
	printf("SCR  = 0x%02x\n", rd(regs, OFF_SCR) & 0xff);

	if (argc > 2 && strcmp(argv[2], "-big")) {
		while (!(rd(regs, OFF_LSR) & LSR_THRE))
			;	/* busy-wait in user space, no lock, no driver */
		wr(regs, OFF_THR, (uint8_t)argv[2][0]);
		printf("wrote '%c' (0x%02x) to THR directly\n", argv[2][0], argv[2][0]);
	}

	munmap((void *)regs, len);
	close(fd);
	return 0;
}
