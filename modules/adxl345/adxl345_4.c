// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - own driver for the BeagleBone Black labs
 *
 * Stage 3.1: spi_driver skeleton
 * Stage 3.2: read DEVID, refuse other chips
 * Stage 3.3: regmap, init sequence, standby on unbind
 * Stage 3.4: minimal IIO device, X/Y/Z raw + shared scale
 */
#include <linux/bitops.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

/*
 * SPI command byte = R/W bit | MB bit | 6-bit register address
 *   bit 7  1 = read, 0 = write
 *   bit 6  1 = multi-byte (address auto-increments), see Stage 1 T3/T4
 */
#define ADXL345_SPI_READ	BIT(7)
#define ADXL345_SPI_MB		BIT(6)

#define ADXL345_REG_DEVID	0x00
#define ADXL345_DEVID		0xE5

/* registers used by the init sequence */
#define ADXL345_REG_BW_RATE		0x2C
#define ADXL345_BW_RATE_100HZ		0x0A
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_POWER_CTL_MEASURE	BIT(3)
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_DATA_FORMAT_FULL_RES	BIT(3)
#define ADXL345_DATA_FORMAT_RANGE_16G	0x03
#define ADXL345_REG_MAX			0x39	/* FIFO_STATUS, last register */

/* NEW (3.4): data registers, 16-bit little endian, X0 X1 Y0 Y1 Z0 Z1 */
#define ADXL345_REG_DATAX0		0x32
#define ADXL345_REG_DATAY0		0x34
#define ADXL345_REG_DATAZ0		0x36

/*
 * NEW (3.4): full resolution = 3.9 mg/LSB in every range.
 * 3.9 mg * 9.80665 = 0.038246 m/s^2; upstream rounds it to 0.0383
 * (Stage 2 read 0.038300), we keep the same value to compare.
 * In full resolution at +-16 g the value is 13 bits, sign bit = bit 12.
 */
#define ADXL345_USCALE			38300
#define ADXL345_DATA_SIGN_BIT		12

/*
 * NEW (3.4): this struct lives right after struct iio_dev in one
 * allocation (devm_iio_device_alloc), reached with iio_priv().
 */
struct leo_adxl345 {
	struct spi_device *spi;
	struct regmap *regmap;
};

/*
 * NEW (3.4): one IIO channel per axis.
 *   modified + channel2 -> sysfs name in_accel_<x|y|z>_...
 *   address             -> first data register of the axis, used by read_raw
 *   info_mask_separate  -> one in_accel_x_raw per axis
 *   info_mask_shared_by_type -> one in_accel_scale for all accel channels
 */
#define ADXL345_CHANNEL(reg, axis) {				\
	.type = IIO_ACCEL,					\
	.modified = 1,						\
	.channel2 = IIO_MOD_##axis,				\
	.address = reg,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec leo_adxl345_channels[] = {
	ADXL345_CHANNEL(ADXL345_REG_DATAX0, X),
	ADXL345_CHANNEL(ADXL345_REG_DATAY0, Y),
	ADXL345_CHANNEL(ADXL345_REG_DATAZ0, Z),
};

/*
 * NEW (3.4): called by the IIO core for every sysfs read of a channel
 * attribute; mask says which one (raw or scale).
 */
static int leo_adxl345_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct leo_adxl345 *st = iio_priv(indio_dev);
	__le16 regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * Both bytes of one axis in a single multi-byte read (MB set by
		 * read_flag_mask), so low and high byte belong to the same
		 * sample. The three axes are still read at different times;
		 * the triggered buffer of 3.8 reads all 6 bytes at once.
		 * regmap-spi bounces through spi_write_then_read(), so a stack
		 * buffer is fine here.
		 */
		ret = regmap_bulk_read(st->regmap, chan->address, &regval,
				       sizeof(regval));
		if (ret)
			return ret;

		*val = sign_extend32(le16_to_cpu(regval), ADXL345_DATA_SIGN_BIT);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = ADXL345_USCALE;
		return IIO_VAL_INT_PLUS_MICRO;	/* 0 + 38300e-6 = "0.038300" */
	}

	return -EINVAL;
}

/* NEW (3.4): the callbacks of this IIO device */
static const struct iio_info leo_adxl345_info = {
	.read_raw = leo_adxl345_read_raw,
};

/*
 * describe the register map once, regmap builds the SPI
 * messages. For every read it ORs read_flag_mask into the address byte:
 * 0x80 (read) | 0x40 (MB), so multi-byte reads (regmap_bulk_read, 3.4)
 * auto-increment too. MB on a single-byte read is harmless.
 * Writes use the plain address (bit 7 = 0).
 */
static const struct regmap_config leo_adxl345_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = ADXL345_SPI_READ | ADXL345_SPI_MB,
	.max_register = ADXL345_REG_MAX,
};

/*
 * devm action, runs when the device is unbound (rmmod), after
 * remove() and before devm destroys the regmap (devm releases in reverse
 * order of registration, and the regmap was created first).
 */
static void leo_adxl345_standby(void *data)
{
	struct leo_adxl345 *st = data;

	regmap_write(st->regmap, ADXL345_REG_POWER_CTL, 0);
	dev_info(&st->spi->dev, "standby\n");
}

