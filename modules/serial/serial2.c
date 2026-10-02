// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/serial_reg.h>
#include <linux/mod_devicetable.h>

struct serial_dev {
	void __iomem *regs;
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
	/* Wait until the transmit holding register is empty */
	while (!(reg_read(serial, UART_LSR) & UART_LSR_THRE))
		cpu_relax();

	reg_write(serial, c, UART_TX);
}

static void serial_write_str(struct serial_dev *serial, const char *s)
{
	while (*s)
		serial_write_char(serial, *s++);
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	unsigned int baud_divisor, uartclk;
	int ret;

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	/* Power and clock the UART block before touching its registers */
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	/* Configure the baud rate to 115200 */
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

	/* Clear UART FIFOs */
	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	dev_info(&pdev->dev, "uartclk=%u Hz, divisor=%u\n",
		 uartclk, baud_divisor);

	serial_write_str(serial, "Hello\r\n");

	return 0;

err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
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
