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
#include <linux/mod_devicetable.h>
#include <linux/uaccess.h>

//interrupt
#include <linux/interrupt.h>
#include <linux/wait.h>

//Add ioctl 編號
#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;
	unsigned int txcount;// 私有結構加一個計數器
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
	serial->txcount++;// Add
}

static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	return -EINVAL;
}

/**
static ssize_t serial_write(struct file *file, const char __user *buf,
			    size_t sz, loff_t *ppos)
{
	return -EINVAL;
}
**/

static ssize_t serial_write(struct file *file, const char __user *buf,
			    size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev,
						 miscdev);
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

// irq
static irqreturn_t serial_irq(int irq, void *dev_id)
{
	pr_info_ratelimited("serial: irq %d\n", irq);
	return IRQ_HANDLED;
}


//新增 ioctl 函式
static long serial_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev,
						 miscdev);
	unsigned int __user *argp = (unsigned int __user *)arg;

	switch (cmd) {
	case SERIAL_RESET_COUNTER:
		serial->txcount = 0;
		return 0;
	case SERIAL_GET_COUNTER:
		if (put_user(serial->txcount, argp))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write,
	.unlocked_ioctl = serial_ioctl,
};

/**
一個已知的問題
txcount++ 和 txcount = 0 都沒有保護。如果兩個 process 同時寫入,或者一邊寫一邊重設,計數可能出錯。這正是下一個 lab「Locking」要處理的問題,先記住它。
**/

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int baud_divisor, uartclk;
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

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				   &uartclk);
	if (ret) {
		dev_err(&pdev->dev,
			"clock-frequency property not found in Device Tree\n");
		goto err_pm;
	}

	baud_divisor = uartclk / 16 / 115200;
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	/* Register the misc device, once the hardware is ready */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_pm;
	}

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					      "serial-%x", res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_pm;
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_pm;
	}

	dev_info(&pdev->dev, "registered /dev/%s\n", serial->miscdev.name);
	return 0;

err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);

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
