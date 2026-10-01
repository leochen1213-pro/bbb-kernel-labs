// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/bits.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>

#define NUNCHUK_REGS_LEN	6
#define NUNCHUK_POLL_MS		50

struct nunchuk_dev {
	struct i2c_client *i2c_client;
};

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

static void nunchuk_poll(struct input_dev *input)
{
	struct nunchuk_dev *nunchuk = input_get_drvdata(input);
	struct i2c_client *client = nunchuk->i2c_client;
	u8 regs[NUNCHUK_REGS_LEN];
	int zpressed, cpressed;

	if (nunchuk_read_registers(client, regs))
		return;

	zpressed = !(regs[5] & BIT(0));
	cpressed = !(regs[5] & BIT(1));

	input_report_key(input, BTN_Z, zpressed);
	input_report_key(input, BTN_C, cpressed);
	input_sync(input);
}

static int nunchuk_probe(struct i2c_client *client)
{
	struct nunchuk_dev *nunchuk;
	struct input_dev *input;
	int ret;

	nunchuk = devm_kzalloc(&client->dev, sizeof(*nunchuk), GFP_KERNEL);
	if (!nunchuk)
		return -ENOMEM;

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

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	nunchuk->i2c_client = client;
	input_set_drvdata(input, nunchuk);

	input->name = "Wii Nunchuk";
	input->id.bustype = BUS_I2C;

	set_bit(EV_KEY, input->evbit);
	set_bit(BTN_C, input->keybit);
	set_bit(BTN_Z, input->keybit);

	ret = input_setup_polling(input, nunchuk_poll);
	if (ret) {
		dev_err(&client->dev, "failed to set up polling: %d\n", ret);
		return ret;
	}
	input_set_poll_interval(input, NUNCHUK_POLL_MS);

	ret = input_register_device(input);
	if (ret) {
		dev_err(&client->dev, "failed to register input device: %d\n", ret);
		return ret;
	}

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
