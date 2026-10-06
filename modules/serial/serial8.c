// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/serial_reg.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mod_devicetable.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

#define SERIAL_RESET_COUNTER 0
#define SERIAL_GET_COUNTER 1
#define SERIAL_BUFSIZE 16

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;
	unsigned int txcount;
	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;
	wait_queue_head_t wait;
	spinlock_t lock;	/* protects rx_buf/buf_rd/buf_wr, txcount, counters and UART registers */
	unsigned int sw_overflow;	/* ring buffer full, char dropped */
	unsigned int hw_overrun;	/* UART_LSR_OE seen */
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

	/* Process context, but the IRQ handler also touches the registers */
	spin_lock_irqsave(&serial->lock, flags);

	while (!(reg_read(serial, UART_LSR) & UART_LSR_THRE))
		cpu_relax();

	reg_write(serial, c, UART_TX);
	serial->txcount++;

	spin_unlock_irqrestore(&serial->lock, flags);
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
	/* Sleep until the IRQ handler puts something in the buffer (may sleep: no lock held!) */
	if (wait_event_interruptible(serial->wait,
				     serial->buf_rd != serial->buf_wr))
		return -ERESTARTSYS;	/* woken up by a signal (e.g. Ctrl-C) */

	spin_lock_irqsave(&serial->lock, flags);

	/* Another reader may have taken the character between wake-up and lock */
	if (serial->buf_rd == serial->buf_wr) {
		spin_unlock_irqrestore(&serial->lock, flags);
		goto retry;
	}

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

	spin_unlock_irqrestore(&serial->lock, flags);

	/* put_user() may fault and sleep: must be called outside the spinlock */
	if (put_user(c, buf))
		return -EFAULT;

	return 1;	/* one character at a time */
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

		/* Non-preemptible kernel: voluntarily let woken readers run. Lock is NOT held here. */
		cond_resched();
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
		count = serial->txcount;	/* snapshot under the lock */
		spin_unlock_irqrestore(&serial->lock, flags);
		if (put_user(count, argp))	/* copy to user space outside the lock */
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
	unsigned int lsr, next;
	char c;

	/* Hard IRQ context: local IRQs are already disabled, plain spin_lock() is enough */
	spin_lock(&serial->lock);

	/* Drain everything the UART has received (reading RX = ack) */
	while ((lsr = reg_read(serial, UART_LSR)) & UART_LSR_DR) {
		if (lsr & UART_LSR_OE)		/* reading LSR also clears OE */
			serial->hw_overrun++;

		c = reg_read(serial, UART_RX);
		next = (serial->buf_wr + 1) % SERIAL_BUFSIZE;
		if (next == serial->buf_rd) {	/* buffer full: drop new char, keep old ones */
			serial->sw_overflow++;
			continue;
		}
		serial->rx_buf[serial->buf_wr] = c;
		serial->buf_wr = next;
	}

	spin_unlock(&serial->lock);

	wake_up(&serial->wait);

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

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency", &uartclk);
	if (ret) {
		dev_err(&pdev->dev, "clock-frequency property not found in DT\n");
		goto err_pm;
	}

	/* Configure the baud rate to 115200 */
	baud_divisor = uartclk / 16 / 115200;
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	/* Clear UART FIFOs */
	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	/* Must be ready before the IRQ can fire */
	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);

	/* Register the interrupt handler */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pm;
	}

	ret = devm_request_irq(&pdev->dev, irq, serial_irq, 0,
			       pdev->name, serial);
	if (ret) {
		dev_err(&pdev->dev, "cannot request irq %d\n", irq);
		goto err_pm;
	}

	/* Enable RX interrupt */
	reg_write(serial, UART_IER_RDI, UART_IER);

	/* Misc device */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_irq;
	}

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					      "serial-%x", (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed\n");
		goto err_irq;
	}

	dev_info(&pdev->dev, "probed, irq %d\n", irq);
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

	dev_info(&pdev->dev, "sw_overflow=%u hw_overrun=%u\n",
		 serial->sw_overflow, serial->hw_overrun);

	/* Disable interrupts before the clock goes away (devm frees the IRQ later) */
	reg_write(serial, 0, UART_IER);

	misc_deregister(&serial->miscdev);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id serial_of_match[] = {
	{ .compatible = "bootlin,serial" },
	{ }
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
MODULE_DESCRIPTION("Bootlin lab serial driver");
