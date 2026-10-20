// SPDX-License-Identifier: GPL-2.0
/*
 * DMA supplement - X2: streaming DMA mappings (kmalloc + dma_map_single)
 * Based on X1. Lines marked "NEW (X2)" are new.
 *
 * Each iteration plays the real driver pattern:
 *   CPU writes src (stamp)  -> give src to the device (cache CLEAN)
 *   EDMA copies src -> dst
 *   give dst back to the CPU (cache INVALIDATE) -> CPU checks the stamp
 *
 *   insmod dmamemcpy-x2.ko size=65536 iters=20 reuse=0   map/unmap each copy
 *   insmod dmamemcpy-x2.ko size=65536 iters=20 reuse=1   map once, sync each copy
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/string.h>

static unsigned int size = 65536;
module_param(size, uint, 0444);
MODULE_PARM_DESC(size, "bytes per copy (default 65536)");

static unsigned int iters = 20;
module_param(iters, uint, 0444);
MODULE_PARM_DESC(iters, "copies per measurement (default 20)");

/* NEW (X2): 0 = dma_map/unmap around every copy, 1 = map once + dma_sync_* */
static unsigned int reuse;
module_param(reuse, uint, 0444);
MODULE_PARM_DESC(reuse, "0: map/unmap per copy, 1: map once and sync per copy");

static struct dma_chan *chan;
static struct device *dev;
static u32 *src, *dst;			/* NEW (X2): plain kmalloc(), cached */
static struct completion done;

static void x2_callback(void *param)
{
	complete(param);
}

static int x2_dma_copy(dma_addr_t d, dma_addr_t s)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;

	tx = dmaengine_prep_dma_memcpy(chan, d, s, size,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx)
		return -EIO;

	reinit_completion(&done);
	tx->callback = x2_callback;
	tx->callback_param = &done;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;
	dma_async_issue_pending(chan);

	if (!wait_for_completion_timeout(&done, msecs_to_jiffies(1000))) {
		dmaengine_terminate_sync(chan);
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * NEW (X2): one iteration with a fresh mapping each time.
 *  dma_map_single(TO_DEVICE)   : CPU cache CLEANED (dirty lines written to
 *                                RAM), src now owned by the device
 *  dma_map_single(FROM_DEVICE) : dst lines INVALIDATED, owned by the device
 *  ... DMA ...
 *  dma_unmap_single(FROM_DEVICE): invalidated again (speculative prefetch
 *                                may have refilled lines meanwhile), dst
 *                                back to the CPU
 * Between map and unmap the CPU must not touch the buffer.
 */
static int x2_once_map(u32 stamp)
{
	dma_addr_t s, d;
	int ret;

	src[0] = stamp;				/* CPU writes BEFORE mapping */
	src[size / 4 - 1] = ~stamp;

	s = dma_map_single(dev, src, size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, s))
		return -ENOMEM;
	d = dma_map_single(dev, dst, size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, d)) {
		dma_unmap_single(dev, s, size, DMA_TO_DEVICE);
		return -ENOMEM;
	}

	ret = x2_dma_copy(d, s);

	dma_unmap_single(dev, d, size, DMA_FROM_DEVICE);
	dma_unmap_single(dev, s, size, DMA_TO_DEVICE);
	if (ret)
		return ret;

	/* CPU reads AFTER unmapping */
	return (dst[0] == stamp && dst[size / 4 - 1] == ~stamp) ? 0 : -EIO;
}

/*
 * NEW (X2): same thing with long-lived mappings: the cache maintenance is
 * done by dma_sync_single_for_device() / _for_cpu(), without the cost of
 * creating and destroying the mapping (matters with an IOMMU or swiotlb).
 */
static int x2_once_sync(dma_addr_t s, dma_addr_t d, u32 stamp)
{
	int ret;

	dma_sync_single_for_cpu(dev, s, size, DMA_TO_DEVICE);	/* src back to CPU */
	src[0] = stamp;
	src[size / 4 - 1] = ~stamp;
	dma_sync_single_for_device(dev, s, size, DMA_TO_DEVICE);	/* clean */
	dma_sync_single_for_device(dev, d, size, DMA_FROM_DEVICE);	/* invalidate */

	ret = x2_dma_copy(d, s);
	if (ret)
		return ret;

	dma_sync_single_for_cpu(dev, d, size, DMA_FROM_DEVICE);	/* invalidate */
	return (dst[0] == stamp && dst[size / 4 - 1] == ~stamp) ? 0 : -EIO;
}

static int __init x2_init(void)
{
	dma_cap_mask_t mask;
	dma_addr_t s = 0, d = 0;
	ktime_t t0;
	s64 ns;
	unsigned int i;
	int ret;

	if (!size || size % 4 || !iters)
		return -EINVAL;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan))
		return PTR_ERR(chan);
	dev = chan->device->dev;

	init_completion(&done);

	src = kmalloc(size, GFP_KERNEL);
	dst = kmalloc(size, GFP_KERNEL);
	if (!src || !dst) {
		ret = -ENOMEM;
		goto err;
	}
	for (i = 0; i < size / 4; i++)
		src[i] = 0xa5a50000 | i;

	/* NEW (X2): with reuse=1, map once for the whole run */
	if (reuse) {
		s = dma_map_single(dev, src, size, DMA_TO_DEVICE);
		d = dma_map_single(dev, dst, size, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, s) || dma_mapping_error(dev, d)) {
			ret = -ENOMEM;
			goto err;
		}
	}

	t0 = ktime_get();
	for (i = 0; i < iters; i++) {
		ret = reuse ? x2_once_sync(s, d, i) : x2_once_map(i);
		if (ret) {
			pr_err("x2: iteration %u failed: %d\n", i, ret);
			goto err;
		}
	}
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	pr_info("x2: %s, size %u: %lld us/copy, %u iterations verified\n",
		reuse ? "map once + sync" : "map/unmap each copy", size,
		div_s64(ns, iters * 1000), iters);

	if (reuse) {
		dma_unmap_single(dev, d, size, DMA_FROM_DEVICE);
		dma_unmap_single(dev, s, size, DMA_TO_DEVICE);
	}
	return 0;

err:
	if (reuse && s && !dma_mapping_error(dev, s))
		dma_unmap_single(dev, s, size, DMA_TO_DEVICE);
	if (reuse && d && !dma_mapping_error(dev, d))
		dma_unmap_single(dev, d, size, DMA_FROM_DEVICE);
	kfree(src);
	kfree(dst);
	dma_release_channel(chan);
	return ret;
}

static void __exit x2_exit(void)
{
	dmaengine_terminate_sync(chan);
	kfree(src);
	kfree(dst);
	dma_release_channel(chan);
	pr_info("x2: released\n");
}

module_init(x2_init);
module_exit(x2_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA supplement X2: streaming mappings, map/unmap vs sync");
