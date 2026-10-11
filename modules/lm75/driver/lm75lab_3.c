// SPDX-License-Identifier: GPL-2.0
/*
 * lm75lab.c - minimal NXP LM75B driver (learning exercise)
 *
 * Step 3.3: temp1_max / temp1_max_hyst, 9-bit limit conversion,
 *           Tos > Thyst enforcement protected by a mutex.
 */
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>

/* Registers (datasheet Table 5) */
#define LM75_REG_TEMP		0x00
#define LM75_REG_CONF		0x01
#define LM75_REG_THYST		0x02
#define LM75_REG_TOS		0x03

/* Operating range (datasheet), used to clamp user-supplied limits */
#define LM75_TEMP_MIN_MC	(-55000)
#define LM75_TEMP_MAX_MC	125000

struct lm75lab {
	struct i2c_client *client;
	/*
	 * Serializes multi-transfer sequences: checking one limit
	 * against the other and then writing it must not interleave.
	 */
	struct mutex lock;
};

/* Temp: 11-bit two's complement in bits [15:5], 0.125 degC/LSB */
static long lm75lab_temp_to_mc(s16 reg)
{
	return (reg >> 5) * 125;
}

/* Tos/Thyst: 9-bit two's complement in bits [15:7], 0.5 degC/LSB */
static long lm75lab_limit_to_mc(s16 reg)
{
	return (reg >> 7) * 500;
}

/*
 * Millidegree -> 9-bit limit register, rounded to the nearest 0.5 degC.
 * Uses the limit resolution (9 bits), not the temperature resolution
 * (11 bits), so the value written is exactly what the chip stores.
 * Multiply instead of shifting: left-shifting a negative value is UB.
 */
static u16 lm75lab_mc_to_limit(long mc)
{
	mc = clamp_val(mc, LM75_TEMP_MIN_MC, LM75_TEMP_MAX_MC);
	return (u16)(DIV_ROUND_CLOSEST(mc, 500) * 128);
}

static int lm75lab_read_limit(struct lm75lab *data, u8 reg, long *val)
{
	int ret;

	ret = i2c_smbus_read_word_swapped(data->client, reg);
	if (ret < 0)
		return ret;
	*val = lm75lab_limit_to_mc((s16)ret);
	return 0;
}

static int lm75lab_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
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
	case hwmon_temp_max:
		return lm75lab_read_limit(data, LM75_REG_TOS, val);
	case hwmon_temp_max_hyst:
		return lm75lab_read_limit(data, LM75_REG_THYST, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int lm75lab_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct lm75lab *data = dev_get_drvdata(dev);
	u16 new = lm75lab_mc_to_limit(val);
	u8 reg, other;
	int ret;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_temp_max:
		reg = LM75_REG_TOS;
		other = LM75_REG_THYST;
		break;
	case hwmon_temp_max_hyst:
		reg = LM75_REG_THYST;
		other = LM75_REG_TOS;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&data->lock);

	/* Datasheet 7.6: Tos must stay above Thyst, else OS is undefined */
	ret = i2c_smbus_read_word_swapped(data->client, other);
	if (ret < 0)
		goto unlock;

	if ((reg == LM75_REG_TOS && (s16)new <= (s16)ret) ||
	    (reg == LM75_REG_THYST && (s16)new >= (s16)ret)) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = i2c_smbus_write_word_swapped(data->client, reg, new);

unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static umode_t lm75lab_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	case hwmon_temp_max:
	case hwmon_temp_max_hyst:
		return 0644;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const lm75lab_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_HYST),
	NULL
};

static const struct hwmon_ops lm75lab_hwmon_ops = {
	.is_visible = lm75lab_is_visible,
	.read = lm75lab_read,
	.write = lm75lab_write,
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
	/* Must be ready before hwmon registration: callbacks may run at once */
	mutex_init(&data->lock);

	/* First real transfer: proves the chip answers before we claim it */
	conf = i2c_smbus_read_byte_data(client, LM75_REG_CONF);
	if (conf < 0)
		return dev_err_probe(dev, conf,
				     "failed to read config register\n");

	/* Register last: user space may access the attributes right after */
	hwmon_dev = devm_hwmon_device_register_with_info(dev, "lm75lab", data,
							 &lm75lab_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	dev_info(dev, "probed at 0x%02x, conf=0x%02x, %s\n",
		 client->addr, conf, dev_name(hwmon_dev));
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
