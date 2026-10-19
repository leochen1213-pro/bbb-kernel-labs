/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * mmap practice - layout shared by the serial driver and user space
 * (the same idea as a real driver's include/uapi/ header)
 */
#ifndef SERIAL_MMAP_UAPI_H
#define SERIAL_MMAP_UAPI_H

#include <linux/types.h>

/* mmap() offsets, in bytes (multiples of the 4 KiB page size) */
#define SERIAL_MMAP_REGS_OFF	0x0000	/* UART registers, read/write, uncached */
#define SERIAL_MMAP_STATS_OFF	0x1000	/* struct serial_shared, read-only */
#define SERIAL_MMAP_VBUF_OFF	0x2000	/* P3: vmalloc buffer, mapped on demand */

#define SERIAL_VBUF_PAGES	4

struct serial_shared {
	__u32 seq;		/* odd: update in progress */
	__u32 irq_count;
	__u32 rx_chars;
	__u32 rx_lines;
	__u32 bh_runs;
};

#endif
