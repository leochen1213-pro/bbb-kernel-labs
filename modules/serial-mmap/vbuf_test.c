// SPDX-License-Identifier: GPL-2.0
/*
 * mmap practice - P3 user-space test: watch pages being faulted in
 *
 *   vbuf_test <dev> <debugfs vbuf file>
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -Wall -I. -o vbuf_test vbuf_test.c
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include "serial_mmap_uapi.h"

static const char *dbg;

static void show(const char *what)
{
	char line[64] = "";
	FILE *f = fopen(dbg, "r");

	if (f) {
		if (!fgets(line, sizeof(line), f))
			line[0] = 0;
		fclose(f);
	}
	line[strcspn(line, "\n")] = 0;
	printf("%-34s -> %s\n", what, line);
}

int main(int argc, char *argv[])
{
	size_t len = SERIAL_VBUF_PAGES * 4096;
	volatile char *buf;
	char c;
	int fd;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <dev> <debugfs vbuf file>\n", argv[0]);
		return 1;
	}
	dbg = argv[2];

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	show("before mmap");
	buf = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, SERIAL_MMAP_VBUF_OFF);
	if (buf == MAP_FAILED) {
		printf("mmap failed: %s\n", strerror(errno));
		return 1;
	}
	show("after mmap (nothing touched yet)");

	c = buf[0];
	show("read page 0");
	c = buf[100];
	show("read page 0 again");
	c = buf[2 * 4096];
	show("read page 2");
	(void)c;

	printf("page 0 says: \"%s\"\n", (const char *)buf);
	printf("page 2 says: \"%s\"\n", (const char *)buf + 2 * 4096);
	printf("page 3 says: \"%s\"\n", (const char *)buf + 3 * 4096);
	show("after printing page 3");

	munmap((void *)buf, len);
	show("after munmap");

	buf = mmap(NULL, len + 4096, PROT_READ, MAP_SHARED, fd, SERIAL_MMAP_VBUF_OFF);
	printf("mmap of %d pages: %s\n", SERIAL_VBUF_PAGES + 1,
	       buf == MAP_FAILED ? strerror(errno) : "accepted?!");

	close(fd);
	return 0;
}
