// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupts lab review - I2
 * Acknowledge the RX interrupt by reading UART_RX, print the character.
 *
 * Based on I1. Lines marked "NEW (I2)" are new in this step
 * ("NEW (I1)" comments kept for reference).
 *
 * Flood guard (lab-review only, not in the Bootlin lab): after
 * FLOOD_GUARD calls the handler disables the RX interrupt itself.
 * With a correct ack it should never trigger.
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
#include <linux/interrupt.h>		/* NEW (I1): devm_request_irq(), irqreturn_t */

#define SERIAL_BAUD	115200
#define FLOOD_GUARD	10000		/* NEW (I1): handler calls before we give up */

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;
	unsigned int txcount;
	unsigned int irq_count;		/* NEW (I1): how many times the handler ran */
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
	return -EINVAL;
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
 * Interrupt handler (hard IRQ context: local IRQs disabled, must not
 * sleep, must be short). dev_id is the pointer given to devm_request_irq().
 *
 * NEW (I2): ACKNOWLEDGE the interrupt by reading UART_RX.
 * Reading RHR removes the character -> LSR.DR goes to 0 -> the UART
 * releases its (level-triggered) interrupt line -> no more flood.
 * Reading LSR FIRST lets us see DR/OE as they were (reading LSR also
 * clears OE).
 */
static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	u32 lsr;			/* NEW (I2) */
	char c;				/* NEW (I2) */

	serial->irq_count++;

	lsr = reg_read(serial, UART_LSR);	/* NEW (I2): status before the read */
	c = reg_read(serial, UART_RX);		/* NEW (I2): THE ack */

	/*
	 * NEW (I2): print what we received. Only a few characters per test,
	 * so no ratelimit. NOTE: printk on the serial console is synchronous
	 * (~5 ms per line at 115200) and we run with IRQs off...
	 */
	dev_info(serial->miscdev.parent, "irq #%u: LSR 0x%02x, got 0x%02x '%c'\n",
		 serial->irq_count, lsr, (unsigned char)c,
		 (c >= 0x20 && c < 0x7f) ? c : '.');

	/* Lab-review safety net, kept in case the ack does not work */
	if (serial->irq_count == FLOOD_GUARD) {
		reg_write(serial, 0, UART_IER);
		dev_warn(serial->miscdev.parent,
			 "flood guard: %u calls, RX interrupt disabled\n",
			 serial->irq_count);
	}

	return IRQ_HANDLED;
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int uartclk, baud_divisor;
	int irq;			/* NEW (I1) */
	int ret;

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

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

	/*
	 * NEW (I1) (1): get the Linux (virtual) IRQ number.
	 * The DT "interrupts" property holds the HARDWARE number of the
	 * interrupt controller (INTC); the irqdomain maps it to a virtual
	 * number. Returns a negative errno on failure (may be -EPROBE_DEFER).
	 */

	/* ① 問 kernel：我的中斷號是多少？ */
	irq = platform_get_irq(pdev, 0); 
	if (irq < 0) {
		ret = irq;
		goto err_pm;
	}

	/*
	 * NEW (I1) (2): register the handler BEFORE enabling the interrupt
	 * in the UART: an interrupt with no handler would be reported as
	 * "nobody cared" and the line disabled by the kernel.
	 * devm_: free_irq() is done automatically AFTER remove() returns.
	 * flags 0: not shared. Name shows up in /proc/interrupts.
	 * dev_id = serial: handed back to the handler.
	 */

	/* ② 告訴 kernel：這個中斷來了就呼叫 serial_irq */
	ret = devm_request_irq(&pdev->dev, irq, serial_irq, 0,
			       dev_name(&pdev->dev), serial);
	if (ret) {
		dev_err(&pdev->dev, "cannot request IRQ %d: %d\n", irq, ret);
		goto err_pm;
	}

	/* NEW (I1) (3): enable the "RX data available" interrupt in the UART */
	
	/* ③ 告訴 UART：收到資料就發中斷 */
	reg_write(serial, UART_IER_RDI, UART_IER);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_irq;			/* NEW (I1): IER is on now */
	}

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;			/* NEW (I1) */
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_irq;			/* NEW (I1) */
	}

	dev_info(&pdev->dev, "registered /dev/%s (misc minor %d), irq %d\n",
		 serial->miscdev.name, serial->miscdev.minor, irq);	/* NEW (I1): irq */

	return 0;

err_irq:				/* NEW (I1): undo the UART-side enable */
	reg_write(serial, 0, UART_IER);
err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "irq_count=%u\n", serial->irq_count);	/* NEW (I1) */

	misc_deregister(&serial->miscdev);

	/*
	 * NEW (I1): silence the UART BEFORE the clock is gated.
	 * devm frees the IRQ only after remove() returns: if the UART fired
	 * in between, the handler would read registers with the clock OFF.
	 */
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
MODULE_DESCRIPTION("Interrupts lab review I2: ack RX interrupt by reading UART_RX");
