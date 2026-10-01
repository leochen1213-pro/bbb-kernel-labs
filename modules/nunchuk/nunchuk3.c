// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/bits.h>
#include <linux/mod_devicetable.h>

#define NUNCHUK_REGS_LEN 6

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

static int nunchuk_read_registers(struct i2c_client *client, u8 *regs)
{
	u8 zero = 0x00;
	int ret;

	fsleep(10000);

	ret = i2c_master_send(client, &zero, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	fsleep(10000);

	ret = i2c_master_recv(client, regs, NUNCHUK_REGS_LEN);
	if (ret < 0)
		return ret;
	if (ret != NUNCHUK_REGS_LEN)
		return -EIO;

	return 0;
}

static int nunchuk_probe(struct i2c_client *client)
{
	u8 regs[NUNCHUK_REGS_LEN];
	int zpressed, cpressed;
	int ret;

	/* 初始化,使用不加密模式 */
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

	/* 第一次讀取只是觸發轉換,資料丟掉 */
	ret = nunchuk_read_registers(client, regs);
	if (ret) {
		dev_err(&client->dev, "first read failed: %d\n", ret);
		return ret;
	}

	ret = nunchuk_read_registers(client, regs);
	if (ret) {
		dev_err(&client->dev, "second read failed: %d\n", ret);
		return ret;
	}

	zpressed = !(regs[5] & BIT(0));
	cpressed = !(regs[5] & BIT(1));

	if (zpressed)
		dev_info(&client->dev, "Z button pressed\n");
	if (cpressed)
		dev_info(&client->dev, "C button pressed\n");
	
	dev_info(&client->dev, "nunchuk initialized\n");
	return 0;
}

static void nunchuk_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "remove called\n");
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


