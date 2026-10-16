// SPDX-License-Identifier: GPL-2.0
/*
 * DMA lab review - D4: process user write requests with DMA
 * Lines marked "NEW (D4)" are new in this step.
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
#include <linux/spinlock.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>		/* NEW (D4): struct completion */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

#define OMAP_UART_SCR_DMAMODE_CTL3	0x7
#define OMAP_UART_SCR_TX_TRIG_GRANU1	BIT(6)

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;

	spinlock_t lock;

	unsigned int txcount;
	char rx_buf[SERIAL_BUFSIZE];
	unsigned int buf_rd;
	unsigned int buf_wr;

	unsigned int irq_count;
	unsigned int sw_overflow;
	unsigned int hw_overrun;

	wait_queue_head_t wait;

	struct device *dev;
	struct resource *res;
	char tx_buf[SERIAL_BUFSIZE];

	struct dma_chan *txchan;
	dma_addr_t fifo_dma_addr;

	/*
	 * NEW (D4):
	 *   txongoing  : one DMA write at a time (tx_buf and txchan are
	 *                shared); protected by lock, but the lock is NOT
	 *                held during the transfer (we sleep while waiting)
	 *   txcomplete : signalled by the DMA completion callback
	 */
	bool txongoing;
	struct completion txcomplete;
};

static u32 reg_read(struct serial_dev *serial, unsigned int reg)
{
	return readl(serial->regs + reg * 4);
}

static void reg_write(struct serial_dev *serial, u32 val, unsigned int reg)
{
	writel(val, serial->regs + reg * 4);
}

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

static ssize_t serial_read(struct file *file, char __user *buf,
			   size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned long flags;
	char c;

	if (sz == 0)
		return 0;

retry:
	if (wait_event_interruptible(serial->wait,
				     serial->buf_rd != serial->buf_wr))
		return -ERESTARTSYS;

	spin_lock_irqsave(&serial->lock, flags);

	if (serial->buf_rd == serial->buf_wr) {
		spin_unlock_irqrestore(&serial->lock, flags);
		goto retry;
	}

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

	spin_unlock_irqrestore(&serial->lock, flags);

	if (put_user(c, buf))
		return -EFAULT;

	return 1;
}

static ssize_t serial_write_pio(struct file *file, const char __user *buf,
				size_t sz, loff_t *ppos)
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

static long serial_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	unsigned int __user *argp = (unsigned int __user *)arg;
	unsigned long flags;
	unsigned int count;

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
 * NEW (D4): wait until the UART has sent everything.
 * DMA completion only means the last byte was written into THR, not
 * that it left the wire. Same lock discipline as serial_write_char().
 */
static void serial_wait_tx_empty(struct serial_dev *serial)
{
	unsigned long flags;
	u32 lsr;

	for (;;) {
		spin_lock_irqsave(&serial->lock, flags);
		lsr = reg_read(serial, UART_LSR);
		spin_unlock_irqrestore(&serial->lock, flags);
		if (lsr & UART_LSR_THRE)
			return;
		cpu_relax();
	}
}

/*
 * NEW (D4): DMA completion callback.
 * Called by the dmaengine driver (EDMA) from its completion path, in
 * atomic context (tasklet): only wake up the writer, never sleep here.
 */
static void serial_dma_tx_callback(void *param)
{
	struct serial_dev *serial = param;

	complete(&serial->txcomplete);
}

/*
 * NEW (D4): write() with DMA.
 *  1. txongoing flag: refuse a second concurrent writer with -EBUSY
 *  2. copy_from_user() into the tx_buf bounce buffer (max 16 bytes)
 *  3. OMAP UART quirk: the first byte must be written by the CPU
 *  4. dma_map_single(): CPU cache cleaned, buffer now owned by the device
 *  5. prep (tx_buf + 1, len - 1) -> callback -> submit -> issue_pending
 *  6. sleep in wait_for_completion until the DMA callback
 *  7. dma_unmap_single(): buffer back to the CPU
 *  8. wait for THRE, account txcount, clear txongoing
 * Returns the number of bytes consumed (<= 16): a short write, user
 * space calls write() again for the rest. No '\n' -> "\n\r" conversion:
 * the DMA copies the bytes as they are.
 */
