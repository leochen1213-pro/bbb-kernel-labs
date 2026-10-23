// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - own driver for the BeagleBone Black labs
 *
 * Stage 3.1: spi_driver skeleton
 * Stage 3.2: read DEVID, refuse other chips
 * Stage 3.3: regmap, init sequence, standby on unbind
 * Stage 3.4: minimal IIO device, X/Y/Z raw + shared scale
 * Stage 3.5: write_raw (scale = range, sampling_frequency, calibbias),
 *            *_available, mutex
 * Experiment A: bug=1 drops the mutex around the scale (range) update
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

/*
 * NEW (exp A): 0 = correct driver
 *              1 = no mutex around "range register + st->range", and a
 *                  random sleep between the two steps to widen the race
 *                  window
 *              2 = same sleep and logs as 1, but WITH the mutex: the
 *                  control that shows the lock closes that window
 */
static unsigned int bug;
module_param(bug, uint, 0444);
MODULE_PARM_DESC(bug, "0 = correct, 1 = exp A without mutex, 2 = exp A with mutex");

/*
 * SPI command byte = R/W bit | MB bit | 6-bit register address
 *   bit 7  1 = read, 0 = write
 *   bit 6  1 = multi-byte (address auto-increments), see Stage 1 T3/T4
 */
#define ADXL345_SPI_READ	BIT(7)
#define ADXL345_SPI_MB		BIT(6)

#define ADXL345_REG_DEVID	0x00
#define ADXL345_DEVID		0xE5

/* offset registers, s8, 15.6 mg/LSB, added by the chip */
#define ADXL345_REG_OFSX		0x1E
#define ADXL345_REG_OFS(mod)		(ADXL345_REG_OFSX + (mod) - IIO_MOD_X)

/* registers used by the init sequence */
#define ADXL345_REG_BW_RATE		0x2C
#define ADXL345_BW_RATE_RATE_MASK	GENMASK(3, 0)
#define ADXL345_BW_RATE_100HZ		0x0A
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_POWER_CTL_MEASURE	BIT(3)
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_DATA_FORMAT_RANGE_MASK	GENMASK(1, 0)
#define ADXL345_REG_MAX			0x39	/* FIFO_STATUS, last register */

/* data registers, 16-bit little endian, X0 X1 Y0 Y1 Z0 Z1 */
#define ADXL345_REG_DATAX0		0x32
#define ADXL345_REG_DATAY0		0x34
#define ADXL345_REG_DATAZ0		0x36

/*
 * FULL_RES is no longer set. In full resolution the LSB is
 * always 3.9 mg, so changing the range would not change the scale and
 * "write the scale" would mean nothing. In 10-bit mode the LSB doubles
 * with every range step, so scale <-> range is one to one:
 *   range  +-2 g   +-4 g   +-8 g   +-16 g
 *   LSB    3.9 mg  7.8 mg  15.6 mg 31.2 mg
 * 10-bit two's complement: sign bit = bit 9.
 * Default +-2 g keeps the 3.4 numbers (0.0383, Z ~ +245).
 */
#define ADXL345_DATA_SIGN_BIT		9

static const int leo_adxl345_scale_avail[] = {
	0,  38300,	/* +-2 g  (range bits 0) */
	0,  76600,	/* +-4 g  (range bits 1) */
	0, 153200,	/* +-8 g  (range bits 2) */
	0, 306400,	/* +-16 g (range bits 3) */
};

/*
 * output data rate = 3200 Hz / 2^(15 - code).
 * Codes 0x7..0xF = 12.5 .. 3200 Hz; index i <-> code 0x7 + i.
 */
#define ADXL345_ODR_FIRST_CODE		0x7
static const int leo_adxl345_odr_avail[] = {
	  12, 500000,
	  25,      0,
	  50,      0,
	 100,      0,
	 200,      0,
	 400,      0,
	 800,      0,
	1600,      0,
	3200,      0,
};

/*
 * This struct lives right after struct iio_dev in one allocation
 * (devm_iio_device_alloc), reached with iio_priv().
 */
struct leo_adxl345 {
	struct spi_device *spi;
	struct regmap *regmap;

	/*
	 * lock protects the invariant
	 *   "range bits in DATA_FORMAT == st->range"
	 * and serializes every multi-step access to the chip from sysfs.
	 * regmap's own lock only covers one regmap_*() call at a time.
	 * A mutex (not a spinlock): SPI transfers sleep.
	 */
	struct mutex lock;
	unsigned int range;	/* cached range bits, 0..3 */
};

/*
 * One IIO channel per axis.
 *   modified + channel2 -> sysfs name in_accel_<x|y|z>_...
 *   address             -> first data register of the axis, used by read_raw
 *   info_mask_separate  -> one file per axis (raw, calibbias)
 *   info_mask_shared_by_type -> one file for all accel channels
 *   ..._available -> in_accel_scale_available etc.
 */
