// SPDX-License-Identifier: GPL-2.0
// SPDX-License-Identifier: GPL-2.0
/*
 * I/O memory lab review - Step 4
 * Register accessors + runtime PM, then read a few registers.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>

#include <linux/pm_runtime.h>		/* pm_runtime_*() */
#include <linux/serial_reg.h>		/* UART_LSR, UART_OMAP_* register indexes */

/*
 * skip_pm=1 reads the registers WITHOUT enabling the module clock first,
 * to see what happens on real hardware. Expect a kernel Oops, then reboot.
 */
static bool skip_pm;
module_param(skip_pm, bool, 0444);
MODULE_PARM_DESC(skip_pm, "1 = read registers without runtime PM (crash experiment)");

struct serial_dev {
	void __iomem *regs;	
};

/*
 * register accessors.
 * serial_reg.h gives register INDEXES (UART_LSR = 5). On OMAP each register
 * sits on a 32-bit boundary, so the byte offset is index * 4.
 * readl()/writel(): exact 32-bit access, not reordered/optimized away,
 * with the barriers ARM needs for device memory.
 */
static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}
 
static void __maybe_unused reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;	
	struct resource *res;		
	resource_size_t size;	
	
	//Add
	u32 mver, syss, lsr;
	int ret;

	dev_info(&pdev->dev, "Called %s\n", __func__);

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	dev_info(&pdev->dev, "phys %pa size %pa -> virt %p (raw %px)\n",
		 &res->start, &size, serial->regs, serial->regs);


	/*
	 * power up the device before touching any register.
	 * pm_runtime_enable(): allow runtime PM for this device.
	 * pm_runtime_resume_and_get(): take a usage reference and resume the
	 *   device NOW. The parent (ti-sysc "target-module") is resumed first
	 *   and enables the UART module clock.
	 *   (The lab uses pm_runtime_get_sync(); resume_and_get() is the modern
	 *   variant that drops the reference by itself if resuming fails.)
	 */
	if (!skip_pm) {
		pm_runtime_enable(&pdev->dev);
		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret < 0) {
			pm_runtime_disable(&pdev->dev);
			return ret;
		}
	} 
	else {
		dev_warn(&pdev->dev, "skip_pm=1: reading registers with the clock possibly OFF!\n");
	}

	/* read a few registers to prove we talk to the UART */
	mver = reg_read(serial, UART_OMAP_MVER);	/* offset 0x50: IP revision */
	syss = reg_read(serial, UART_OMAP_SYSS);	/* offset 0x58: bit0 RESETDONE */
	lsr  = reg_read(serial, UART_LSR);		/* offset 0x14: expect 0x60 (TX empty) */

	dev_info(&pdev->dev, "MVER 0x%08x  SYSS 0x%08x  LSR 0x%02x\n", mver, syss, lsr);

	return 0;
}

static int serial_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Called %s\n", __func__);
	
	/*
	 * runtime PM is NOT devm-managed here, so undo it by hand,
	 * in reverse order of probe(): drop the reference (clock may be gated),
	 * then disable runtime PM.
	 */
	if (!skip_pm) {
		pm_runtime_put_sync(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
	}
 
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
MODULE_DESCRIPTION("I/O memory lab review: UART register access with runtime PM");

