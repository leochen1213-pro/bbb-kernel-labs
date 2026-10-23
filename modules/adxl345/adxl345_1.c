// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - own driver for the BeagleBone Black labs
 *
 * Stage 3.1: spi_driver skeleton
 *   - matched from DT by "leo,adxl345" (of_device_id)
 *   - spi_device_id table as well (SPI core warns without it)
 *   - probe only reports what the SPI core prepared from DT
 */
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>

/* NEW (3.1): per-device state, grows in the next steps */
struct leo_adxl345 {
	struct spi_device *spi;
};

/*
 * NEW (3.1): by the time probe runs, the SPI core has already parsed the
 * DT node (spi-cpol/spi-cpha -> mode, spi-max-frequency -> max_speed_hz,
 * interrupts -> irq) and called spi_setup().
 */
static int leo_adxl345_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct leo_adxl345 *st;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->spi = spi;
	spi_set_drvdata(spi, st);

	dev_info(dev, "probe: mode 0x%x (CPOL %d CPHA %d), %u Hz, %u bits/word, irq %d\n",
		 spi->mode,
		 !!(spi->mode & SPI_CPOL), !!(spi->mode & SPI_CPHA),
		 spi->max_speed_hz, spi->bits_per_word, spi->irq);

	return 0;
}

/* NEW (3.1): void since 6.0; devm releases st after this returns */
static void leo_adxl345_remove(struct spi_device *spi)
{
	dev_info(&spi->dev, "remove\n");
}

/* NEW (3.1): DT match */
static const struct of_device_id leo_adxl345_of_match[] = {
	{ .compatible = "leo,adxl345" },
	{ }
};
MODULE_DEVICE_TABLE(of, leo_adxl345_of_match);

/* NEW (3.1): name = DT compatible without the vendor prefix (modalias) */
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
MODULE_DESCRIPTION("ADXL345 accelerometer driver (BBB labs, stage 3.1: SPI skeleton)");
MODULE_LICENSE("GPL");
