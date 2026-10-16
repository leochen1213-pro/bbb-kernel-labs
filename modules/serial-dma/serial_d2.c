// SPDX-License-Identifier: GPL-2.0
/*
 * DMA lab review - D2: prepare the DMA controller (on top of D1: prepare the driver)
 *  - D2: dma_request_chan("tx"), dma_map_resource(TX FIFO), dmaengine_slave_config()
 *        serial_cleanup_dma(): terminate_sync, unmap_resource, release_channel
 *        write still uses PIO (DMA transfer comes in D4)
 *  - save dev and res in serial_dev, add the tx_buf bounce buffer
 *  - serial_write -> serial_write_pio, serial_fops -> serial_fops_pio
 *  - new serial_write_dma (calls PIO for now) + serial_fops_dma (default)
 *  - serial_init_dma()/serial_cleanup_dma() skeletons, -ENODEV -> PIO
 * Based on the locking review L1. Lines marked "NEW (D1)"/"NEW (D2)" are new.
 *
 * (L1 notes) One spinlock protects:
 *   - the RX circular buffer (rx_buf, buf_rd, buf_wr), shared by the IRQ
 *     handler and read()
 *   - the UART registers (and txcount), touched by write(), ioctl() and
 *     the IRQ handler
 *
 * Based on the interrupts review I3. Lines marked "NEW (L1)" are new.
 * Also folded in from I4 experiment A: a full buffer is detected and
 * the NEW character is dropped (never wrap into an "empty" buffer).
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/serial_reg.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/spinlock.h>		/* NEW (L1): spinlock_t, spin_lock*() */
#include <linux/dmaengine.h>		/* NEW (D2): dma_request_chan(), dmaengine_*() */
#include <linux/dma-mapping.h>		/* NEW (D2): dma_map_resource(), dma_addr_t */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;

	/*
	 * NEW (L1): protects everything below it, AND every access to the
	 * UART registers after probe(). Taken from process context
	 * (read/write/ioctl) and from hard IRQ context (serial_irq).
	 */
	spinlock_t lock;

	unsigned int txcount;
	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;

	unsigned int irq_count;
	unsigned int sw_overflow;	/* NEW (L1): buffer full, new char dropped */
	unsigned int hw_overrun;	/* NEW (L1): LSR.OE seen */

	wait_queue_head_t wait;		/* has its own internal lock */

	/*
	 * NEW (D1): needed outside probe() by the DMA code
	 *   dev    : struct device for dma_request_chan() / dma_map_*()
	 *   res    : MMIO physical address of the UART (TX FIFO for DMA)
	 *   tx_buf : bounce buffer, user data is copied here before DMA
	 */
	struct device *dev;
	struct resource *res;
	char tx_buf[SERIAL_BUFSIZE];

	/*
	 * NEW (D2): dmaengine side
	 *   txchan        : TX DMA channel (NULL = no DMA, PIO is used)
	 *   fifo_dma_addr : TX FIFO address as seen by the DMA controller
	 */
	struct dma_chan *txchan;
	dma_addr_t fifo_dma_addr;
};

static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}

static void reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}

/*
 * NEW (L1): send one char, register access under the lock.
 *
 * Process context, but the IRQ handler takes the same lock
 * -> spin_lock_irqsave(): with local IRQs still enabled, the handler
 * could interrupt us while we hold the lock and spin on it forever
 * (deadlock on a single CPU).
 *
 * The lock is NOT held while waiting for THRE: holding it would keep
 * local IRQs off for up to one character time (87 us), long enough to
 * make the receiving UART overrun (I4 experiment C). So: check and
 * write atomically, otherwise release, relax, retry.
 */
static void serial_write_char(struct serial_dev *serial, char c)
{
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&serial->lock, flags);
		if (reg_read(serial, UART_LSR) & UART_LSR_THRE) {
			reg_write(serial, c, UART_TX);
			serial->txcount++;
			spin_unlock_irqrestore(&serial->lock, flags);
			return;
		}
		spin_unlock_irqrestore(&serial->lock, flags);
		cpu_relax();
	}
}