#define ADXL345_CHANNEL(reg, axis) {					\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.address = reg,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_CALIBBIAS),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SCALE) | \
					      BIT(IIO_CHAN_INFO_SAMP_FREQ), \
}

static const struct iio_chan_spec leo_adxl345_channels[] = {
	ADXL345_CHANNEL(ADXL345_REG_DATAX0, X),
	ADXL345_CHANNEL(ADXL345_REG_DATAY0, Y),
	ADXL345_CHANNEL(ADXL345_REG_DATAZ0, Z),
};

/*
 * Called by the IIO core for every sysfs read of a channel attribute;
 * mask says which one.
 */
static int leo_adxl345_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct leo_adxl345 *st = iio_priv(indio_dev);
	unsigned int regval;
	__le16 data;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * Both bytes of one axis in a single multi-byte read (MB set by
		 * read_flag_mask), so low and high byte belong to the same
		 * sample. regmap-spi bounces through spi_write_then_read(), so
		 * a stack buffer is fine here.
		 * under the lock, so a raw value is never read in
		 * the middle of a range change.
		 */
		mutex_lock(&st->lock);
		ret = regmap_bulk_read(st->regmap, chan->address, &data,
				       sizeof(data));
		mutex_unlock(&st->lock);
		if (ret)
			return ret;

		*val = sign_extend32(le16_to_cpu(data), ADXL345_DATA_SIGN_BIT);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* from the cached range, no SPI transfer */
		if (bug != 1)			/* NEW (exp A) */
			mutex_lock(&st->lock);
		*val = leo_adxl345_scale_avail[st->range * 2];
		*val2 = leo_adxl345_scale_avail[st->range * 2 + 1];
		if (bug != 1)			/* NEW (exp A) */
			mutex_unlock(&st->lock);
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		/* one register, read straight from the chip */
		ret = regmap_read(st->regmap, ADXL345_REG_BW_RATE, &regval);
		if (ret)
			return ret;

		regval = (regval & ADXL345_BW_RATE_RATE_MASK) -
			 ADXL345_ODR_FIRST_CODE;
		if (regval >= ARRAY_SIZE(leo_adxl345_odr_avail) / 2)
			return -EINVAL;	/* a code we never write */

		*val = leo_adxl345_odr_avail[regval * 2];
		*val2 = leo_adxl345_odr_avail[regval * 2 + 1];
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_CALIBBIAS:
		/* OFSx is a signed byte */
		ret = regmap_read(st->regmap, ADXL345_REG_OFS(chan->channel2),
				  &regval);
		if (ret)
			return ret;

		*val = sign_extend32(regval, 7);
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

/*
 * find val.val2 in a {int, micro} table, return its index.
 * Only exact matches: user space writes one of the *_available strings.
 */
static int leo_adxl345_find(const int *tbl, int n, int val, int val2)
{
	int i;

	for (i = 0; i < n; i++)
		if (tbl[2 * i] == val && tbl[2 * i + 1] == val2)
			return i;

	return -EINVAL;
}

/*
 * sysfs writes. The IIO core parses the string with the
 * default format IIO_VAL_INT_PLUS_MICRO: "0.076600" -> 0, 76600 and
 * "-4" -> -4, 0.
 */
static int leo_adxl345_write_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int val, int val2, long mask)
{
	struct leo_adxl345 *st = iio_priv(indio_dev);
	int i, ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		i = leo_adxl345_find(leo_adxl345_scale_avail,
				     ARRAY_SIZE(leo_adxl345_scale_avail) / 2,
				     val, val2);
		if (i < 0)
			return i;

		/*
		 * Two steps that must stay together: the register and the
		 * cached copy. Without the lock, two writers can interleave
		 * (A writes reg, B writes reg, B updates cache, A updates
		 * cache) and leave register and cache disagreeing for good:
		 * experiment A.
		 */
		if (bug != 1)			/* NEW (exp A) */
			mutex_lock(&st->lock);
		if (bug)			/* NEW (exp A): show the interleaving */
			dev_info(&st->spi->dev, "exp A: pid %d range %d -> register\n",
				 current->pid, i);
		ret = regmap_update_bits(st->regmap, ADXL345_REG_DATA_FORMAT,
					 ADXL345_DATA_FORMAT_RANGE_MASK, i);
		/*
		 * NEW (exp A): sleep between the two steps. Sleeping lets the
		 * scheduler run the other writer right here, even on this
		 * single-core CPU. The length is random (20-100 ms):
		 * - random: with a fixed length the writer that went to sleep
		 *   first also wakes up first, and the later cache update wins
		 *   (consistent end state);
		 * - long: the second shell writer is a forked subshell and
		 *   may start several ms after the first; with 1-5 ms the
		 *   window could close before it arrives (the dev_info lines
		 *   show whether the two writes really overlap).
		 */
		if (bug)
			msleep(20 + get_random_u32_below(80));
		if (!ret)
			st->range = i;
		if (bug)			/* NEW (exp A) */
			dev_info(&st->spi->dev, "exp A: pid %d range %d -> cache\n",
				 current->pid, i);
		if (bug != 1)			/* NEW (exp A) */
			mutex_unlock(&st->lock);
		return ret;

	case IIO_CHAN_INFO_SAMP_FREQ:
		i = leo_adxl345_find(leo_adxl345_odr_avail,
				     ARRAY_SIZE(leo_adxl345_odr_avail) / 2,
				     val, val2);
		if (i < 0)
			return i;

		/* one regmap call: regmap's own lock is enough */
		return regmap_update_bits(st->regmap, ADXL345_REG_BW_RATE,
					  ADXL345_BW_RATE_RATE_MASK,
					  ADXL345_ODR_FIRST_CODE + i);

	case IIO_CHAN_INFO_CALIBBIAS:
		if (val2 || val < S8_MIN || val > S8_MAX)
			return -EINVAL;

		return regmap_write(st->regmap,
				    ADXL345_REG_OFS(chan->channel2),
				    (u8)val);
	}

	return -EINVAL;
}

