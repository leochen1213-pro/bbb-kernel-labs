// SPDX-License-Identifier: GPL-2.0
/*
 * I/O memory lab review - Step 3
 * Private structure + ioremap of the UART registers.
 *
 * Lines marked "NEW (Step 3)" were added on top of Step 2.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>			/* void __iomem, readl()/writel() */

/*
 * per-device private data.每個 UART 一份
 * One instance per UART (UART2 and UART4 each get their own),
 * so never keep device state in global variables.
 */
struct serial_dev {
	void __iomem *regs;		/* virtual base address of the UART registers */
};

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;	
	struct resource *res;		
	resource_size_t size;	

	dev_info(&pdev->dev, "Called %s\n", __func__);

	/* ① 配置私有結構（devm：裝置解綁時自動釋放
	 * allocate the private structure.
	 * devm_ = managed resource: freed automatically when the device is
	 * unbound (remove, or probe failure), so no kfree() is needed.
	 */
	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	/* ② 把實體位址映射成虛擬位址，同時 request_mem_region 認領它
	 * (2): map the registers.
	 * This one call:
	 *   - gets the first "reg" resource (0x48024000, size 0x1000)
	 *   - claims it with request_mem_region() -> shows up in /proc/iomem,
	 *     and a second driver trying to claim it gets -EBUSY
	 *   - ioremap()s it to a kernel virtual address
	 * On failure it returns an ERR_PTR (never NULL): check with IS_ERR().
	 */
	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	/* ③ 讓 remove() 之後能找回這個結構
	 * attach our structure to the platform device,
	 * so that remove() (which only receives pdev) can get it back.
	 */
	platform_set_drvdata(pdev, serial);

	/* ④ 印出實體位址和虛擬位址，觀察用
	 * print physical vs virtual address, for observation.
	 * %pa  : prints phys_addr_t / resource_size_t, takes a POINTER
	 * %p   : prints a kernel pointer, hashed by default (security)
	 * %px  : prints the raw pointer value (debug only, leaks addresses)
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	dev_info(&pdev->dev, "phys %pa size %pa -> virt %p (raw %px)\n",
		 &res->start, &size, serial->regs, serial->regs);

	return 0;
}

static int serial_remove(struct platform_device *pdev)
{
	/* get our private structure back from pdev */
	struct serial_dev *serial = platform_get_drvdata(pdev);
	
	/* devm releases data in reverse order after this, so it can do nothing 
	 * so can just print logs */
	dev_info(&pdev->dev, "Called %s, regs was %px\n", __func__, serial->regs);

	/*
	 * Nothing to free here: devm_ releases (in reverse order of allocation)
	 * the ioremap, the request_mem_region and the kzalloc'ed structure
	 * automatically after remove() returns.
	 */

	return 0;
}

static const struct of_device_id serial_of_match[] = {
	{ .compatible = "bootlin,serial" },
	{ },				/* sentinel */
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
MODULE_DESCRIPTION("I/O memory lab review: UART register mapping");
