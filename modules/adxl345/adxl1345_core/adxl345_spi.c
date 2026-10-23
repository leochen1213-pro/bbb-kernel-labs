// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - SPI bus driver (BBB labs)
 * NEW (3.6): split out of adxl345.c. Only knows how to build an SPI
 * regmap; everything else is in adxl345_core.c.
 */
#include <linux/bits.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include "adxl345.h"

/*
 * SPI command byte = R/W bit | MB bit | 6-bit register address
 *   bit 7  1 = read, 0 = write
 *   bit 6  1 = multi-byte (address auto-increments), see Stage 1 T3/T4
 */
#define ADXL345_SPI_READ	BIT(7)
#define ADXL345_SPI_MB		BIT(6)

/*
 * For every read regmap ORs read_flag_mask into the address byte:
 * 0x80 (read) | 0x40 (MB), so multi-byte reads auto-increment too.
 * Writes use the plain address (bit 7 = 0).
 */
static const struct regmap_config leo_adxl345_spi_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = ADXL345_SPI_READ | ADXL345_SPI_MB,
	.max_register = ADXL345_REG_MAX,
};

/*
 * By the time probe runs, the SPI core has already parsed the DT node
 * (spi-cpol/spi-cpha -> mode, spi-max-frequency -> max_speed_hz,
 * interrupts -> irq) and called spi_setup().
 */
static int leo_adxl345_spi_probe(struct spi_device *spi)
{
	struct regmap *regmap;

	dev_info(&spi->dev, "spi: mode 0x%x (CPOL %d CPHA %d), %u Hz, %u bits/word\n",
		 spi->mode,
		 !!(spi->mode & SPI_CPOL), !!(spi->mode & SPI_CPHA),
		 spi->max_speed_hz, spi->bits_per_word);

	regmap = devm_regmap_init_spi(spi, &leo_adxl345_spi_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&spi->dev, PTR_ERR(regmap),
				     "cannot create SPI regmap\n");

	return leo_adxl345_core_probe(&spi->dev, regmap, spi->irq);
}

/* DT match */
static const struct of_device_id leo_adxl345_spi_of_match[] = {
	{ .compatible = "leo,adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(of, leo_adxl345_spi_of_match);

/* name = DT compatible without the vendor prefix (modalias) */
static const struct spi_device_id leo_adxl345_spi_id[] = {
	{ "adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(spi, leo_adxl345_spi_id);

/*
 * No .remove: everything (regmap, standby, IIO device) is devm, released
 * when the device is unbound.
 */
static struct spi_driver leo_adxl345_spi_driver = {
	.driver = {
		.name = "leo-adxl345-spi",
		.of_match_table = leo_adxl345_spi_of_match,
	},
	.probe = leo_adxl345_spi_probe,
	.id_table = leo_adxl345_spi_id,
};
module_spi_driver(leo_adxl345_spi_driver);

MODULE_AUTHOR("Leo <leochen1213@gmail.com>");
MODULE_DESCRIPTION("ADXL345 accelerometer SPI driver (BBB labs, stage 3.6)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(LEO_ADXL345);	/* leo_adxl345_core_probe() */
