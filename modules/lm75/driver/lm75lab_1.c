
// SPDX-License-Identifier: GPL-2.0
/*
 * lm75lab.c - minimal NXP LM75B driver (learning exercise)
 */
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#define LM75_REG_CONF	0x01

static int lm75lab_probe(struct i2c_client *client)
{
	int conf;

	/* First real transfer: proves the chip answers before we claim it */
	conf = i2c_smbus_read_byte_data(client, LM75_REG_CONF);
	if (conf < 0)
		return dev_err_probe(&client->dev, conf, "failed to read config register\n");

	dev_info(&client->dev, "probed at 0x%02x, conf=0x%02x\n", client->addr, conf);
	return 0;
}

static void lm75lab_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "removed\n");
}

static const struct of_device_id lm75lab_of_match[] = {
	{ .compatible = "national,lm75b" },
	{ }
};
MODULE_DEVICE_TABLE(of, lm75lab_of_match);

static const struct i2c_device_id lm75lab_id[] = {
	{ "lm75b", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm75lab_id);

static struct i2c_driver lm75lab_driver = {
	.driver = {
		.name = "lm75lab",
		.of_match_table = lm75lab_of_match,
	},
	.probe = lm75lab_probe,
	.remove = lm75lab_remove,
	.id_table = lm75lab_id,
};
module_i2c_driver(lm75lab_driver);

MODULE_AUTHOR("Leo Chen");
MODULE_DESCRIPTION("Minimal LM75B driver (lab)");
MODULE_LICENSE("GPL");
