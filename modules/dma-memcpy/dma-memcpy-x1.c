// SPDX-License-Identifier: GPL-2.0
/*
 * DMA supplement - X1: a mem-to-mem dmaengine client on the AM335x EDMA
 *
 * Not a device driver: on load it asks the dmaengine core for ANY channel
 * able to do DMA_MEMCPY, copies a coherent buffer with it, checks the
 * result and compares the time with CPU memcpy() on:
 *   - the same coherent buffers (uncached on ARMv7)
 *   - normal kmalloc() buffers (cached)
 * Then it stays loaded doing nothing; rmmod releases everything.
 *
 *   insmod dmamemcpy-x1.ko size=65536 iters=20
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

static struct dma_chan *chan;
static void *src, *dst;			/* coherent buffers, CPU addresses */
static dma_addr_t src_dma, dst_dma;	/* the same buffers, as the EDMA sees them */
static struct completion done;

static void x1_callback(void *param)
{
	complete(param);		/* EDMA completion path: atomic, no sleep */
}

/* One DMA copy src -> dst, sleeping until the completion callback */
static int x1_dma_copy(void)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;

	tx = dmaengine_prep_dma_memcpy(chan, dst_dma, src_dma, size,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx)
		return -EIO;

	reinit_completion(&done);
	tx->callback = x1_callback;
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

/* MB/s from bytes copied and elapsed ns (1 MB = 10^6 bytes) */
static unsigned int mbps(u64 bytes, s64 ns)
{
	return ns > 0 ? (unsigned int)div64_s64(bytes * 1000, ns) : 0;
}

static int __init x1_init(void)
{
	struct device *dev;
	dma_cap_mask_t mask;
	void *ksrc, *kdst;
	ktime_t t0;
	s64 ns_dma, ns_cpu_coh, ns_cpu_kmem;
	unsigned int i;
	int ret;

	if (!size || size % 4 || !iters)
		return -EINVAL;

	/* 1. Any channel with the MEMCPY capability (no DT "dmas" needed) */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(chan)) {
		ret = PTR_ERR(chan);
		pr_err("x1: no DMA_MEMCPY channel: %pe\n", ERR_PTR(ret));
		return ret;
	}

	/*
	 * 2. Map with the DMA controller's own struct device: it is the
	 * EDMA that accesses the memory, so its DMA constraints apply.
	 */
	dev = chan->device->dev;
	pr_info("x1: channel %s on %s, size %u, iters %u\n",
		dma_chan_name(chan), dev_name(dev), size, iters);

	init_completion(&done);

	src = dma_alloc_coherent(dev, size, &src_dma, GFP_KERNEL);
	dst = dma_alloc_coherent(dev, size, &dst_dma, GFP_KERNEL);
	ksrc = kmalloc(size, GFP_KERNEL);
	kdst = kmalloc(size, GFP_KERNEL);
	if (!src || !dst || !ksrc || !kdst) {
		ret = -ENOMEM;
		goto err;
	}
	pr_info("x1: src cpu %px dma %pad, dst cpu %px dma %pad\n",
		src, &src_dma, dst, &dst_dma);

	for (i = 0; i < size / 4; i++)
		((u32 *)src)[i] = 0xa5a50000 | i;
	memset(dst, 0, size);
	memcpy(ksrc, src, size);

	/* 3. DMA: one warm-up copy, check it, then measure */
	ret = x1_dma_copy();
	if (ret)
		goto err;
	if (memcmp(src, dst, size)) {
		pr_err("x1: DMA copy MISMATCH\n");
		ret = -EIO;
		goto err;
	}
	pr_info("x1: DMA copy verified (%u bytes)\n", size);

	t0 = ktime_get();
	for (i = 0; i < iters; i++) {
		ret = x1_dma_copy();
		if (ret)
			goto err;
	}
	ns_dma = ktime_to_ns(ktime_sub(ktime_get(), t0));

	/* 4. CPU memcpy() on the SAME coherent (uncached) buffers */
	t0 = ktime_get();
	for (i = 0; i < iters; i++)
		memcpy(dst, src, size);
	ns_cpu_coh = ktime_to_ns(ktime_sub(ktime_get(), t0));

	/* 5. CPU memcpy() on normal cached kmalloc() buffers */
	t0 = ktime_get();
	for (i = 0; i < iters; i++)
		memcpy(kdst, ksrc, size);
	ns_cpu_kmem = ktime_to_ns(ktime_sub(ktime_get(), t0));

	pr_info("x1: DMA            %6lld us/copy %4u MB/s\n",
		div_s64(ns_dma, iters * 1000), mbps((u64)size * iters, ns_dma));
	pr_info("x1: CPU coherent   %6lld us/copy %4u MB/s\n",
		div_s64(ns_cpu_coh, iters * 1000), mbps((u64)size * iters, ns_cpu_coh));
	pr_info("x1: CPU kmalloc    %6lld us/copy %4u MB/s\n",
		div_s64(ns_cpu_kmem, iters * 1000), mbps((u64)size * iters, ns_cpu_kmem));

	kfree(ksrc);
	kfree(kdst);
	return 0;

err:
	kfree(ksrc);
	kfree(kdst);
	if (dst)
		dma_free_coherent(dev, size, dst, dst_dma);
	if (src)
		dma_free_coherent(dev, size, src, src_dma);
	dma_release_channel(chan);
	return ret;
}

static void __exit x1_exit(void)
{
	struct device *dev = chan->device->dev;

	dmaengine_terminate_sync(chan);
	dma_free_coherent(dev, size, dst, dst_dma);
	dma_free_coherent(dev, size, src, src_dma);
	dma_release_channel(chan);
	pr_info("x1: released\n");
}

module_init(x1_init);
module_exit(x1_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA supplement X1: mem-to-mem dmaengine client, DMA vs CPU memcpy");
