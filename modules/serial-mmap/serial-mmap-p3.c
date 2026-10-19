// SPDX-License-Identifier: GPL-2.0
/*
 * mmap practice - P3: a vmalloc buffer mapped lazily with a .fault handler
 * Based on P2. Lines marked "NEW (P3)" are new.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/serial_reg.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>	/* NEW (P3): vzalloc(), vmalloc_to_page() */
#include <linux/atomic.h>	/* NEW (P3) */
#include "serial_mmap_uapi.h"

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;

	spinlock_t lock;

	unsigned int txcount;
	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;

	unsigned int irq_count;
	unsigned int sw_overflow;
	unsigned int hw_overrun;

	wait_queue_head_t wait;

	int irq;
	struct tasklet_struct rx_tasklet;
	unsigned int pending_chars;	/* protected by lock */
	unsigned int pending_lines;	/* protected by lock */
	unsigned int irq_pc;		/* preempt_count() seen in the hard IRQ */

	spinlock_t stats_lock;
	unsigned long rx_chars;		/* protected by stats_lock */
	unsigned int rx_lines;		/* protected by stats_lock */
	unsigned int tasklet_runs;	/* protected by stats_lock */
	unsigned int tasklet_pc;	/* preempt_count() seen in the tasklet */

	struct dentry *debugfs_dir;

	phys_addr_t phys;

	struct serial_shared *shared;

	/*
	 * NEW (P3): SERIAL_VBUF_PAGES pages from vmalloc: virtually
	 * contiguous for the kernel, but each page may sit anywhere in
	 * physical memory. Mapped at mmap offset SERIAL_MMAP_VBUF_OFF.
	 */
	void *vbuf;
	atomic_t vbuf_faults;	/* pages inserted by serial_vm_fault() */
	atomic_t vbuf_maps;	/* live VMAs on the buffer (mmap + open - close) */
};

static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}

static void reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}

static void serial_write_char(struct serial_dev *serial, char c)
{
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&serial->lock, flags);
		if (reg_read(serial, UART_LSR) & UART_LSR_THRE) {
			reg_write(serial, c, UART_TX);
			serial->txcount++;
			spin_unlock_irqrestore(&serial->lock, flags);
			return;
		}
		spin_unlock_irqrestore(&serial->lock, flags);
		cpu_relax();
	}
}

static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned long flags;
	char c;

	if (sz == 0)
		return 0;

retry:
	if (wait_event_interruptible(serial->wait,
				     serial->buf_rd != serial->buf_wr))
		return -ERESTARTSYS;

	spin_lock_irqsave(&serial->lock, flags);

	if (serial->buf_rd == serial->buf_wr) {
		spin_unlock_irqrestore(&serial->lock, flags);
		goto retry;
	}

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

	spin_unlock_irqrestore(&serial->lock, flags);

	if (put_user(c, buf))
		return -EFAULT;

	return 1;
}

static ssize_t serial_write(struct file *file, const char __user *buf,
			    size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	size_t i;

	for (i = 0; i < sz; i++) {
		char c;

		if (get_user(c, buf + i))
			return -EFAULT;

		serial_write_char(serial, c);
		if (c == '\n')
			serial_write_char(serial, '\r');
	}

	return sz;
}