/*
 * backs the *_available files. IIO_AVAIL_LIST = a list of
 * values (IIO_AVAIL_RANGE would be min, step, max).
 */
static int leo_adxl345_read_avail(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  const int **vals, int *type, int *length,
				  long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*vals = leo_adxl345_scale_avail;
		*length = ARRAY_SIZE(leo_adxl345_scale_avail);
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = leo_adxl345_odr_avail;
		*length = ARRAY_SIZE(leo_adxl345_odr_avail);
		break;
	default:
		return -EINVAL;
	}

	*type = IIO_VAL_INT_PLUS_MICRO;
	return IIO_AVAIL_LIST;
}

/* the callbacks of this IIO device */
static const struct iio_info leo_adxl345_info = {
	.read_raw = leo_adxl345_read_raw,
	.write_raw = leo_adxl345_write_raw,
	.read_avail = leo_adxl345_read_avail,
};

/*
 * Describe the register map once, regmap builds the SPI messages. For
 * every read it ORs read_flag_mask into the address byte: 0x80 (read) |
 * 0x40 (MB), so multi-byte reads auto-increment too. MB on a single-byte
 * read is harmless. Writes use the plain address (bit 7 = 0).
 */
static const struct regmap_config leo_adxl345_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = ADXL345_SPI_READ | ADXL345_SPI_MB,
	.max_register = ADXL345_REG_MAX,
};

/*
 * devm action, runs when the device is unbound (rmmod), after remove()
 * and before devm destroys the regmap (devm releases in reverse order of
 * registration, and the regmap was created first).
 */
static void leo_adxl345_standby(void *data)
{
	struct leo_adxl345 *st = data;

	regmap_write(st->regmap, ADXL345_REG_POWER_CTL, 0);
	dev_info(&st->spi->dev, "standby\n");
}

/*
 * POWER_CTL = 0 (standby) first: the datasheet recommends configuring
 * the device in standby, then enabling measurement last.
 * DATA_FORMAT = +-2 g, 10-bit (FULL_RES off, see above),
 * and the cached range starts in sync with it.
 * OFSX/Y/Z are not touched: they reset to 0 at power-up and keep what
 * user space wrote while the chip stays powered.
 */
static int leo_adxl345_setup(struct leo_adxl345 *st)
{
	int ret;

	ret = regmap_write(st->regmap, ADXL345_REG_POWER_CTL, 0);
	if (ret)
		return ret;

	st->range = 0;
	ret = regmap_write(st->regmap, ADXL345_REG_DATA_FORMAT, st->range);
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
	struct iio_dev *indio_dev;
	struct leo_adxl345 *st;
	unsigned int devid;	/* regmap values are unsigned int */
	int ret;

	/* one allocation holds the iio_dev and our state behind it */
	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	mutex_init(&st->lock);		/* before anyone can use it */
	spi_set_drvdata(spi, indio_dev);

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
	 * Register the undo right after the do. If adding the action fails,
	 * _or_reset runs it immediately, so the chip never stays in
	 * measurement mode without an owner.
	 */
	ret = devm_add_action_or_reset(dev, leo_adxl345_standby, st);
	if (ret)
		return ret;

	/* describe the IIO device */
	indio_dev->name = "leo-adxl345";
	indio_dev->info = &leo_adxl345_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = leo_adxl345_channels;
	indio_dev->num_channels = ARRAY_SIZE(leo_adxl345_channels);

	/*
	 * Register LAST. From here user space can call read_raw/write_raw,
	 * so the chip must already be set up. devm unregisters it first of
	 * the devm resources on unbind (still after remove()).
	 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register IIO device\n");

	dev_info(dev, "ADXL345 found (DEVID 0x%02x), +-2 g, 100 Hz, bug=%u\n",
		 devid, bug);	/* NEW (exp A): bug */

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
MODULE_DESCRIPTION("ADXL345 accelerometer driver (BBB labs, stage 3.5: write_raw + mutex)");
MODULE_LICENSE("GPL");