/*
 * read(): one character per call.
 * NEW (L1):
 *  - wait_event_interruptible() is called WITHOUT the lock (it sleeps).
 *    Its condition is only a hint; it is re-checked under the lock,
 *    because another reader may have taken the char in between.
 *  - put_user() is called AFTER unlocking: it may fault and sleep.
 */
static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned long flags;		/* NEW (L1) */
	char c;

	if (sz == 0)
		return 0;

retry:					/* NEW (L1) */
	if (wait_event_interruptible(serial->wait,
				     serial->buf_rd != serial->buf_wr))
		return -ERESTARTSYS;

	spin_lock_irqsave(&serial->lock, flags);		/* NEW (L1) */

	/* NEW (L1): check-then-act must be atomic: re-check under the lock */
	if (serial->buf_rd == serial->buf_wr) {
		spin_unlock_irqrestore(&serial->lock, flags);
		goto retry;
	}

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

	spin_unlock_irqrestore(&serial->lock, flags);		/* NEW (L1) */

	if (put_user(c, buf))		/* outside the lock: may sleep */
		return -EFAULT;

	return 1;
}

/*
 * write(): two writers at the same time are allowed. Each character is
 * sent atomically (serial_write_char), so the UART never gets confused,
 * but the two strings may be interleaved on the wire.
 * get_user() stays outside the lock (may sleep).
 */
static ssize_t serial_write_pio(struct file *file, const char __user *buf,
				size_t sz, loff_t *ppos)	/* NEW (D1): renamed from serial_write */
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	size_t i;

	for (i = 0; i < sz; i++) {
		char c;

		if (get_user(c, buf + i))
			return -EFAULT;

		serial_write_char(serial, c);
		if (c == '\n')
			serial_write_char(serial, '\r');
	}

	return sz;
}

/*
 * NEW (L1): txcount is also written by serial_write_char() under the lock.
 * GET: take a snapshot under the lock, copy to user space after unlocking.
 */
