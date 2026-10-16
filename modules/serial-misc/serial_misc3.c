// SPDX-License-Identifier: GPL-2.0
/*
 * Output-only misc driver lab review - M2
 * Implement write(): container_of() + get_user(), \n -> \n\r.
 *
 * Based on the I/O memory review (Step 6), cleaned up:
 *  - skip_pm experiment parameter and debug prints removed
 *  - "Hello" test string in probe() removed
 * Lines marked "NEW (M1)" are the misc additions, "NEW (M2)" the write() implementation.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/serial_reg.h>
#include <linux/of.h>
#include <linux/miscdevice.h>		/* NEW (M1): struct miscdevice, misc_register() */
#include <linux/fs.h>			/* NEW (M1): struct file_operations */
#include <linux/uaccess.h>		/* NEW (M2): get_user() */

#define SERIAL_BAUD	115200

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;	/* NEW (M1): EMBEDDED, not a pointer (needed by container_of later) */
};

static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}

static void reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}

static void __maybe_unused serial_write_char(struct serial_dev *serial, char c)
{
	while (!(reg_read(serial, UART_LSR) & UART_LSR_THRE))
		cpu_relax();

	reg_write(serial, c, UART_TX);
}

/*
 * NEW (M1): file operation stubs.
 * -EINVAL = "Invalid argument": signals "not implemented yet".
 * Note the __user annotation: buf is a USER SPACE pointer, never dereference it directly.
 */
static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	return -EINVAL;
}

/*
 * NEW (M2): write() - copy user data to the UART, one character at a time.
 *
 * 1. Find OUR device: file->private_data was set by the misc core at open()
 *    time to point to the struct miscdevice we registered. Since that
 *    miscdevice is EMBEDDED in struct serial_dev, container_of() walks back
 *    to the start of the enclosing structure.
 * 2. buf is a USER pointer (__user): never dereference it. get_user()
 *    checks that the address belongs to user space and handles faults
 *    (unmapped page, bad pointer) by returning -EFAULT instead of crashing.
 * 3. "\n" -> "\n\r": a terminal needs CR to go back to column 0.
 * 4. Return the number of bytes CONSUMED from the user buffer (sz), not the
 *    number sent on the wire (the extra '\r' is our business).
 */
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


/*
 * NEW (M1): the table the VFS uses to dispatch read()/write() on our device file.
 * .owner = THIS_MODULE is deliberately MISSING for now (refcount experiment in M3).
 */
static const struct file_operations serial_fops = {
	.owner = THIS_MODULE
	.read = serial_read,
	.write = serial_write,
};

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int uartclk, baud_divisor;
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
	 * NEW (M1): misc device registration, LAST in probe():
	 * as soon as misc_register() returns, /dev/serial-xxx exists and user space
	 * may open/write it, so the hardware must already be fully initialized.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_pm;
	}

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;	/* let the misc core pick a free minor */
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);	/* -> /dev/serial-48024000 */
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_pm;
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;	/* sysfs: /sys/class/misc/serial-xxx/device -> our platform device */

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_pm;
	}

	dev_info(&pdev->dev, "registered /dev/%s (misc minor %d)\n",
		 serial->miscdev.name, serial->miscdev.minor);

	return 0;

err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);	/* NEW (M1): now really needed */

	/*
	 * NEW (M1): reverse order of probe(): first remove the user-space entry point
	 * (no new open() possible), then power down the hardware.
	 */
	misc_deregister(&serial->miscdev);

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
MODULE_DESCRIPTION("Misc driver lab review M2: write() implementation");
