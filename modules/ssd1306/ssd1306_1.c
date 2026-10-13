// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306 - minimal driver for a 128x32 SSD1306 I2C OLED
 *
 * Step 3.1: i2c_driver skeleton. probe()/remove() only log, so we can see
 * the DT node bind to this driver (and not to the upstream ssd130x one).
 */
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

static int ssd1306_probe(struct i2c_client *client)
{
	/* We will send multi-byte writes (control byte + payload) */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&client->dev, -EOPNOTSUPP, "adapter does not support plain I2C\n");

	dev_info(&client->dev, "probe: addr 0x%02x on %s\n", client->addr, client->adapter->name);
	return 0;
}

static void ssd1306_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "remove\n");
}

static const struct of_device_id ssd1306_of_match[] = {
	{ .compatible = "leo,ssd1306" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd1306_of_match);

static struct i2c_driver ssd1306_driver = {
	.driver = {
		.name = "leo-ssd1306",
		.of_match_table = ssd1306_of_match,
	},
	.probe = ssd1306_probe,
	.remove = ssd1306_remove,
};
module_i2c_driver(ssd1306_driver);

MODULE_AUTHOR("Leo Chen");
MODULE_DESCRIPTION("Minimal SSD1306 128x32 I2C OLED driver");
MODULE_LICENSE("GPL");