static long serial_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned int __user *argp = (unsigned int __user *)arg;
	unsigned long flags;		/* NEW (L1) */
	unsigned int count;		/* NEW (L1) */

	switch (cmd) {
	case SERIAL_RESET_COUNTER:
		spin_lock_irqsave(&serial->lock, flags);
		serial->txcount = 0;
		spin_unlock_irqrestore(&serial->lock, flags);
		break;
	case SERIAL_GET_COUNTER:
		spin_lock_irqsave(&serial->lock, flags);
		count = serial->txcount;
		spin_unlock_irqrestore(&serial->lock, flags);
		if (put_user(count, argp))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

/*
 * NEW (D1): DMA write path, placeholder for now.
 * D4 will replace the body with the real dmaengine transfer. In D1 it
 * simply calls the PIO path, so behaviour is unchanged and we can check
 * that the fops switching and the init/cleanup plumbing work.
 */
static ssize_t serial_write_dma(struct file *file, const char __user *buf,
				size_t sz, loff_t *ppos)
{
	return serial_write_pio(file, buf, sz, ppos);
}

/*
 * NEW (D1): two sets of file operations, identical except .write.
 * probe() picks one per device: DMA if serial_init_dma() succeeded,
 * PIO if no DMA channel is available (-ENODEV).
 */
static const struct file_operations serial_fops_pio = {	/* NEW (D1): renamed from serial_fops */
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write_pio,
	.unlocked_ioctl = serial_ioctl,
};

static const struct file_operations serial_fops_dma = {	/* NEW (D1) */
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write_dma,
	.unlocked_ioctl = serial_ioctl,
};

/*
 * NEW (D1): DMA setup / teardown helpers (empty for now, filled in D2).
 *
 * serial_init_dma(): called right before misc_register().
 *   0       -> DMA usable, use serial_fops_dma
 *   -ENODEV -> no DMA channel for this UART, fall back to serial_fops_pio
 *   other   -> real error, probe() fails
 * serial_cleanup_dma(): called from the probe() error path and remove();
 *   must be safe to call even when init failed or DMA is not used.
 */
static int serial_init_dma(struct serial_dev *serial)
{
	struct dma_slave_config txconf = {};
	int ret;

	/*
	 * NEW (D2) (1): get the TX channel.
	 * The name "tx" is looked up in the DT "dma-names" property of this
	 * UART node; the matching "dmas" entry gives the DMA controller
	 * (EDMA) and the request line number. Returns an ERR_PTR, never NULL:
	 *   -ENODEV       no "dmas" in DT (UART4)          -> PIO fallback
	 *   -EPROBE_DEFER DMA controller driver not ready  -> probe retried later
	 */
	serial->txchan = dma_request_chan(serial->dev, "tx");
	if (IS_ERR(serial->txchan)) {
		ret = PTR_ERR(serial->txchan);
		serial->txchan = NULL;		/* cleanup must see "no channel" */
		if (ret == -ENODEV)
			dev_info(serial->dev, "no TX DMA channel: %pe\n", ERR_PTR(ret));
		else
			dev_err(serial->dev, "cannot get TX DMA channel: %pe\n", ERR_PTR(ret));
		return ret;
	}

	/*
	 * NEW (D2) (2): DMA address of the TX FIFO (THR register).
	 * The DMA controller needs the PHYSICAL (bus) address of the
	 * register, not serial->regs (CPU virtual). dma_map_resource() is the
	 * generic way to get a dma_addr_t for MMIO (works with an IOMMU too).
	 * Size 4: the register is accessed as one 32-bit word.
	 */
	serial->fifo_dma_addr = dma_map_resource(serial->dev,
						 serial->res->start + UART_TX * 4,
						 4, DMA_TO_DEVICE, 0);
	if (dma_mapping_error(serial->dev, serial->fifo_dma_addr)) {
		dev_err(serial->dev, "cannot map the TX FIFO for DMA\n");
		ret = -ENOMEM;
		goto err_release;
	}

	/*
	 * NEW (D2) (3): tell the DMA controller what the transfers look like.
	 * Only the DEVICE side is fixed here (destination = TX FIFO, 1 byte
	 * per request). The memory side (source address, length) changes
	 * with every write() and is given later, in dmaengine_prep_*().
	 */
	txconf.direction = DMA_MEM_TO_DEV;
	txconf.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	txconf.dst_addr = serial->fifo_dma_addr;
	ret = dmaengine_slave_config(serial->txchan, &txconf);
	if (ret) {
		dev_err(serial->dev, "cannot configure the TX DMA channel: %pe\n", ERR_PTR(ret));
		goto err_unmap;
	}

	dev_info(serial->dev, "TX DMA channel %s, TX FIFO phys %pa -> dma %pad\n",
		 dma_chan_name(serial->txchan), &serial->res->start,
		 &serial->fifo_dma_addr);

	return 0;

	/* NEW (D2): undo in reverse order */
err_unmap:
	dma_unmap_resource(serial->dev, serial->fifo_dma_addr, 4, DMA_TO_DEVICE, 0);
err_release:
	dma_release_channel(serial->txchan);
	serial->txchan = NULL;
	return ret;
}

/*
 * NEW (D2): reverse of serial_init_dma(). Safe when DMA is not used
 * (txchan == NULL: UART4, or init failed).
 */
static void serial_cleanup_dma(struct serial_dev *serial)
{
	if (!serial->txchan)
		return;

	dmaengine_terminate_sync(serial->txchan);	/* stop + wait for callbacks, may sleep */
	dma_unmap_resource(serial->dev, serial->fifo_dma_addr, 4, DMA_TO_DEVICE, 0);
	dma_release_channel(serial->txchan);
	serial->txchan = NULL;
}

/*
 * IRQ handler.
 * NEW (L1): hard IRQ context, local IRQs are already disabled
 * -> plain spin_lock() is enough (irqsave would be harmless but useless).
 * wake_up() after unlocking: the reader does not spin on our lock
 * right after being woken.
 */
static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	unsigned int next;
	u32 lsr;
	char c;

	spin_lock(&serial->lock);		/* NEW (L1) */

	serial->irq_count++;

	/* Drain everything received (with FIFOs off: at most one char) */
	while ((lsr = reg_read(serial, UART_LSR)) & UART_LSR_DR) {
		if (lsr & UART_LSR_OE)
			serial->hw_overrun++;

		c = reg_read(serial, UART_RX);		/* ack */

		/* NEW (L1), from I4-A: never wrap into an "empty" buffer */
		next = serial->buf_wr + 1;
		if (next >= SERIAL_BUFSIZE)
			next = 0;
		if (next == serial->buf_rd) {
			serial->sw_overflow++;
			continue;			/* drop the new char */
		}

		serial->rx_buf[serial->buf_wr] = c;
		serial->buf_wr = next;
	}

	spin_unlock(&serial->lock);		/* NEW (L1) */

	wake_up(&serial->wait);

	return IRQ_HANDLED;
}

