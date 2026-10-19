// SPDX-License-Identifier: GPL-2.0
/*
 * Bottom-half practice - B3: tasklet vs workqueue (use_work=), sleeping
 * in the bottom half (bug=2)
 * Based on B2. Lines marked "NEW (B3)" are new.
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
#include <linux/workqueue.h>	/* NEW (B3) */
#include <linux/delay.h>	/* NEW (B3): msleep() for bug=2 */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

static int bug;
module_param(bug, int, 0444);
MODULE_PARM_DESC(bug, "1: debugfs stats taken with spin_lock() (missing _bh), "
		 "2: msleep(1) in the bottom half");	/* NEW (B3): bug=2 */

/* NEW (B3): which bottom half: 0 = tasklet (softirq), 1 = workqueue (kworker) */
static int use_work;
module_param(use_work, int, 0444);
MODULE_PARM_DESC(use_work, "0: tasklet, 1: system workqueue");

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
	struct work_struct rx_work;	/* NEW (B3) */
	unsigned int pending_chars;	/* protected by lock */
	unsigned int pending_lines;	/* protected by lock */
	unsigned int irq_pc;		/* preempt_count() seen in the hard IRQ */

	spinlock_t stats_lock;
	unsigned long rx_chars;		/* protected by stats_lock */
	unsigned int rx_lines;		/* protected by stats_lock */
	unsigned int bh_runs;		/* NEW (B3): was tasklet_runs */
	unsigned int bh_pc;		/* NEW (B3): was tasklet_pc */

	struct dentry *debugfs_dir;
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

	if (bug == 1)
		spin_lock(&serial->stats_lock);
	else
		spin_lock_bh(&serial->stats_lock);
	rx_chars = serial->rx_chars;
	rx_lines = serial->rx_lines;
	runs = serial->bh_runs;
	tpc = serial->bh_pc;
	if (bug == 1)
		spin_unlock(&serial->stats_lock);
	else
		spin_unlock_bh(&serial->stats_lock);

	seq_printf(m, "bottom half: %s\n", use_work ? "workqueue" : "tasklet");	/* NEW (B3) */
	seq_printf(m, "irq_count=%u bh_runs=%u\n",
		   READ_ONCE(serial->irq_count), runs);
	seq_printf(m, "rx_chars=%lu rx_lines=%u\n", rx_chars, rx_lines);
	seq_printf(m, "preempt_count: irq=0x%x bh=0x%x\n",
		   READ_ONCE(serial->irq_pc), tpc);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_stats);

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write,
	.unlocked_ioctl = serial_ioctl,
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

	/*
	 * NEW (B3): schedule_work() is just as safe from hard IRQ context as
	 * tasklet_schedule(), and folds the same way: a work item already
	 * pending is not queued twice.
	 */
	if (use_work)
		schedule_work(&serial->rx_work);
	else
		tasklet_schedule(&serial->rx_tasklet);

	return IRQ_HANDLED;
}

/*
 * NEW (B3): the bottom-half body, shared by the tasklet and the work item.
 * Called in softirq context (tasklet) or in process context (kworker).
 */
static void serial_rx_process(struct serial_dev *serial)
{
	unsigned int pc = preempt_count();	/* tasklet: SOFTIRQ_OFFSET, work: 0 */
	unsigned int chars, lines;
	unsigned long flags;

	/* lock is shared with the hard IRQ: irqsave even here */
	spin_lock_irqsave(&serial->lock, flags);
	chars = serial->pending_chars;
	lines = serial->pending_lines;
	serial->pending_chars = 0;
	serial->pending_lines = 0;
	spin_unlock_irqrestore(&serial->lock, flags);

	/*
	 * NEW (B3): spin_lock_bh() now, because this code also runs in
	 * process context (kworker), where softirqs are enabled. It is also
	 * fine inside the tasklet (BH already disabled, the call just nests).
	 */
	spin_lock_bh(&serial->stats_lock);
	serial->rx_chars += chars;
	serial->rx_lines += lines;
	serial->bh_runs++;
	serial->bh_pc = pc;
	spin_unlock_bh(&serial->stats_lock);

	/*
	 * NEW (B3): bug=2. msleep() may sleep: allowed in a work item
	 * (process context), forbidden in a tasklet (softirq, atomic).
	 * CONFIG_DEBUG_ATOMIC_SLEEP catches it in might_sleep().
	 */
	if (bug == 2)
		msleep(1);

	wake_up(&serial->wait);
}

static void serial_rx_tasklet(struct tasklet_struct *t)
{
	struct serial_dev *serial = from_tasklet(serial, t, rx_tasklet);

	serial_rx_process(serial);
}

/* NEW (B3): work handler, runs in a kworker thread (process context) */
static void serial_rx_work(struct work_struct *work)
{
	struct serial_dev *serial = container_of(work, struct serial_dev, rx_work);

	serial_rx_process(serial);
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int uartclk, baud_divisor;
	int irq;
	int ret;

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);
	spin_lock_init(&serial->stats_lock);
	tasklet_setup(&serial->rx_tasklet, serial_rx_tasklet);
	INIT_WORK(&serial->rx_work, serial_rx_work);	/* NEW (B3): before request_irq() */

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

	dev_info(&pdev->dev, "registered /dev/%s, irq %d (bottom half B3: %s, bug=%d)\n",
		 serial->miscdev.name, irq, use_work ? "workqueue" : "tasklet", bug);

	return 0;

err_irq:
	reg_write(serial, 0, UART_IER);
	synchronize_irq(irq);
	tasklet_kill(&serial->rx_tasklet);
	cancel_work_sync(&serial->rx_work);	/* NEW (B3): waits for a running work */
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
	cancel_work_sync(&serial->rx_work);	/* NEW (B3): waits for a running work */

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
MODULE_DESCRIPTION("Bottom-half practice B3: tasklet vs workqueue");
