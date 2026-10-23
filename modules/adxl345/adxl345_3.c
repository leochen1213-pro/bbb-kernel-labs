// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - own driver for the BeagleBone Black labs
 *
 * Stage 3.1: spi_driver skeleton
 * Stage 3.2: read DEVID, refuse other chips
 * Stage 3.3: regmap, init sequence, standby on unbind
 */
#include <linux/bits.h>
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

/* NEW (3.3): registers used by the init sequence */
#define ADXL345_REG_BW_RATE		0x2C
#define ADXL345_BW_RATE_100HZ		0x0A
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_POWER_CTL_MEASURE	BIT(3)
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_DATA_FORMAT_FULL_RES	BIT(3)
#define ADXL345_DATA_FORMAT_RANGE_16G	0x03
#define ADXL345_REG_MAX			0x39	/* FIFO_STATUS, last register */

struct leo_adxl345 {
	struct spi_device *spi;
	struct regmap *regmap;		/* NEW (3.3) */
};

/*
 * NEW (3.3): describe the register map once, regmap builds the SPI
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
 * NEW (3.3): devm action, runs when the device is unbound (rmmod), after
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
 * NEW (3.3): POWER_CTL = 0 (standby) first: the datasheet recommends
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
	struct leo_adxl345 *st;
	unsigned int devid;	/* NEW (3.3): regmap values are unsigned int */
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->spi = spi;
	spi_set_drvdata(spi, st);

	dev_info(dev, "probe: mode 0x%x (CPOL %d CPHA %d), %u Hz, %u bits/word, irq %d\n",
		 spi->mode,
		 !!(spi->mode & SPI_CPOL), !!(spi->mode & SPI_CPHA),
		 spi->max_speed_hz, spi->bits_per_word, spi->irq);

	/* NEW (3.3): replaces leo_adxl345_read_reg() / spi_write_then_read() */
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

	/* NEW (3.3) */
	ret = leo_adxl345_setup(st);
	if (ret)
		return dev_err_probe(dev, ret, "init sequence failed\n");

	/*
	 * NEW (3.3): register the undo right after the do. If adding the
	 * action fails, _or_reset runs it immediately, so the chip never
	 * stays in measurement mode without an owner.
	 */
	ret = devm_add_action_or_reset(dev, leo_adxl345_standby, st);
	if (ret)
		return ret;

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
MODULE_DESCRIPTION("ADXL345 accelerometer driver (BBB labs, stage 3.3: regmap)");
MODULE_LICENSE("GPL");