static int serial_probe(struct platform_device *pdev)
{
	struct serial_dev *serial;
	struct resource *res;
	unsigned int uartclk, baud_divisor;
	int irq;
	int ret;

	serial = devm_kzalloc(&pdev->dev, sizeof(*serial), GFP_KERNEL);
	if (!serial)
		return -ENOMEM;

	serial->dev = &pdev->dev;		/* NEW (D1) */

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	/* Everything the handler uses must be ready before request_irq() */
	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);		/* NEW (L1) */

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) {
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency", &uartclk);
	if (ret) {
		dev_err(&pdev->dev, "clock-frequency property not found in Device Tree\n");
		goto err_pm;
	}

	/*
	 * Register setup without the lock: the IRQ is not requested yet and
	 * the device file does not exist yet, nobody else can touch the UART.
	 */
	baud_divisor = uartclk / 16 / SERIAL_BAUD;
	reg_write(serial, 0x07, UART_OMAP_MDR1);
	reg_write(serial, 0x00, UART_LCR);
	reg_write(serial, UART_LCR_DLAB, UART_LCR);
	reg_write(serial, baud_divisor & 0xff, UART_DLL);
	reg_write(serial, (baud_divisor >> 8) & 0xff, UART_DLM);
	reg_write(serial, UART_LCR_WLEN8, UART_LCR);
	reg_write(serial, 0x00, UART_OMAP_MDR1);

	reg_write(serial, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT, UART_FCR);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pm;
	}

	ret = devm_request_irq(&pdev->dev, irq, serial_irq, 0,
			       dev_name(&pdev->dev), serial);
	if (ret) {
		dev_err(&pdev->dev, "cannot request IRQ %d: %d\n", irq, ret);
		goto err_pm;
	}

	reg_write(serial, UART_IER_RDI, UART_IER);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_irq;
	}
	serial->res = res;			/* NEW (D1) */

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;
	}
	serial->miscdev.parent = &pdev->dev;

	/*
	 * NEW (D1): set up DMA right before registering the device file,
	 * then pick the file operations accordingly.
	 */
	ret = serial_init_dma(serial);
	if (ret == -ENODEV) {
		dev_info(&pdev->dev, "no DMA channel, falling back to PIO\n");
		serial->miscdev.fops = &serial_fops_pio;
	} else if (ret) {
		dev_err(&pdev->dev, "DMA init failed: %pe\n", ERR_PTR(ret));
		goto err_irq;
	} else {
		serial->miscdev.fops = &serial_fops_dma;
	}

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_dma;			/* NEW (D1) */
	}

	dev_info(&pdev->dev, "registered /dev/%s, irq %d, %s write\n",
		 serial->miscdev.name, irq,
		 serial->miscdev.fops == &serial_fops_dma ? "DMA" : "PIO");	/* NEW (D1) */

	return 0;

err_dma:				/* NEW (D1) */
	serial_cleanup_dma(serial);
err_irq:
	reg_write(serial, 0, UART_IER);
err_pm:
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int serial_remove(struct platform_device *pdev)
{
	struct serial_dev *serial = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "irq_count=%u sw_overflow=%u hw_overrun=%u\n",
		 serial->irq_count, serial->sw_overflow, serial->hw_overrun);

	misc_deregister(&serial->miscdev);

	serial_cleanup_dma(serial);		/* NEW (D1): after the device file is gone */

	reg_write(serial, 0, UART_IER);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

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
MODULE_DESCRIPTION("DMA lab review D2: request and configure the TX DMA channel");
