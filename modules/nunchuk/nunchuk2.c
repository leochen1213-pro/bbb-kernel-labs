// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>

static int nunchuk_write(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;
	return 0;
}

static int nunchuk_probe(struct i2c_client *client)
{
	int ret;

	ret = nunchuk_write(client, 0xf0, 0x55);
	if (ret) {
		dev_err(&client->dev, "init step 1 failed: %d\n", ret);
		return ret;
	}

	fsleep(1000);

	ret = nunchuk_write(client, 0xfb, 0x00);
	if (ret) {
		dev_err(&client->dev, "init step 2 failed: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "nunchuk initialized\n");
	return 0;
}

static void nunchuk_remove(struct i2c_client *client)
{
}

static const struct of_device_id nunchuk_of_match[] = {
	{ .compatible = "nintendo,nunchuk" },
	{ }
};
MODULE_DEVICE_TABLE(of, nunchuk_of_match);

static struct i2c_driver nunchuk_driver = {
	.driver = {
		.name = "nunchuk",
		.of_match_table = nunchuk_of_match,
	},
	.probe = nunchuk_probe,
	.remove = nunchuk_remove,
};
module_i2c_driver(nunchuk_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Wii Nunchuk driver");
