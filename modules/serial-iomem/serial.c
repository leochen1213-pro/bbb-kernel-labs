// SPDX-License-Identifier: GPL-2.0
/*
 * I/O memory lab review - Step 6
 * Send characters from probe() by polling LSR.THRE and writing THR.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>

#include <linux/pm_runtime.h>		/* pm_runtime_*() */
#include <linux/serial_reg.h>		/* UART_LSR, UART_OMAP_* register indexes */
#include <linux/of.h>			/* of_property_read_u32() */

#define SERIAL_BAUD	115200		

/*
 * skip_pm=1 reads the registers WITHOUT enabling the module clock first,
 * to see what happens on real hardware. Expect a kernel Oops, then reboot.
 */
static bool skip_pm;
module_param(skip_pm, bool, 0444);
MODULE_PARM_DESC(skip_pm, "1 = read registers without runtime PM (crash experiment)");

struct serial_dev {
	void __iomem *regs;	
};

/*
 * register accessors.
 * serial_reg.h gives register INDEXES (UART_LSR = 5). On OMAP each register
 * sits on a 32-bit boundary, so the byte offset is index * 4.
 * readl()/writel(): exact 32-bit access, not reordered/optimized away,
 * with the barriers ARM needs for device memory.
 */
static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}
 
static void reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}


/*
 * NEW (Step 6): send one character (polling / PIO).
 * 1. Busy-wait until LSR.THRE (bit 5) = 1: the Transmit Holding Register is
 *    empty and can take a new byte. cpu_relax() tells the CPU/compiler we are
 *    spinning (and keeps the loop from being optimized into something odd).
 * 2. Write the byte to UART_TX (index 0 = THR while LCR.DLAB = 0).
 * Note: no timeout here (lab version). If the UART is not clocked/enabled this
 * loops forever -> a production driver would use readl_poll_timeout_atomic().
 */

static void serial_write_char(struct serial_dev *serial, char c)
{
	while (!(reg_read(serial, UART_LSR) & UART_LSR_THRE))
		cpu_relax();

	reg_write(serial, c, UART_TX);
}

