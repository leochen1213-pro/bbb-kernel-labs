// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>       /* NEW: struct of_device_id */

static int serial_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Called %s\n", __func__);   /* NEW: pr_info -> dev_info */
	return 0;
}

static int serial_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Called %s\n", __func__);   /* NEW */
	return 0;
}

/* NEW: 這個 driver 能處理哪些 DT 節點 */
static const struct of_device_id serial_of_match[] = {
	{ .compatible = "bootlin,serial" },
	{ },                                  /* sentinel：結尾一定要有空的一項 */
};
MODULE_DEVICE_TABLE(of, serial_of_match);

static struct platform_driver serial_driver = {
	.driver = {
		.name = "serial",
		.of_match_table = serial_of_match,   /* NEW */
	},
	.probe = serial_probe,
	.remove = serial_remove,
};
module_platform_driver(serial_driver);

MODULE_LICENSE("GPL");
