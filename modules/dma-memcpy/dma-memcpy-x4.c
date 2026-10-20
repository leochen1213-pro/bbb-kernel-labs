// SPDX-License-Identifier: GPL-2.0
/*
 * DMA supplement - X4: let CONFIG_DMA_API_DEBUG catch DMA API misuse
 * Based on X2/X3 (no transfer needed: map/unmap is what is checked).
 * Lines marked "NEW (X4)" are new.
 *
 *   insmod dmamemcpy-x4.ko bug=N
 *     0  correct map/unmap (reference, must be silent)
 *     1  unmap with a different size
 *     2  unmap with a different direction
 *     3  map a buffer on the stack
 *     4  map a vmalloc() buffer
 *     5  never unmap (leak): look at /sys/kernel/debug/dma-api/dump
 *     6  unmap the same mapping twice
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define X4_SIZE	4096

static unsigned int bug;
module_param(bug, uint, 0444);
MODULE_PARM_DESC(bug, "0 ok, 1 size, 2 direction, 3 stack, 4 vmalloc, 5 leak, 6 double unmap");

static struct dma_chan *chan;
static struct device *dev;
static void *buf;

/* NEW (X4): noinline so the stack buffer really lives in this frame */
static noinline void x4_stack_map(void)
{
	u8 stackbuf[64];
	dma_addr_t a;

	a = dma_map_single(dev, stackbuf, sizeof(stackbuf), DMA_TO_DEVICE);
	if (!dma_mapping_error(dev, a))
		dma_unmap_single(dev, a, sizeof(stackbuf), DMA_TO_DEVICE);
}

static int __init x4_init(void)
{
	dma_cap_mask_t mask;
	dma_addr_t a;
	void *vbuf;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan))
		return PTR_ERR(chan);
	dev = chan->device->dev;

	buf = kmalloc(X4_SIZE, GFP_KERNEL);
	if (!buf) {
		dma_release_channel(chan);
		return -ENOMEM;
	}

	pr_info("x4: bug=%u on %s\n", bug, dev_name(dev));

	switch (bug) {
	case 0:	/* reference: same pointer, size and direction both ways */
		a = dma_map_single(dev, buf, X4_SIZE, DMA_TO_DEVICE);
		if (!dma_mapping_error(dev, a))
			dma_unmap_single(dev, a, X4_SIZE, DMA_TO_DEVICE);
		break;
	case 1:	/* NEW (X4): unmap must use the size given to map */
		a = dma_map_single(dev, buf, X4_SIZE, DMA_TO_DEVICE);
		if (!dma_mapping_error(dev, a))
			dma_unmap_single(dev, a, X4_SIZE / 2, DMA_TO_DEVICE);
		break;
	case 2:	/* NEW (X4): ... and the direction (it decides the cache op) */
		a = dma_map_single(dev, buf, X4_SIZE, DMA_TO_DEVICE);
		if (!dma_mapping_error(dev, a))
			dma_unmap_single(dev, a, X4_SIZE, DMA_FROM_DEVICE);
		break;
	case 3:	/* NEW (X4): stack: may be vmalloc'ed, shares cache lines */
		x4_stack_map();
		break;
	case 4:	/* NEW (X4): vmalloc: not physically contiguous (P3) */
		vbuf = vmalloc(X4_SIZE);
		if (vbuf) {
			a = dma_map_single(dev, vbuf, X4_SIZE, DMA_TO_DEVICE);
			pr_info("x4: vmalloc map %s\n",
				dma_mapping_error(dev, a) ? "REFUSED" : "accepted?!");
			if (!dma_mapping_error(dev, a))
				dma_unmap_single(dev, a, X4_SIZE, DMA_TO_DEVICE);
			vfree(vbuf);
		}
		break;
	case 5:	/* NEW (X4): mapped, never unmapped */
		a = dma_map_single(dev, buf, X4_SIZE, DMA_TO_DEVICE);
		pr_info("x4: leaked mapping at %pad\n", &a);
		break;
	case 6:	/* NEW (X4): second unmap of the same address */
		a = dma_map_single(dev, buf, X4_SIZE, DMA_TO_DEVICE);
		if (!dma_mapping_error(dev, a)) {
			dma_unmap_single(dev, a, X4_SIZE, DMA_TO_DEVICE);
			dma_unmap_single(dev, a, X4_SIZE, DMA_TO_DEVICE);
		}
		break;
	}

	return 0;
}

static void __exit x4_exit(void)
{
	kfree(buf);
	dma_release_channel(chan);
	pr_info("x4: released\n");
}

module_init(x4_init);
module_exit(x4_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA supplement X4: DMA API misuse caught by DMA_API_DEBUG");
