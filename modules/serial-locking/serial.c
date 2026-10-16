// SPDX-License-Identifier: GPL-2.0
/*
 * Locking lab review - L3 (= L2 + mutex for whole write() calls)
 *   use_mutex=1  : serialize write() calls with a mutex (process context)
 *   pace_us=N    : sleep N us after each char in write()
 *   bug=3        : mutex_lock() in the IRQ handler (WRONG, demo only)
 * Lines marked "NEW (L3)" are new in this step, "NEW (L2)" from L2.
 *
 * One spinlock protects:
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
#include <linux/mutex.h>		/* NEW (L3): struct mutex */
#include <linux/delay.h>		/* NEW (L3): usleep_range() */

#define SERIAL_BAUD	115200
#define SERIAL_BUFSIZE	16

#define SERIAL_RESET_COUNTER	0
#define SERIAL_GET_COUNTER	1

/*
 * NEW (L2): deliberate locking bugs, NEVER use bug != 0 outside this lab.
 *   0 = correct (= L1)
 *   1 = put_user() called while holding the spinlock (read)
 *   2 = read() takes the lock with spin_lock(), IRQs still enabled
 *   3 = NEW (L3): mutex_lock() in the hard IRQ handler
 * 0444: must not change while the module runs (lock/unlock must match).
 */
static int bug;
module_param(bug, int, 0444);
MODULE_PARM_DESC(bug, "0=correct, 1=put_user under spinlock, 2=spin_lock without irqsave in read(), 3=mutex in IRQ handler");

/* NEW (L3) */
static bool use_mutex;
module_param(use_mutex, bool, 0444);
MODULE_PARM_DESC(use_mutex, "1 = serialize whole write() calls with a mutex");

static unsigned int pace_us;
module_param(pace_us, uint, 0444);
MODULE_PARM_DESC(pace_us, "sleep N us after each char in write() (0 = busy writer, as before)");

struct serial_dev {
	void __iomem *regs;
	struct miscdevice miscdev;

	/*
	 * NEW (L3): serializes whole write() calls (use_mutex=1).
	 * Process context ONLY -> may sleep while holding it.
	 */
	struct mutex write_lock;

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
 * NEW (L2): lock helpers for read(), so that bug=2 changes only how
 * read() takes the lock.
 * bug=2 (WRONG): plain spin_lock() in process context, local IRQs stay
 * enabled. The IRQ handler takes the same lock -> if the UART IRQ fires
 * while read() holds it: deadlock on this CPU. lockdep reports the
 * pattern the first time both usages are seen, even without a collision.
 */
static void rd_lock(struct serial_dev *serial, unsigned long *flags)
{
	if (bug == 2)
		spin_lock(&serial->lock);
	else
		spin_lock_irqsave(&serial->lock, *flags);
}

static void rd_unlock(struct serial_dev *serial, unsigned long *flags)
{
	if (bug == 2)
		spin_unlock(&serial->lock);
	else
		spin_unlock_irqrestore(&serial->lock, *flags);
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

	rd_lock(serial, &flags);			/* NEW (L2): was spin_lock_irqsave() */

	/* NEW (L1): check-then-act must be atomic: re-check under the lock */
	if (serial->buf_rd == serial->buf_wr) {
		rd_unlock(serial, &flags);		/* NEW (L2) */
		goto retry;
	}

	c = serial->rx_buf[serial->buf_rd];
	serial->buf_rd++;
	if (serial->buf_rd >= SERIAL_BUFSIZE)
		serial->buf_rd = 0;

	/*
	 * NEW (L2) bug=1 (WRONG): copy to user space while holding the
	 * spinlock with local IRQs off. put_user() may fault and sleep.
	 * CONFIG_DEBUG_ATOMIC_SLEEP catches it through might_fault(), even
	 * when no fault actually happens.
	 */
	if (bug == 1) {
		int ret = put_user(c, buf);

		rd_unlock(serial, &flags);
		return ret ? -EFAULT : 1;
	}

	rd_unlock(serial, &flags);			/* NEW (L2) */

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
static ssize_t serial_write(struct file *file, const char __user *buf,
			    size_t sz, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct serial_dev *serial = container_of(miscdev, struct serial_dev, miscdev);
	ssize_t ret = sz;		/* NEW (L3) */
	size_t i;

	/*
	 * NEW (L3): optional mutex around the WHOLE write().
	 * Process context only (never taken by the IRQ handler, except
	 * bug=3), so a sleeping lock is fine and preferable:
	 * a second writer SLEEPS here instead of spinning.
	 * _interruptible: Ctrl-C while waiting returns -ERESTARTSYS.
	 * Lock order: write_lock (mutex) -> lock (spinlock), never reversed.
	 */
	if (use_mutex && mutex_lock_interruptible(&serial->write_lock))
		return -ERESTARTSYS;

	for (i = 0; i < sz; i++) {
		char c;

		if (get_user(c, buf + i)) {	/* may sleep: OK under a mutex */
			ret = -EFAULT;
			break;			/* NEW (L3): unlock before returning */
		}

		serial_write_char(serial, c);	/* takes the spinlock briefly */
		if (c == '\n')
			serial_write_char(serial, '\r');

		/*
		 * NEW (L3): sleep between characters (slow device / writer
		 * waiting for a TX interrupt). Legal here: no spinlock held,
		 * and sleeping while holding a MUTEX is allowed.
		 * While we sleep, the other writer and the reader can run.
		 */
		if (pace_us)
			usleep_range(pace_us, pace_us + 50);
	}

	if (use_mutex)
		mutex_unlock(&serial->write_lock);

	return ret;
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

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.read = serial_read,
	.write = serial_write,
	.unlocked_ioctl = serial_ioctl,
};

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

	/*
	 * NEW (L3) bug=3 (WRONG): take a mutex in hard IRQ context.
	 * mutex_lock() may sleep; here there is no task of our own to put
	 * to sleep (we borrowed the interrupted one) and IRQs are off.
	 * Uncontended in this test (nobody else uses write_lock when
	 * use_mutex=0), so it "works" - but the debug checks fire.
	 */
	if (bug == 3) {
		mutex_lock(&serial->write_lock);
		mutex_unlock(&serial->write_lock);
	}

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

	serial->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serial->regs))
		return PTR_ERR(serial->regs);

	platform_set_drvdata(pdev, serial);

	/* Everything the handler uses must be ready before request_irq() */
	init_waitqueue_head(&serial->wait);
	spin_lock_init(&serial->lock);		/* NEW (L1) */
	mutex_init(&serial->write_lock);	/* NEW (L3): before request_irq (bug=3) and misc_register */

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

	serial->miscdev.minor = MISC_DYNAMIC_MINOR;
	serial->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "serial-%x",
					      (unsigned int)res->start);
	if (!serial->miscdev.name) {
		ret = -ENOMEM;
		goto err_irq;
	}
	serial->miscdev.fops = &serial_fops;
	serial->miscdev.parent = &pdev->dev;

	ret = misc_register(&serial->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed: %d\n", ret);
		goto err_irq;
	}

	dev_info(&pdev->dev, "registered /dev/%s, irq %d (locking L3, bug=%d use_mutex=%d pace_us=%u)\n",
		 serial->miscdev.name, irq, bug, use_mutex, pace_us);	/* NEW (L3) */

	return 0;

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
MODULE_DESCRIPTION("Locking lab review L3: mutex + spinlock");