static ssize_t serial_write_dma(struct file *file, const char __user *buf,
				size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	dma_addr_t dma_addr;
	unsigned long flags;
	size_t len;
	char first;
	ssize_t ret;

	if (sz == 0)
		return 0;

	/* NEW (D4) 1: one DMA transfer at a time */
	spin_lock_irqsave(&serial->lock, flags);
	if (serial->txongoing) {
		spin_unlock_irqrestore(&serial->lock, flags);
		return -EBUSY;
	}
	serial->txongoing = true;
	spin_unlock_irqrestore(&serial->lock, flags);

	/* NEW (D4) 2: user buffer -> bounce buffer (may sleep: no lock held) */
	len = min_t(size_t, sz, SERIAL_BUFSIZE);
	if (copy_from_user(serial->tx_buf, buf, len)) {
		ret = -EFAULT;
		goto out;
	}

	/* NEW (D4) 3: OMAP 8250 UART quirk, the first byte is sent by the CPU */
	first = serial->tx_buf[0];

	/* A single byte: nothing left for the DMA, plain PIO */
	if (len == 1) {
		serial_write_char(serial, first);
		ret = 1;
		goto out;
	}

	/* NEW (D4) 4: streaming mapping, CPU -> device (cache clean on ARMv7) */
	dma_addr = dma_map_single(serial->dev, serial->tx_buf, len, DMA_TO_DEVICE);
	if (dma_mapping_error(serial->dev, dma_addr)) {
		ret = -ENOMEM;
		goto out;
	}

	/* NEW (D4) 5: describe the memory side: skip the first byte */
	desc = dmaengine_prep_slave_single(serial->txchan, dma_addr + 1, len - 1,
					   DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
	if (!desc) {
		ret = -EIO;
		goto unmap;
	}

	reinit_completion(&serial->txcomplete);
	desc->callback = serial_dma_tx_callback;
	desc->callback_param = serial;

	cookie = dmaengine_submit(desc);	/* queued, not started yet */
	ret = dma_submit_error(cookie);
	if (ret)
		goto unmap;

	dma_async_issue_pending(serial->txchan);	/* DMA controller side: start */
	serial_write_char(serial, first);		/* UART side: kick the TX DMA requests */

	/*
	 * NEW (D4) 6: sleep until the callback. Timeout instead of waiting
	 * forever: if the UART never requests data, stop the channel cleanly
	 * before unmapping the buffer it may still use.
	 */
	if (!wait_for_completion_timeout(&serial->txcomplete, msecs_to_jiffies(1000))) {
		dev_err(serial->dev, "TX DMA timeout\n");
		dmaengine_terminate_sync(serial->txchan);
		ret = -ETIMEDOUT;
		goto unmap;
	}

	/* NEW (D4) 7: buffer back to the CPU */
	dma_unmap_single(serial->dev, dma_addr, len, DMA_TO_DEVICE);

	/* NEW (D4) 8: last byte really sent, then account the DMA bytes */
	serial_wait_tx_empty(serial);

	spin_lock_irqsave(&serial->lock, flags);
	serial->txcount += len - 1;	/* the first byte was counted by serial_write_char() */
	spin_unlock_irqrestore(&serial->lock, flags);

	ret = len;
	goto out;

unmap:
	dma_unmap_single(serial->dev, dma_addr, len, DMA_TO_DEVICE);
out:
	spin_lock_irqsave(&serial->lock, flags);
	serial->txongoing = false;
	spin_unlock_irqrestore(&serial->lock, flags);
	return ret;
}

static const struct file_operations serial_fops_pio = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write_pio,
	.unlocked_ioctl = serial_ioctl,
};

static const struct file_operations serial_fops_dma = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write_dma,
	.unlocked_ioctl = serial_ioctl,
};

