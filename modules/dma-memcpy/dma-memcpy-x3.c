// SPDX-License-Identifier: GPL-2.0
/*
 * DMA supplement - X3: what goes wrong WITHOUT the DMA API cache maintenance
 * Based on X2. Lines marked "NEW (X3)" are new.
 *
 *   insmod dmamemcpy-x3.ko size=4096 proper=0   virt_to_phys(), no cache ops
 *   insmod dmamemcpy-x3.ko size=4096 proper=1   dma_map_single() / unmap
 *
 * The CPU fills src with a pattern and dst with 0xdeadbeef. Both are
 * written through the (write-back) data cache, so the new values may still
 * sit in dirty cache lines and not be in RAM yet. The EDMA only sees RAM.
 * After the copy, every dst word is classified:
 *   good  : the src pattern       (what we want)
 *   stale : still 0xdeadbeef      (CPU reads its own cache, not the DMA data)
 *   other : neither               (DMA copied old RAM content of src)
 * WARNING: proper=0 is a deliberate bug (and only "works" because AM335x has
 * no IOMMU and dma_addr_t == physical address). Never do this in a driver.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/io.h>			/* NEW (X3): virt_to_phys() */

static unsigned int size = 4096;
module_param(size, uint, 0444);
MODULE_PARM_DESC(size, "bytes per copy (default 4096)");

/* NEW (X3) */
static unsigned int proper;
module_param(proper, uint, 0444);
MODULE_PARM_DESC(proper, "0: bypass the DMA API (bug), 1: dma_map_single()");

static struct dma_chan *chan;
static struct device *dev;
static u32 *src, *dst;
static struct completion done;

static void x3_callback(void *param)
{
	complete(param);
}

static int x3_dma_copy(dma_addr_t d, dma_addr_t s)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;

	tx = dmaengine_prep_dma_memcpy(chan, d, s, size,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx)
		return -EIO;

	reinit_completion(&done);
	tx->callback = x3_callback;
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

static int __init x3_init(void)
{
	unsigned int i, good = 0, stale = 0, other = 0, first_bad = ~0u;
	dma_cap_mask_t mask;
	dma_addr_t s, d;
	int ret;

	if (!size || size % 4)
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

	/* NEW (X3): CPU writes, through the cache */
	for (i = 0; i < size / 4; i++) {
		src[i] = 0x11110000 | (i & 0xffff);
		dst[i] = 0xdeadbeef;
	}

	if (proper) {
		/* clean src, invalidate dst: RAM is what the CPU wrote */
		s = dma_map_single(dev, src, size, DMA_TO_DEVICE);
		d = dma_map_single(dev, dst, size, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, s) || dma_mapping_error(dev, d)) {
			ret = -ENOMEM;
			goto err;
		}
	} else {
		/*
		 * NEW (X3): THE BUG. A physical address handed to the device,
		 * no cache maintenance at all. DMA_API_DEBUG cannot see this:
		 * nothing was mapped, so there is nothing to check.
		 */
		s = virt_to_phys(src);
		d = virt_to_phys(dst);
	}

	ret = x3_dma_copy(d, s);

	if (proper) {
		dma_unmap_single(dev, d, size, DMA_FROM_DEVICE);
		dma_unmap_single(dev, s, size, DMA_TO_DEVICE);
	}
	if (ret)
		goto err;

	/* NEW (X3): what does the CPU see in dst now? */
	for (i = 0; i < size / 4; i++) {
		u32 want = 0x11110000 | (i & 0xffff);

		if (dst[i] == want) {
			good++;
		} else {
			if (dst[i] == 0xdeadbeef)
				stale++;
			else
				other++;
			if (first_bad == ~0u)
				first_bad = i;
		}
	}

	pr_info("x3: %s, size %u: good=%u stale(0xdeadbeef)=%u other=%u\n",
		proper ? "dma_map_single" : "virt_to_phys, NO cache ops", size,
		good, stale, other);
	if (first_bad != ~0u)
		pr_info("x3: first bad word %u: got 0x%08x, want 0x%08x\n",
			first_bad, dst[first_bad], 0x11110000 | (first_bad & 0xffff));

	return 0;

err:
	kfree(src);
	kfree(dst);
	src = dst = NULL;
	dma_release_channel(chan);
	return ret;
}

static void __exit x3_exit(void)
{
	dmaengine_terminate_sync(chan);
	kfree(src);
	kfree(dst);
	dma_release_channel(chan);
	pr_info("x3: released\n");
}

module_init(x3_init);
module_exit(x3_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA supplement X3: stale data without DMA API cache maintenance");
