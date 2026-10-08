// SPDX-License-Identifier: GPL-2.0
/*
 * lm75lab.c - minimal NXP LM75B driver (learning exercise)
 *
 * Step 3.2: read the temperature and expose it through hwmon.
 */
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

/* Registers (datasheet Table 5) */
#define LM75_REG_TEMP	0x00
#define LM75_REG_CONF	0x01

struct lm75lab {
	struct i2c_client *client;
};

/* 11-bit two's complement in bits [15:5], 0.125 degC/LSB -> millidegree */
static long lm75lab_temp_to_mc(s16 reg)
{
	return (reg >> 5) * 125;
}

static int lm75lab_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	struct lm75lab *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_temp_input:
		/* LM75 sends MSB first: the swapped variant undoes SMBus LE order */
		ret = i2c_smbus_read_word_swapped(data->client, LM75_REG_TEMP);
		if (ret < 0)
			return ret;
		*val = lm75lab_temp_to_mc((s16)ret);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t lm75lab_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;
	return 0;
}

static const struct hwmon_channel_info * const lm75lab_info[] = {HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops lm75lab_hwmon_ops = {
	.is_visible = lm75lab_is_visible,
	.read = lm75lab_read,
};

static const struct hwmon_chip_info lm75lab_chip_info = {
	.ops = &lm75lab_hwmon_ops,
	.info = lm75lab_info,
};

static int lm75lab_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;
	struct lm75lab *data;
	int conf;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->client = client;

	/* First real transfer: proves the chip answers before we claim it */
	conf = i2c_smbus_read_byte_data(client, LM75_REG_CONF);
	if (conf < 0)
		return dev_err_probe(dev, conf, "failed to read config register\n");

	/* Register last: user space may read temp1_input right after this */
	hwmon_dev = devm_hwmon_device_register_with_info(dev, "lm75lab", data, &lm75lab_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "probed at 0x%02x, conf=0x%02x, %s\n", client->addr, conf, dev_name(hwmon_dev));
	return 0;
}

static void lm75lab_remove(struct i2c_client *client)
{
	/* devm releases hwmon_dev and data in reverse order after this */
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