static long serial_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned int __user *argp = (unsigned int __user *)arg;
	unsigned long flags;
	unsigned int count;

	switch (cmd) {
	case SERIAL_RESET_COUNTER:
		spin_lock_irqsave(&serial->lock, flags);
		serial->txcount = 0;
		spin_unlock_irqrestore(&serial->lock, flags);
		break;
	case SERIAL_GET_COUNTER:
		spin_lock_irqsave(&serial->lock, flags);
		count = serial->txcount;
		spin_unlock_irqrestore(&serial->lock, flags);
		if (put_user(count, argp))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

static int serial_stats_show(struct seq_file *m, void *v)
{
	struct serial_dev *serial = m->private;
	unsigned long rx_chars;
	unsigned int rx_lines, runs, tpc;

	spin_lock_bh(&serial->stats_lock);
	rx_chars = serial->rx_chars;
	rx_lines = serial->rx_lines;
	runs = serial->tasklet_runs;
	tpc = serial->tasklet_pc;
	spin_unlock_bh(&serial->stats_lock);

	seq_printf(m, "irq_count=%u tasklet_runs=%u\n",
		   READ_ONCE(serial->irq_count), runs);
	seq_printf(m, "rx_chars=%lu rx_lines=%u\n", rx_chars, rx_lines);
	seq_printf(m, "preempt_count: irq=0x%x tasklet=0x%x\n",
		   READ_ONCE(serial->irq_pc), tpc);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_stats);

/* NEW (P3): debugfs "vbuf": how many pages were faulted in, how many VMAs */
static int serial_vbuf_show(struct seq_file *m, void *v)
{
	struct serial_dev *serial = m->private;

	seq_printf(m, "faults=%d maps=%d\n", atomic_read(&serial->vbuf_faults),
		   atomic_read(&serial->vbuf_maps));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_vbuf);

/* NEW (P3): devm action, vfree() the buffer when the device goes away */
static void serial_vbuf_free(void *vbuf)
{
	vfree(vbuf);
}

/*
 * NEW (P3): vm_operations_struct for the vmalloc buffer.
 * Nothing is mapped at mmap() time: the first access to each page makes
 * the MMU raise a page fault, the core mm calls .fault, and we hand back
 * the struct page; the core inserts it in this process's page table.
 * A second access to the same page does not fault any more.
 */
static vm_fault_t serial_vm_fault(struct vm_fault *vmf)
{
	struct serial_dev *serial = vmf->vma->vm_private_data;
	/* vmf->pgoff = file offset in pages = vm_pgoff + page index in the VMA */
	unsigned long idx = vmf->pgoff - (SERIAL_MMAP_VBUF_OFF >> PAGE_SHIFT);
	struct page *page;

	if (idx >= SERIAL_VBUF_PAGES)
		return VM_FAULT_SIGBUS;

	page = vmalloc_to_page(serial->vbuf + idx * PAGE_SIZE);
	get_page(page);			/* the core drops this reference at unmap */
	vmf->page = page;

	atomic_inc(&serial->vbuf_faults);
	return 0;
}

/*
 * NEW (P3): .open/.close follow the life of each VMA. .open is NOT called
 * for the first mmap() (we count that one in serial_mmap()), but it is for
 * every copy (fork(), VMA split); .close is called for each VMA that goes
 * away (munmap(), exit). A real driver keeps its buffer alive until the
 * count drops to 0 (the P2 caveat).
 */
static void serial_vm_open(struct vm_area_struct *vma)
{
	struct serial_dev *serial = vma->vm_private_data;

	atomic_inc(&serial->vbuf_maps);
}

static void serial_vm_close(struct vm_area_struct *vma)
{
	struct serial_dev *serial = vma->vm_private_data;

	atomic_dec(&serial->vbuf_maps);
}

static const struct vm_operations_struct serial_vm_ops = {
	.fault = serial_vm_fault,
	.open = serial_vm_open,
	.close = serial_vm_close,
};

static int serial_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned long size = vma->vm_end - vma->vm_start;

	switch (vma->vm_pgoff) {
	case SERIAL_MMAP_REGS_OFF >> PAGE_SHIFT:
		if (size != PAGE_SIZE)
			return -EINVAL;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		return remap_pfn_range(vma, vma->vm_start, serial->phys >> PAGE_SHIFT,
				       size, vma->vm_page_prot);

	case SERIAL_MMAP_STATS_OFF >> PAGE_SHIFT:
		if (size != PAGE_SIZE)
			return -EINVAL;
		if (vma->vm_flags & VM_WRITE)
			return -EPERM;
		vm_flags_clear(vma, VM_MAYWRITE);
		return remap_pfn_range(vma, vma->vm_start,
				       virt_to_phys(serial->shared) >> PAGE_SHIFT,
				       size, vma->vm_page_prot);

	case SERIAL_MMAP_VBUF_OFF >> PAGE_SHIFT:
		/*
		 * NEW (P3): up to SERIAL_VBUF_PAGES pages, mapped on demand.
		 * remap_pfn_range() is not possible here: it maps ONE physically
		 * contiguous range, and vmalloc pages are not contiguous.
		 * VM_DONTEXPAND: mremap() may not grow the VMA past our buffer.
		 */
		if (size > SERIAL_VBUF_PAGES * PAGE_SIZE)
			return -EINVAL;
		vma->vm_ops = &serial_vm_ops;
		vma->vm_private_data = serial;
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		atomic_inc(&serial->vbuf_maps);		/* .open is not called for this one */
		return 0;			/* no page mapped yet */

	default:
		return -EINVAL;
	}
}

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write,
	.unlocked_ioctl = serial_ioctl,
	.mmap = serial_mmap,
};

static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	unsigned int next;
	u32 lsr;
	char c;

	spin_lock(&serial->lock);

	serial->irq_count++;
	serial->irq_pc = preempt_count();

	while ((lsr = reg_read(serial, UART_LSR)) & UART_LSR_DR) {
		if (lsr & UART_LSR_OE)
			serial->hw_overrun++;

		c = reg_read(serial, UART_RX);

		next = serial->buf_wr + 1;
		if (next >= SERIAL_BUFSIZE)
			next = 0;
		if (next == serial->buf_rd) {
			serial->sw_overflow++;
			continue;
		}

		serial->rx_buf[serial->buf_wr] = c;
		serial->buf_wr = next;

		serial->pending_chars++;
		if (c == '\n')
			serial->pending_lines++;
	}

	spin_unlock(&serial->lock);

	tasklet_schedule(&serial->rx_tasklet);

	return IRQ_HANDLED;
}

