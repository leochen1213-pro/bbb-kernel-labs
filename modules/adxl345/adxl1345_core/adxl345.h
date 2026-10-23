/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ADXL345 - what the core shares with the SPI and I2C bus drivers
 * NEW (3.6)
 */
#ifndef _LEO_ADXL345_H
#define _LEO_ADXL345_H

struct device;
struct regmap;

/* last register (FIFO_STATUS), used by both regmap configs */
#define ADXL345_REG_MAX		0x39

int leo_adxl345_core_probe(struct device *dev, struct regmap *regmap, int irq);

#endif /* _LEO_ADXL345_H */