/*
 * POWER_CTL = 0 (standby) first: the datasheet recommends
 * configuring the device in standby, then enabling measurement last.
 * FULL_RES keeps 3.9 mg/LSB in every range, so with +-16 g the scale
 * stays 0.0383 m/s^2 (same as upstream, Stage 2).
 */
static int leo_adxl345_setup(struct leo_adxl345 *st)
{
	int ret;

	ret = regmap_write(st->regmap, ADXL345_REG_POWER_CTL, 0);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, ADXL345_REG_DATA_FORMAT,
			   ADXL345_DATA_FORMAT_FULL_RES |
			   ADXL345_DATA_FORMAT_RANGE_16G);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, ADXL345_REG_BW_RATE,
			   ADXL345_BW_RATE_100HZ);
	if (ret)
		return ret;

	return regmap_write(st->regmap, ADXL345_REG_POWER_CTL,
			    ADXL345_POWER_CTL_MEASURE);
}

/*
 * By the time probe runs, the SPI core has already parsed the DT node
 * (spi-cpol/spi-cpha -> mode, spi-max-frequency -> max_speed_hz,
 * interrupts -> irq) and called spi_setup().
 */
static int leo_adxl345_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;	/* NEW (3.4) */
	struct leo_adxl345 *st;
	unsigned int devid;	/* regmap values are unsigned int */
	int ret;

	/*
	 * NEW (3.4): replaces devm_kzalloc(): one allocation holds the
	 * iio_dev and our state behind it.
	 
	 *3.3：devm_kzalloc                  3.4：devm_iio_device_alloc
	┌───────────────────┐              ┌─────────────────────────┐
	│ struct leo_adxl345│ ← st         │ struct iio_dev          │ ← indio_dev
	│   spi, regmap     │              │   name, channels, info …│
	└───────────────────┘              ├─────────────────────────┤
        	                           │ struct leo_adxl345      │ ← st = iio_priv(indio_dev)
                	                   │   spi, regmap           │
                        	           └─────────────────────────┘
	 *
	 */

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	spi_set_drvdata(spi, indio_dev);	/* NEW (3.4): drvdata is the iio_dev */

	dev_info(dev, "probe: mode 0x%x (CPOL %d CPHA %d), %u Hz, %u bits/word, irq %d\n",
		 spi->mode,
		 !!(spi->mode & SPI_CPOL), !!(spi->mode & SPI_CPHA),
		 spi->max_speed_hz, spi->bits_per_word, spi->irq);

	st->regmap = devm_regmap_init_spi(spi, &leo_adxl345_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "cannot create regmap\n");

	/*
	 * SPI has no ACK (unlike I2C), so reading a known value is the only
	 * way to know something is really there.
	 */
	ret = regmap_read(st->regmap, ADXL345_REG_DEVID, &devid);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read DEVID\n");

	if (devid != ADXL345_DEVID)
		return dev_err_probe(dev, -ENODEV,
				     "DEVID 0x%02x, expected 0x%02x\n",
				     devid, ADXL345_DEVID);

	ret = leo_adxl345_setup(st);
	if (ret)
		return dev_err_probe(dev, ret, "init sequence failed\n");

	/*
	 * Register the undo right after the do. If adding the
	 * action fails, _or_reset runs it immediately, so the chip never
	 * stays in measurement mode without an owner.
	 */
	ret = devm_add_action_or_reset(dev, leo_adxl345_standby, st);
	if (ret)
		return ret;

	/* NEW (3.4): describe the IIO device */
	indio_dev->name = "leo-adxl345";
	indio_dev->info = &leo_adxl345_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = leo_adxl345_channels;
	indio_dev->num_channels = ARRAY_SIZE(leo_adxl345_channels);

	/*
	 * NEW (3.4): register LAST. From here user space can call read_raw,
	 * so the chip must already be set up. devm unregisters it FIRST on
	 * unbind (reverse order), before standby and before the regmap goes.
	 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register IIO device\n");

	dev_info(dev, "ADXL345 found (DEVID 0x%02x), measuring at 100 Hz\n",
		 devid);

	return 0;
}

/* void since 6.0; devm releases everything after this returns */
static void leo_adxl345_remove(struct spi_device *spi)
{
	dev_info(&spi->dev, "remove\n");
}

/* DT match */
static const struct of_device_id leo_adxl345_of_match[] = {
	{ .compatible = "leo,adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(of, leo_adxl345_of_match);

/* name = DT compatible without the vendor prefix (modalias) */
static const struct spi_device_id leo_adxl345_id[] = {
	{ "adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(spi, leo_adxl345_id);

static struct spi_driver leo_adxl345_driver = {
	.driver = {
		.name = "leo-adxl345",
		.of_match_table = leo_adxl345_of_match,
	},
	.probe = leo_adxl345_probe,
	.remove = leo_adxl345_remove,
	.id_table = leo_adxl345_id,
};
module_spi_driver(leo_adxl345_driver);

MODULE_AUTHOR("Leo <leochen1213@gmail.com>");
MODULE_DESCRIPTION("ADXL345 accelerometer driver (BBB labs, stage 3.4: IIO)");
MODULE_LICENSE("GPL");
