// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - I2C bus driver (BBB labs)
 * NEW (3.6): the whole file. Same core as SPI, only the regmap differs.
 */
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "adxl345.h"

/*
 * No flag masks: on I2C the read/write direction is the R/W bit of the
 * address byte (handled by the I2C core), and the ADXL345 auto-increments
 * the register address in multi-byte reads by itself (no MB bit).
 */
static const struct regmap_config leo_adxl345_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ADXL345_REG_MAX,
};

static int leo_adxl345_i2c_probe(struct i2c_client *client)
{
	struct regmap *regmap;

	dev_info(&client->dev, "i2c: addr 0x%02x on %s\n",
		 client->addr, dev_name(&client->adapter->dev));

	regmap = devm_regmap_init_i2c(client, &leo_adxl345_i2c_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "cannot create I2C regmap\n");

	return leo_adxl345_core_probe(&client->dev, regmap, client->irq);
}

/* DT match */
static const struct of_device_id leo_adxl345_i2c_of_match[] = {
	{ .compatible = "leo,adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(of, leo_adxl345_i2c_of_match);

/* also used by "echo adxl345 0x53 > .../new_device" (no DT needed) */
static const struct i2c_device_id leo_adxl345_i2c_id[] = {
	{ "adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, leo_adxl345_i2c_id);

static struct i2c_driver leo_adxl345_i2c_driver = {
	.driver = {
		.name = "leo-adxl345-i2c",
		.of_match_table = leo_adxl345_i2c_of_match,
	},
	.probe = leo_adxl345_i2c_probe,
	.id_table = leo_adxl345_i2c_id,
};
module_i2c_driver(leo_adxl345_i2c_driver);

MODULE_AUTHOR("Leo <leochen1213@gmail.com>");
MODULE_DESCRIPTION("ADXL345 accelerometer I2C driver (BBB labs, stage 3.6)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(LEO_ADXL345);	/* leo_adxl345_core_probe() */
