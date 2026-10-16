// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupts lab review - I4 (custom, not in the Bootlin lab)
 * Stress the I3 design and SEE where data gets lost:
 *
 *   A. Software buffer wrap : >= 16 chars while nobody reads
 *   B. Reader starvation    : long write(), reader cannot run (non-preemptible
 *                             kernel, busy-polling writer) -> wrap even with cat open
 *   C. Hardware overrun     : interrupt latency > 1 char time -> LSR.OE
 *
 * Module parameters (default = I3 behaviour):
 *   drop_full=1   detect a full buffer and drop the NEW char (instead of wrapping)
 *   yield_tx=1    cond_resched() in the write() loop
 *   latency_us=N  busy-wait N us in the handler BEFORE touching the UART
 *                 (simulates interrupt latency, e.g. another long IRQs-off section)
 *
 * Based on I3. Lines marked "NEW (I4)" are new in this step.
 * Still NO LOCKING (next lab).
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
#include <linux/delay.h>		/* NEW (I4): udelay() */
#include <linux/sched.h>		/* NEW (I4): cond_resched() */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

/* NEW (I4): experiment switches, read-only once loaded (0444) */
static bool drop_full;
module_param(drop_full, bool, 0444);
MODULE_PARM_DESC(drop_full, "1 = drop new char when the buffer is full (default: wrap/overwrite as in the lab)");

static bool yield_tx;
module_param(yield_tx, bool, 0444);
MODULE_PARM_DESC(yield_tx, "1 = cond_resched() after each char in write()");

static unsigned int latency_us;
module_param(latency_us, uint, 0444);
MODULE_PARM_DESC(latency_us, "busy-wait in the IRQ handler before reading the UART (max 2000)");

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;
	unsigned int txcount;
	unsigned int irq_count;

	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;
	wait_queue_head_t wait;

	/* NEW (I4): where did the data go? (printed in remove()) */
	unsigned int sw_overflow;	/* char arrived while the buffer was full */
	unsigned int hw_overrun;	/* LSR.OE seen: the UART lost a char */
	unsigned int yields;		/* cond_resched() really switched to another task */
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
	while (!(reg_read(serial, UART_LSR) & UART_LSR_THRE))
		cpu_relax();

	reg_write(serial, c, UART_TX);
	serial->txcount++;
}

static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	char c;

	if (sz == 0)
		return 0;

	if (wait_event_interruptible(serial->wait,
				     serial->buf_rd != serial->buf_wr))
		return -ERESTARTSYS;

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

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

		/*
		 * NEW (I4) experiment B: the kernel is NOT preemptible, and this
		 * loop busy-polls LSR without ever sleeping. wake_up() in the
		 * handler only marks the reader runnable (and sets need_resched);
		 * the reader really runs only when we give the CPU away.
		 * cond_resched() returns 1 if it actually scheduled.
		 */
		if (yield_tx && cond_resched())
			serial->yields++;
	}

	return sz;
}

static long serial_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned int __user *argp = (unsigned int __user *)arg;

	switch (cmd) {
	case SERIAL_RESET_COUNTER:
		serial->txcount = 0;
		break;
	case SERIAL_GET_COUNTER:
		if (put_user(serial->txcount, argp))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write,
	.unlocked_ioctl = serial_ioctl,
};

static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	unsigned int next;		/* NEW (I4) */
	u32 lsr;			/* NEW (I4) */
	char c;

	serial->irq_count++;

	/*
	 * NEW (I4) experiment C: pretend the CPU reached this handler late.
	 * udelay() busy-waits (never sleeps), so it is legal in hard IRQ
	 * context, just very bad practice beyond a few us.
	 */
	if (latency_us)
		udelay(min(latency_us, 2000U));

	/* NEW (I4): LSR first: OE is only visible here (reading LSR clears it) */
	lsr = reg_read(serial, UART_LSR);
	if (lsr & UART_LSR_OE)
		serial->hw_overrun++;

	c = reg_read(serial, UART_RX);		/* ack */

	/*
	 * NEW (I4) experiment A: is the buffer full?
	 * With rd == wr meaning "empty", the buffer is "full" when writing one
	 * more char would make wr catch up with rd -> only 15 usable slots.
	 */
	next = serial->buf_wr + 1;
	if (next >= SERIAL_BUFSIZE)
		next = 0;

	if (next == serial->buf_rd) {
		serial->sw_overflow++;
		if (drop_full)
			goto out;	/* keep the 15 old chars, lose the new one */
		/* else: lab behaviour, wr catches rd -> buffer looks EMPTY */
	}

	serial->rx_buf[serial->buf_wr] = c;
	serial->buf_wr = next;

	wake_up(&serial->wait);
out:
	return IRQ_HANDLED;
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

	/* 115200 8N1 */
	baud_divisor = uartclk / 16 / SERIAL_BAUD;
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	/* Clear UART FIFOs (FIFOs stay DISABLED: 1-char RHR/THR) */
	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pm;
	}

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

	/* NEW (I4): show which experiment is running */
	dev_info(&pdev->dev, "/dev/%s irq %d (drop_full=%d yield_tx=%d latency_us=%u)\n",
		 serial->miscdev.name, irq, drop_full, yield_tx, latency_us);

	return 0;

err_irq:
	reg_write(serial, 0, UART_IER);
err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);

	/* NEW (I4): experiment report */
	dev_info(&pdev->dev, "irq_count=%u sw_overflow=%u hw_overrun=%u yields=%u\n",
		 serial->irq_count, serial->sw_overflow, serial->hw_overrun,
		 serial->yields);

	misc_deregister(&serial->miscdev);

	reg_write(serial, 0, UART_IER);

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
MODULE_DESCRIPTION("Interrupts lab review I4: buffer wrap, reader starvation, HW overrun");