static void serial_rx_tasklet(struct tasklet_struct *t)
{
	struct serial_dev *serial = from_tasklet(serial, t, rx_tasklet);
	unsigned int pc = preempt_count();	/* SOFTIRQ_OFFSET */
	unsigned int chars, lines;
	unsigned long flags;

	/* lock is shared with the hard IRQ: irqsave even here */
	spin_lock_irqsave(&serial->lock, flags);
	chars = serial->pending_chars;
	lines = serial->pending_lines;
	serial->pending_chars = 0;
	serial->pending_lines = 0;
	spin_unlock_irqrestore(&serial->lock, flags);

	/* stats_lock is only shared with process context: plain spin_lock */
	spin_lock(&serial->stats_lock);
	serial->rx_chars += chars;
	serial->rx_lines += lines;
	serial->tasklet_runs++;
	serial->tasklet_pc = pc;

	WRITE_ONCE(serial->shared->seq, serial->shared->seq + 1);
	smp_wmb();
	WRITE_ONCE(serial->shared->irq_count, READ_ONCE(serial->irq_count));
	WRITE_ONCE(serial->shared->rx_chars, serial->rx_chars);
	WRITE_ONCE(serial->shared->rx_lines, serial->rx_lines);
	WRITE_ONCE(serial->shared->bh_runs, serial->tasklet_runs);
	smp_wmb();
	WRITE_ONCE(serial->shared->seq, serial->shared->seq + 1);
	spin_unlock(&serial->stats_lock);

	wake_up(&serial->wait);
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int uartclk, baud_divisor;
	int irq;
	int ret;
	int i;			/* NEW (P3) */

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	serial->shared = (struct serial_shared *)
		devm_get_free_pages(&pdev->dev, GFP_KERNEL | __GFP_ZERO, 0);
	if (!serial->shared)
		return -ENOMEM;

	/* NEW (P3): the buffer, each page tagged so user space can see which one it got */
	serial->vbuf = vzalloc(SERIAL_VBUF_PAGES * PAGE_SIZE);
	if (!serial->vbuf)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, serial_vbuf_free, serial->vbuf);
	if (ret)
		return ret;
	for (i = 0; i < SERIAL_VBUF_PAGES; i++)
		snprintf(serial->vbuf + i * PAGE_SIZE, 64, "page %d of %s, pfn 0x%lx",
			 i, dev_name(&pdev->dev),
			 page_to_pfn(vmalloc_to_page(serial->vbuf + i * PAGE_SIZE)));
	atomic_set(&serial->vbuf_faults, 0);
	atomic_set(&serial->vbuf_maps, 0);

	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);
	spin_lock_init(&serial->stats_lock);
	tasklet_setup(&serial->rx_tasklet, serial_rx_tasklet);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) {
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency", &uartclk);
	if (ret) {
		dev_err(&pdev->dev, "clock-frequency property not found in Device Tree\n");
		goto err_pm;
	}

	baud_divisor = uartclk / 16 / SERIAL_BAUD;
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pm;
	}

	serial->irq = irq;

	ret = devm_request_irq(&pdev->dev, irq, serial_irq, 0,
			       dev_name(&pdev->dev), serial);
	if (ret) {
		dev_err(&pdev->dev, "cannot request IRQ %d: %d\n", irq, ret);
		goto err_pm;
	}

	reg_write(serial, UART_IER_RDI, UART_IER);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_irq;
	}
	serial->phys = res->start;

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_irq;
	}

	serial->debugfs_dir = debugfs_create_dir(serial->miscdev.name, NULL);
	debugfs_create_file("stats", 0444, serial->debugfs_dir, serial,
			    &serial_stats_fops);
	debugfs_create_file("vbuf", 0444, serial->debugfs_dir, serial,
			    &serial_vbuf_fops);		/* NEW (P3) */

	dev_info(&pdev->dev, "registered /dev/%s, irq %d (mmap P3, regs at %pa)\n",
		 serial->miscdev.name, irq, &serial->phys);

	return 0;

err_irq:
	reg_write(serial, 0, UART_IER);
	synchronize_irq(irq);
	tasklet_kill(&serial->rx_tasklet);
err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "irq_count=%u sw_overflow=%u hw_overrun=%u\n",
		 serial->irq_count, serial->sw_overflow, serial->hw_overrun);

	debugfs_remove_recursive(serial->debugfs_dir);

	misc_deregister(&serial->miscdev);

	reg_write(serial, 0, UART_IER);

	synchronize_irq(serial->irq);
	tasklet_kill(&serial->rx_tasklet);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id serial_of_match[] = {
	{ .compatible = "bootlin,serial" },
	{ },
};
MODULE_DEVICE_TABLE(of, serial_of_match);

static struct platform_driver serial_driver = {
	.driver = {
		.name = "serial",
		.of_match_table = serial_of_match,
	},
	.probe = serial_probe,
	.remove = serial_remove,
};
module_platform_driver(serial_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("mmap practice P3: vmalloc buffer mapped on demand with .fault");
