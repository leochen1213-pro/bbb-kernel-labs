// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306 - minimal driver for a 128x32 SSD1306 I2C OLED
 *
 * Step 3.2: probe() sends the init sequence and clears GDDRAM,
 *           remove() turns the display off.
 *
 * Every I2C write starts with a control byte:
 *   0x00 = the rest are commands, 0x40 = the rest are GDDRAM data.
 *
 * If writes are ACKed but the panel stays dark, power-cycle the module:
 * its RC reset sometimes leaves the controller in a bad state.
 */
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>

#define SSD1306_CTRL_CMD	0x00
#define SSD1306_CTRL_DATA	0x40

#define SSD1306_DISPLAY_OFF	0xAE
#define SSD1306_DISPLAY_ON	0xAF

#define SSD1306_WIDTH		128
#define SSD1306_HEIGHT		32
#define SSD1306_PAGES		(SSD1306_HEIGHT / 8)
#define SSD1306_FB_SIZE		(SSD1306_WIDTH * SSD1306_PAGES)	/* 512 */

/*
 * Same sequence as tools/oled_init.sh, minus the final DISPLAY_ON:
 * we clear GDDRAM first so the random power-on content never shows.
 */
static const u8 ssd1306_init_seq[] = {
	SSD1306_CTRL_CMD,
	SSD1306_DISPLAY_OFF,
	0xD5, 0x80,			/* clock divide / oscillator */
	0xA8, SSD1306_HEIGHT - 1, 	/* multiplex ratio */
	0xD3, 0x00,			/* display offset */
	0x40,				/* start line 0 */
	0x8D, 0x14,			/* charge pump on (takes effect at DISPLAY_ON) */
	0x20, 0x00,			/* horizontal addressing */
	0xA1,				/* segment remap */
	0xC8,				/* COM scan reversed */
	0xDA, 0x02,			/* COM pins: sequential (128x32) */
	0x81, 0x8F,			/* contrast */
	0xD9, 0xF1,			/* pre-charge */
	0xDB, 0x40,			/* VCOMH */
	0xA4,				/* follow RAM */
	0xA6,				/* normal (not inverted) */
	0x2E,				/* scroll off */
};

/* Column 0..127, page 0..3 */
static const u8 ssd1306_full_window[] = {
	SSD1306_CTRL_CMD,
	0x21, 0x00, SSD1306_WIDTH - 1,
	0x22, 0x00, SSD1306_PAGES - 1,
};

/* i2c_master_send() returns the byte count; turn a short write into -EIO */
static int ssd1306_send(struct i2c_client *client, const u8 *buf, int len)
{
	int ret = i2c_master_send(client, buf, len);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int ssd1306_cmd(struct i2c_client *client, u8 cmd)
{
	u8 buf[2] = { SSD1306_CTRL_CMD, cmd };

	return ssd1306_send(client, buf, sizeof(buf));
}

static int ssd1306_clear(struct i2c_client *client)
{
	u8 *buf;
	ktime_t t0;
	int ret;

	ret = ssd1306_send(client, ssd1306_full_window, sizeof(ssd1306_full_window));
	if (ret)	
		return ret;

	/* control byte + 512 zero bytes */
	buf = kzalloc(SSD1306_FB_SIZE + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf[0] = SSD1306_CTRL_DATA;

	t0 = ktime_get();
	ret = ssd1306_send(client, buf, SSD1306_FB_SIZE + 1);
	if (!ret)	/* baseline for step 3.5 (100 kHz vs 400 kHz) */
		dev_info(&client->dev, "%d-byte frame took %lld us\n", SSD1306_FB_SIZE + 1, ktime_us_delta(ktime_get(), t0));

	kfree(buf);
	return ret;
}

static int ssd1306_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP, "adapter does not support plain I2C\n");

	ret = ssd1306_send(client, ssd1306_init_seq, sizeof(ssd1306_init_seq));
	if (ret)
		return dev_err_probe(dev, ret, "init sequence failed\n");

	ret = ssd1306_clear(client);
	if (ret)
		return dev_err_probe(dev, ret, "clearing GDDRAM failed\n");

	ret = ssd1306_cmd(client, SSD1306_DISPLAY_ON);
	if (ret)
		return dev_err_probe(dev, ret, "display on failed\n");

	dev_info(dev, "%dx%d display on (addr 0x%02x)\n", SSD1306_WIDTH, SSD1306_HEIGHT, client->addr);
	return 0;
}

static void ssd1306_remove(struct i2c_client *client)
{
	/* remove() cannot fail: just report it */
	if (ssd1306_cmd(client, SSD1306_DISPLAY_OFF))
		dev_warn(&client->dev, "display off failed\n");
	else
		dev_info(&client->dev, "display off\n");
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