/* NEW (Step 6): send a string, character by character */
static void serial_write_str(struct serial_dev *serial, const char *s)
{
	while (*s)
		serial_write_char(serial, *s++);
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;	
	struct resource *res;		
	resource_size_t size;	
	
	//Add
	u32 mver, syss, lsr;
	int ret;

	unsigned int uartclk, baud_divisor;	/* NEW (Step 5) */
	u32 lcr, mdr1, dll, dlm;		/* NEW (Step 5): read-back check */

	dev_info(&pdev->dev, "Called %s\n", __func__);

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	dev_info(&pdev->dev, "phys %pa size %pa -> virt %p (raw %px)\n",
		 &res->start, &size, serial->regs, serial->regs);


	/*
	 * power up the device before touching any register.
	 * pm_runtime_enable(): allow runtime PM for this device.
	 * pm_runtime_resume_and_get(): take a usage reference and resume the
	 *   device NOW. The parent (ti-sysc "target-module") is resumed first
	 *   and enables the UART module clock.
	 *   (The lab uses pm_runtime_get_sync(); resume_and_get() is the modern
	 *   variant that drops the reference by itself if resuming fails.)
	 */
	if (!skip_pm) {
		pm_runtime_enable(&pdev->dev);
		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret < 0) {
			pm_runtime_disable(&pdev->dev);
			return ret;
		}
	} 
	else {
		dev_warn(&pdev->dev, "skip_pm=1: reading registers with the clock possibly OFF!\n");
	}

	/* read a few registers to prove we talk to the UART */
	mver = reg_read(serial, UART_OMAP_MVER);	/* offset 0x50: IP revision */
	syss = reg_read(serial, UART_OMAP_SYSS);	/* offset 0x58: bit0 RESETDONE */
	lsr  = reg_read(serial, UART_LSR);		/* offset 0x14: expect 0x60 (TX empty) */

	dev_info(&pdev->dev, "MVER 0x%08x  SYSS 0x%08x  LSR 0x%02x\n", mver, syss, lsr);
	
	/*
	 * NEW (Step 5) (1): get the UART functional clock from the Device Tree.
	 * "clock-frequency" is inherited from the SoC .dtsi (48 MHz on AM335x).
	 * From here on, an error must undo runtime PM -> goto err_pm.
	 */
	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency", &uartclk);
	if (ret) {
		dev_err(&pdev->dev, "clock-frequency property not found in Device Tree\n");
		goto err_pm;
	}

	/*
	 * NEW (Step 5) (2): baud rate divisor, UART 16x mode:
	 *   baud = uartclk / (16 * divisor)  =>  divisor = uartclk / 16 / baud
	 *   48000000 / 16 / 115200 = 26.04 -> 26 (integer division)
	 *   real baud = 48000000 / 16 / 26 = 115384.6 -> +0.16 % error, fine (< ~2 %)
	 */
	baud_divisor = uartclk / 16 / SERIAL_BAUD;
	
	/*
	 * NEW (Step 5) (3): configure line + baud rate. ORDER MATTERS.
	 *
	 * MDR1 = 0x7 : MODESELECT = "disable". TRM: the UART must be disabled
	 *              while the divisor (DLL/DLH) is changed.
	 * LCR  = 0x0 : start from a known state (DLAB=0, config modes off).
	 * LCR  = DLAB: bit 7 = 1 -> offsets 0x00/0x04 now access DLL/DLH
	 *              instead of RHR-THR/IER ("register banking").
	 * DLL/DLM    : low/high byte of the divisor (26 = 0x1a, 0x00).
	 * LCR  = WLEN8 (0x03): 8 data bits, no parity, 1 stop bit (8N1),
	 *              and DLAB back to 0 -> offsets 0x00/0x04 are THR/IER again.
	 * MDR1 = 0x0 : MODESELECT = "UART 16x mode" -> UART enabled.
	 */
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	/*
	 * NEW (Step 5) (4): reset both FIFOs.
	 * FCR bit1 = clear RX FIFO, bit2 = clear TX FIFO (self-clearing).
	 * Bit0 (FIFO enable) stays 0 -> FIFOs disabled, 1-byte holding registers.
	 * (That is why RX could overrun in the interrupt/locking labs.)
	 */
	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);
 	
	/*
	 * NEW (Step 5) (5): read back to verify the configuration.
	 * DLL/DLM are only visible while LCR.DLAB = 1, so switch bank,
	 * read them, then restore LCR.
	 */
	lcr  = reg_read(serial, UART_LCR);
	mdr1 = reg_read(serial, UART_OMAP_MDR1);
	reg_write(serial, lcr | UART_LCR_DLAB, UART_LCR);
	dll  = reg_read(serial, UART_DLL);
	dlm  = reg_read(serial, UART_DLM);
	reg_write(serial, lcr, UART_LCR);

	dev_info(&pdev->dev, "uartclk %u Hz, divisor %u -> LCR 0x%02x MDR1 0x%02x DLL 0x%02x DLM 0x%02x\n",
		 uartclk, baud_divisor, lcr, mdr1, dll, dlm);
	
	/*
	 * NEW (Step 6): send our first characters!
	 * Expected on the wire (8N1, LSB first): 48 65 6C 6C 6F 0D 0A
	 */
	serial_write_str(serial, "Hello\r\n");


	/*
	 * NEW (Step 6): LSR right after the last write.
	 * Usually 0x00 or 0x20: the last byte(s) may still be shifting out
	 * (TEMT=0). Compare with 0x60 = completely idle.
	 */
	lsr = reg_read(serial, UART_LSR);
	dev_info(&pdev->dev, "sent \"Hello\\r\\n\", LSR now 0x%02x\n", lsr);

	return 0;

	err_pm:	
		if (!skip_pm) {
			pm_runtime_put_sync(&pdev->dev);
			pm_runtime_disable(&pdev->dev);
		}
		return ret;

}

static int serial_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Called %s\n", __func__);
	
	/*
	 * runtime PM is NOT devm-managed here, so undo it by hand,
	 * in reverse order of probe(): drop the reference (clock may be gated),
	 * then disable runtime PM.
	 */
	if (!skip_pm) {
		pm_runtime_put_sync(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
	}
 
	return 0;
}

static const struct of_device_id serial_of_match[] = {
	{ .compatible = "bootlin,serial" },
	{ },				
};
MODULE_DEVICE_TABLE(of, serial_of_match);

static struct platform_driver serial_driver = {
	.driver = {
		.name = "serial",
		.of_match_table = serial_of_match,
	},
	.probe = serial_probe,
	.remove = serial_remove,
};
module_platform_driver(serial_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("I/O memory lab review: send first characters over the UART");
 

