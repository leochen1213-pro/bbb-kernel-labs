// SPDX-License-Identifier: GPL-2.0
/*
 * ADXL345 3-axis accelerometer - own driver for the BeagleBone Black labs
 *
 * Stage 3.1: spi_driver skeleton
 * Stage 3.2: read DEVID with spi_write_then_read(), refuse other chips
 */
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>

/*
 * NEW (3.2): SPI command byte = R/W bit | MB bit | 6-bit register address
 *   bit 7  1 = read, 0 = write
 *   bit 6  1 = multi-byte (address auto-increments), see Stage 1 T3/T4
 */
#define ADXL345_SPI_READ	BIT(7)
#define ADXL345_SPI_MB		BIT(6)

#define ADXL345_REG_DEVID	0x00
#define ADXL345_DEVID		0xE5

/* per-device state, grows in the next steps */
struct leo_adxl345 {
	struct spi_device *spi;
};

/*
 * NEW (3.2): one register read.
 * spi_write_then_read() is half duplex: it clocks out the command, then
 * clocks in len bytes. The byte received while the command goes out
 * (Stage 1: a leftover from the previous transfer) is thrown away for us.
 * It copies through a DMA-safe bounce buffer, so the stack variables are
 * fine here (compare X4 bug=3).
 */
static int leo_adxl345_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	u8 cmd = ADXL345_SPI_READ | reg;

	return spi_write_then_read(spi, &cmd, 1, val, 1);
}

/*
 * By the time probe runs, the SPI core has already parsed the
 * DT node (spi-cpol/spi-cpha -> mode, spi-max-frequency -> max_speed_hz,
 * interrupts -> irq) and called spi_setup().
 */
static int leo_adxl345_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct leo_adxl345 *st;
	u8 devid;
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

	/*
	 * NEW (3.2): talk to the chip before registering anything.
	 * SPI has no ACK (unlike I2C), so reading a known value is the only
	 * way to know something is really there.
	 * dev_err_probe(): prints and returns the error in one go, and stays
	 * quiet for -EPROBE_DEFER.
	 */
	ret = leo_adxl345_read_reg(spi, ADXL345_REG_DEVID, &devid);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read DEVID\n");

	if (devid != ADXL345_DEVID)
		return dev_err_probe(dev, -ENODEV,
				     "DEVID 0x%02x, expected 0x%02x\n",
				     devid, ADXL345_DEVID);

	dev_info(dev, "ADXL345 found (DEVID 0x%02x)\n", devid);

	return 0;
}

/* void since 6.0; devm releases st after this returns */
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
MODULE_DESCRIPTION("ADXL345 accelerometer driver (BBB labs, stage 3.2: DEVID check)");
MODULE_LICENSE("GPL");
