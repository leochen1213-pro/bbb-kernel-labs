// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal NXP LM75B driver (learning exercise)
 *
 * Step 3.4: OS pin as interrupt (threaded IRQ, IRQF_ONESHOT),
 *           temp1_max_alarm with hwmon_notify_event().
 */
#include <linux/bits.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
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

/* Conf register bits (datasheet Table 8) */
#define LM75_CONF_SHUTDOWN	BIT(0)
#define LM75_CONF_OS_INT	BIT(1)	/* 0 = comparator, 1 = interrupt */
#define LM75_CONF_OS_POL	BIT(2)	/* 0 = active low, 1 = active high */

/* Operating range (datasheet), used to clamp user-supplied limits */
#define LM75_TEMP_MIN_MC	(-55000)
#define LM75_TEMP_MAX_MC	125000

struct lm75lab {
	struct i2c_client *client;
	struct device *hwmon_dev;
	/*
	 * Serializes multi-transfer sequences (limit check-then-write,
	 * IRQ thread temp/limit reads) and protects @alarm.
	 */
	struct mutex lock;
	bool alarm;
	u8 orig_conf;		/* restored on unbind */
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
	case hwmon_temp_max_alarm:
		mutex_lock(&data->lock);
		*val = data->alarm;
		mutex_unlock(&data->lock);
		return 0;
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
	const struct lm75lab *data = drvdata;

	if (type != hwmon_temp)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	case hwmon_temp_max:
	case hwmon_temp_max_hyst:
		return 0644;
	case hwmon_temp_max_alarm:
		/* Only meaningful when the OS pin is wired to an interrupt */
		return data->client->irq > 0 ? 0444 : 0;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const lm75lab_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_MAX_HYST |
			   HWMON_T_MAX_ALARM),
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

/*
 * Runs in process context (may sleep), with the IRQ line kept masked
 * by IRQF_ONESHOT until we return. In interrupt mode any register read
 * releases OS, which deasserts the level-low interrupt.
 */
static irqreturn_t lm75lab_irq_thread(int irq, void *dev_id)
{
	struct lm75lab *data = dev_id;
	long temp = 0, temp9, tos, thyst;
	bool changed = false, alarm = false;
	int ret;

	mutex_lock(&data->lock);

	/* Read Temp first: this read is what clears the OS output */
	ret = i2c_smbus_read_word_swapped(data->client, LM75_REG_TEMP);
	if (ret < 0)
		goto unlock;
	temp = lm75lab_temp_to_mc((s16)ret);
	/*
	 * The chip compares only the 9 MSBs of Temp (0.5 degC steps) against
	 * the limits, inclusively on both sides: OS fires at T9 >= Tos and
	 * again at T9 <= Thyst. Decide with the same view of the temperature,
	 * otherwise e.g. 28.375 degC (T9 = 28.0) never clears Thyst = 28.0.
	 */
	temp9 = lm75lab_limit_to_mc((s16)ret);

	ret = lm75lab_read_limit(data, LM75_REG_TOS, &tos);
	if (ret < 0)
		goto unlock;
	ret = lm75lab_read_limit(data, LM75_REG_THYST, &thyst);
	if (ret < 0)
		goto unlock;

	/*
	 * Interrupt mode signals both crossings: decide which one it was from
	 * the state, not by toggling, so a repeated (spurious) IRQ is harmless.
	 */
	if (!data->alarm && temp9 >= tos) {
		data->alarm = true;
		changed = true;
	} else if (data->alarm && temp9 <= thyst) {
		data->alarm = false;
		changed = true;
	}
	alarm = data->alarm;	/* snapshot for logging outside the lock */

unlock:
	mutex_unlock(&data->lock);

	if (ret < 0) {
		/* OS stays asserted: the line will fire again, so rate-limit */
		dev_err_ratelimited(&data->client->dev,
				    "IRQ: failed to read sensor: %d\n", ret);
		return IRQ_HANDLED;
	}

	if (changed) {
		dev_info(&data->client->dev, "alarm %s at %ld mC\n",
			 alarm ? "raised" : "cleared", temp);
		/* Wakes up user space poll()/select() on temp1_max_alarm */
		hwmon_notify_event(data->hwmon_dev, hwmon_temp,
				   hwmon_temp_max_alarm, 0);
	}

	return IRQ_HANDLED;
}

static void lm75lab_restore_conf(void *arg)
{
	struct lm75lab *data = arg;
	int ret;

	/* devm actions cannot fail, but a lost write must not go unnoticed */
	ret = i2c_smbus_write_byte_data(data->client, LM75_REG_CONF,
					data->orig_conf);
	if (ret < 0)
		dev_warn(&data->client->dev,
			 "failed to restore config register: %d\n", ret);
}

static int lm75lab_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lm75lab *data;
	int conf, ret;

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
	data->orig_conf = conf;

	if (client->irq > 0) {
		/* Interrupt mode, OS active low to match IRQ_TYPE_LEVEL_LOW */
		conf |= LM75_CONF_OS_INT;
		conf &= ~LM75_CONF_OS_POL;
		ret = i2c_smbus_write_byte_data(client, LM75_REG_CONF, conf);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to write config register\n");
	}

	/* Put the chip back as we found it on unbind (runs last, see below) */
	ret = devm_add_action_or_reset(dev, lm75lab_restore_conf, data);
	if (ret)
		return ret;

	data->hwmon_dev = devm_hwmon_device_register_with_info(dev, "lm75lab",
							       data,
							       &lm75lab_chip_info,
							       NULL);
	if (IS_ERR(data->hwmon_dev))
		return PTR_ERR(data->hwmon_dev);

	/*
	 * Request the IRQ last: the thread uses hwmon_dev. devm releases in
	 * reverse order, so on unbind the IRQ goes first, then hwmon, then
	 * the config is restored.
	 */
	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						lm75lab_irq_thread,
						IRQF_ONESHOT, dev_name(dev),
						data);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ %d\n",
					     client->irq);
	}

	dev_info(dev, "probed at 0x%02x, conf=0x%02x->0x%02x, irq=%d, %s\n",
		 client->addr, data->orig_conf, conf, client->irq,
		 dev_name(data->hwmon_dev));
	return 0;
}

static void lm75lab_remove(struct i2c_client *client)
{
	/* devm releases IRQ, hwmon_dev, conf restore, data in that order */
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
