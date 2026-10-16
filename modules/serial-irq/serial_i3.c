// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupts lab review - I3
 * Sleeping, waking up and communication:
 *   IRQ handler -> circular buffer -> wake_up() -> read() returns 1 char
 *
 * Based on I2. Lines marked "NEW (I3)" are new in this step.
 * Removed from I2: printk in the handler, flood guard (ack is proven).
 *
 * NO LOCKING on purpose (that is the next lab): the handler and read()
 * share rx_buf/buf_rd/buf_wr without protection.
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
#include <linux/wait.h>			/* NEW (I3): wait_queue_head_t, wait_event_interruptible() */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16		/* NEW (I3): circular buffer size */

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;
	unsigned int txcount;
	unsigned int irq_count;

	/*
	 * NEW (I3): communication between the IRQ handler (producer)
	 * and read() (consumer).
	 *   buf_wr: next slot the handler writes to
	 *   buf_rd: next slot read() reads from
	 *   buf_rd == buf_wr  <=>  buffer empty
	 */
	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;

	/* NEW (I3): processes sleeping in read() until data arrives */
	wait_queue_head_t wait;
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

/*
 * NEW (I3): read() - one character per call.
 *
 * wait_event_interruptible(wq, condition):
 *   - condition true  -> returns 0 immediately, no sleep
 *   - condition false -> process state = TASK_INTERRUPTIBLE, sleeps on wq;
 *     each wake_up(wq) re-checks the condition, sleeps again if false
 *   - a signal (Ctrl-C) -> returns -ERESTARTSYS
 * Process context, no lock held: sleeping is allowed here.
 */
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

	return 1;		/* one character at a time, even if sz > 1 */
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

/*
 * Interrupt handler (hard IRQ context, must not sleep).
 * NEW (I3): no printk any more (too slow with IRQs off): just store the
 * character and wake up the readers. The slow part (copy to user space)
 * is done later by read(), in process context.
 *
 * As in the Bootlin lab, a full buffer is NOT detected: if 16 characters
 * arrive while nobody reads, buf_wr catches up with buf_rd (see I4).
 */
static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	char c;

	serial->irq_count++;

	c = reg_read(serial, UART_RX);		/* ack */

	/* NEW (I3): producer side of the circular buffer */
	serial->rx_buf[serial->buf_wr] = c;
	serial->buf_wr++;
	if (serial->buf_wr >= SERIAL_BUFSIZE)
		serial->buf_wr = 0;

	/* NEW (I3): wake up every process sleeping in read() */
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

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);	/* buf_rd = buf_wr = 0: empty */
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	/*
	 * NEW (I3): the wait queue must be initialized BEFORE the IRQ can
	 * fire (the handler calls wake_up() on it).
	 */
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

	/* Clear UART FIFOs */
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

	dev_info(&pdev->dev, "registered /dev/%s (misc minor %d), irq %d\n",
		 serial->miscdev.name, serial->miscdev.minor, irq);

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

	dev_info(&pdev->dev, "irq_count=%u\n", serial->irq_count);

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
MODULE_DESCRIPTION("Interrupts lab review I3: circular buffer + wait queue + read()");
