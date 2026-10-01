#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>

static int nunchuk_probe(struct i2c_client *client)
{
	dev_info(&client->dev, "probe called, address 0x%02x\n", client->addr);
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
