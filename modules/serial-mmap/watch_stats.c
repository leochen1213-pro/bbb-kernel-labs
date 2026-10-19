// SPDX-License-Identifier: GPL-2.0
/*
 * mmap practice - P2 user-space test
 *
 *   watch_stats <dev> [seconds]   map the statistics page read-only and
 *                                 print every new snapshot (no read()!)
 *   watch_stats <dev> -w          try to map it writable (must fail)
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -Wall -o watch_stats watch_stats.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include "serial_mmap_uapi.h"

/* Seqcount reader: retry until we saw the same EVEN seq before and after */
static unsigned int snapshot(const volatile struct serial_shared *sh,
			     struct serial_shared *out)
{
	unsigned int retries = 0;
	__u32 s1, s2;

	for (;;) {
		s1 = __atomic_load_n(&sh->seq, __ATOMIC_ACQUIRE);
		if (s1 & 1) {			/* writer in the middle of an update */
			retries++;
			continue;
		}
		out->irq_count = sh->irq_count;
		out->rx_chars = sh->rx_chars;
		out->rx_lines = sh->rx_lines;
		out->bh_runs = sh->bh_runs;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);	/* data before 2nd seq */
		s2 = __atomic_load_n(&sh->seq, __ATOMIC_RELAXED);
		if (s1 == s2)
			break;
		retries++;			/* changed while we were reading */
	}
	out->seq = s1;
	return retries;
}

int main(int argc, char *argv[])
{
	int prot = PROT_READ, secs = 10, i, fd;
	const volatile struct serial_shared *sh;
	struct serial_shared snap;
	__u32 last = ~0u;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <dev> [seconds | -w]\n", argv[0]);
		return 1;
	}
	if (argc > 2 && !strcmp(argv[2], "-w"))
		prot |= PROT_WRITE;
	else if (argc > 2)
		secs = atoi(argv[2]);

	fd = open(argv[1], (prot & PROT_WRITE) ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	sh = mmap(NULL, 4096, prot, MAP_SHARED, fd, SERIAL_MMAP_STATS_OFF);
	if (sh == MAP_FAILED) {
		printf("mmap(stats, %s) failed: %s\n",
		       (prot & PROT_WRITE) ? "PROT_READ|PROT_WRITE" : "PROT_READ",
		       strerror(errno));
		close(fd);
		return 1;
	}

	printf("stats page mapped read-only at %p, watching for %d s\n", (void *)sh, secs);
	for (i = 0; i < secs * 10; i++) {
		unsigned int retries = snapshot(sh, &snap);

		if (snap.seq != last) {
			printf("seq=%-4u irq_count=%-4u rx_chars=%-4u rx_lines=%-3u bh_runs=%-4u retries=%u\n",
			       snap.seq, snap.irq_count, snap.rx_chars,
			       snap.rx_lines, snap.bh_runs, retries);
			last = snap.seq;
		}
		usleep(100000);		/* poll every 100 ms, no system call to the driver */
	}

	munmap((void *)sh, 4096);
	close(fd);
	return 0;
}
