// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306 - minimal driver for a 128x32 SSD1306 I2C OLED
 *
 * Step 3.3: per-device state in struct ssd1306, a misc device /dev/oled,
 *           write() takes exactly one 512-byte frame (SSD1306 page format:
 *           fb[page * 128 + x], bit y % 8, bit 0 at the top).
 *
 * Every I2C write starts with a control byte:
 *   0x00 = the rest are commands, 0x40 = the rest are GDDRAM data.
 *
 * If writes are ACKed but the panel stays dark, power-cycle the module:
 * its RC reset sometimes leaves the controller in a bad state.
 */
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define SSD1306_CTRL_CMD	0x00
#define SSD1306_CTRL_DATA	0x40

#define SSD1306_DISPLAY_OFF	0xAE
#define SSD1306_DISPLAY_ON	0xAF

#define SSD1306_WIDTH		128
#define SSD1306_HEIGHT		32
#define SSD1306_PAGES		(SSD1306_HEIGHT / 8)
#define SSD1306_FB_SIZE		(SSD1306_WIDTH * SSD1306_PAGES)	/* 512 */

struct ssd1306 {
	struct i2c_client *client;
	struct miscdevice misc;
	/*
	 * Serialises frame transfers: one writer at a time owns buf[] and
	 * the controller's address pointer (window command + data must not
	 * interleave with another writer's). A mutex, not a spinlock:
	 * copy_from_user() and i2c_master_send() both sleep.
	 */
	struct mutex lock;
	u8 buf[1 + SSD1306_FB_SIZE];	/* control byte + one frame */
};

/*
 * Same sequence as tools/oled_init.sh, minus the final DISPLAY_ON:
 * we clear GDDRAM first so the random power-on content never shows.
 */
static const u8 ssd1306_init_seq[] = {
	SSD1306_CTRL_CMD,
	SSD1306_DISPLAY_OFF,
	0xD5, 0x80,		/* clock divide / oscillator */
	0xA8, SSD1306_HEIGHT - 1,	/* multiplex ratio */
	0xD3, 0x00,		/* display offset */
	0x40,			/* start line 0 */
	0x8D, 0x14,		/* charge pump on (takes effect at DISPLAY_ON) */
	0x20, 0x00,		/* horizontal addressing */
	0xA1,			/* segment remap */
	0xC8,			/* COM scan reversed */
	0xDA, 0x02,		/* COM pins: sequential (128x32) */
	0x81, 0x8F,		/* contrast */
	0xD9, 0xF1,		/* pre-charge */
	0xDB, 0x40,		/* VCOMH */
	0xA4,			/* follow RAM */
	0xA6,			/* normal (not inverted) */
	0x2E,			/* scroll off */
};

/* Column 0..127, page 0..3: resets the address pointer to (0, 0) */
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

/* Send buf[] to GDDRAM. Caller holds oled->lock (or has no concurrency yet). */
static int ssd1306_flush(struct ssd1306 *oled)
{
	int ret;

	ret = ssd1306_send(oled->client, ssd1306_full_window,
			   sizeof(ssd1306_full_window));
	if (ret)
		return ret;

	oled->buf[0] = SSD1306_CTRL_DATA;
	return ssd1306_send(oled->client, oled->buf, sizeof(oled->buf));
}

static ssize_t ssd1306_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	/* misc core sets private_data to our struct miscdevice on open */
	struct ssd1306 *oled = container_of(file->private_data,
					    struct ssd1306, misc);
	int ret;

	/* One whole frame per write(); the offset is ignored (pwrite at 0 is fine) */
	if (count != SSD1306_FB_SIZE)
		return -EINVAL;

	if (mutex_lock_interruptible(&oled->lock))
		return -ERESTARTSYS;

	/* Sleeping here is fine: we hold a mutex, not a spinlock */
	if (copy_from_user(&oled->buf[1], ubuf, SSD1306_FB_SIZE)) {
		ret = -EFAULT;
		goto out;
	}

	ret = ssd1306_flush(oled);
out:
	mutex_unlock(&oled->lock);
	return ret ? ret : count;
}

static const struct file_operations ssd1306_fops = {
	.owner = THIS_MODULE,	/* rmmod is refused while /dev/oled is open */
	.write = ssd1306_write,
};

static int ssd1306_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ssd1306 *oled;
	ktime_t t0;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter does not support plain I2C\n");

	oled = devm_kzalloc(dev, sizeof(*oled), GFP_KERNEL);
	if (!oled)
		return -ENOMEM;

	oled->client = client;
	mutex_init(&oled->lock);
	i2c_set_clientdata(client, oled);

	ret = ssd1306_send(client, ssd1306_init_seq, sizeof(ssd1306_init_seq));
	if (ret)
		return dev_err_probe(dev, ret, "init sequence failed\n");

	/* buf[] is already zeroed by devm_kzalloc(): this clears GDDRAM */
	t0 = ktime_get();
	ret = ssd1306_flush(oled);
	if (ret)
		return dev_err_probe(dev, ret, "clearing GDDRAM failed\n");
	dev_info(dev, "%zu-byte frame took %lld us\n",
		 sizeof(oled->buf), ktime_us_delta(ktime_get(), t0));

	ret = ssd1306_cmd(client, SSD1306_DISPLAY_ON);
	if (ret)
		return dev_err_probe(dev, ret, "display on failed\n");

	/* Last: user space can reach us only once the hardware is ready */
	oled->misc.minor = MISC_DYNAMIC_MINOR;
	oled->misc.name = "oled";
	oled->misc.fops = &ssd1306_fops;
	oled->misc.parent = dev;
	ret = misc_register(&oled->misc);
	if (ret) {
		ssd1306_cmd(client, SSD1306_DISPLAY_OFF);
		return dev_err_probe(dev, ret, "misc_register failed\n");
	}

	dev_info(dev, "%dx%d display on, /dev/%s ready\n",
		 SSD1306_WIDTH, SSD1306_HEIGHT, oled->misc.name);
	return 0;
}

static void ssd1306_remove(struct i2c_client *client)
{
	struct ssd1306 *oled = i2c_get_clientdata(client);

	/* First: no new open() can reach us */
	misc_deregister(&oled->misc);

	/* Wait for an in-flight frame, then turn the panel off */
	mutex_lock(&oled->lock);
	if (ssd1306_cmd(client, SSD1306_DISPLAY_OFF))
		dev_warn(&client->dev, "display off failed\n");
	else
		dev_info(&client->dev, "display off\n");
	mutex_unlock(&oled->lock);

	mutex_destroy(&oled->lock);
	/* oled itself is freed by devm after remove() returns */
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