static int serial_init_dma(struct serial_dev *serial)
{
	struct dma_slave_config txconf = {};
	int ret;

	serial->txchan = dma_request_chan(serial->dev, "tx");
	if (IS_ERR(serial->txchan)) {
		ret = PTR_ERR(serial->txchan);
		serial->txchan = NULL;
		if (ret == -ENODEV)
			dev_info(serial->dev, "no TX DMA channel: %pe\n", ERR_PTR(ret));
		else
			dev_err(serial->dev, "cannot get TX DMA channel: %pe\n", ERR_PTR(ret));
		return ret;
	}

	serial->fifo_dma_addr = dma_map_resource(serial->dev,
						 serial->res->start + UART_TX * 4,
						 4, DMA_TO_DEVICE, 0);
	if (dma_mapping_error(serial->dev, serial->fifo_dma_addr)) {
		dev_err(serial->dev, "cannot map the TX FIFO for DMA\n");
		ret = -ENOMEM;
		goto err_release;
	}

	txconf.direction = DMA_MEM_TO_DEV;
	txconf.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	txconf.dst_addr = serial->fifo_dma_addr;
	ret = dmaengine_slave_config(serial->txchan, &txconf);
	if (ret) {
		dev_err(serial->dev, "cannot configure the TX DMA channel: %pe\n", ERR_PTR(ret));
		goto err_unmap;
	}

	reg_write(serial, OMAP_UART_SCR_DMAMODE_CTL3 | OMAP_UART_SCR_TX_TRIG_GRANU1,
		  UART_OMAP_SCR);

	dev_info(serial->dev, "TX DMA channel %s, TX FIFO phys %pa -> dma %pad, SCR 0x%02x\n",
		 dma_chan_name(serial->txchan), &serial->res->start,
		 &serial->fifo_dma_addr, reg_read(serial, UART_OMAP_SCR));

	return 0;

err_unmap:
	dma_unmap_resource(serial->dev, serial->fifo_dma_addr, 4, DMA_TO_DEVICE, 0);
err_release:
	dma_release_channel(serial->txchan);
	serial->txchan = NULL;
	return ret;
}

static void serial_cleanup_dma(struct serial_dev *serial)
{
	if (!serial->txchan)
		return;

	dmaengine_terminate_sync(serial->txchan);
	reg_write(serial, 0, UART_OMAP_SCR);
	dma_unmap_resource(serial->dev, serial->fifo_dma_addr, 4, DMA_TO_DEVICE, 0);
	dma_release_channel(serial->txchan);
	serial->txchan = NULL;
}

static irqreturn_t serial_irq(int irq, void *dev_id)
{
	struct serial_dev *serial = dev_id;
	unsigned int next;
	u32 lsr;
	char c;

	spin_lock(&serial->lock);

	serial->irq_count++;

	while ((lsr = reg_read(serial, UART_LSR)) & UART_LSR_DR) {
		if (lsr & UART_LSR_OE)
			serial->hw_overrun++;

		c = reg_read(serial, UART_RX);

		next = serial->buf_wr + 1;
		if (next >= SERIAL_BUFSIZE)
			next = 0;
		if (next == serial->buf_rd) {
			serial->sw_overflow++;
			continue;
		}

		serial->rx_buf[serial->buf_wr] = c;
		serial->buf_wr = next;
	}

	spin_unlock(&serial->lock);

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

	serial->dev = &pdev->dev;

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);
	init_completion(&serial->txcomplete);	/* NEW (D4): before the device file exists */

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
	serial->res = res;

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;
	}
	serial->miscdev.parent = &pdev->dev;

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
		goto err_dma;
	}

	dev_info(&pdev->dev, "registered /dev/%s, irq %d, %s write\n",
		 serial->miscdev.name, irq,
		 serial->miscdev.fops == &serial_fops_dma ? "DMA" : "PIO");

	return 0;

err_dma:
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

	serial_cleanup_dma(serial);

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
MODULE_DESCRIPTION("DMA lab review D4: write() through the EDMA TX channel");
