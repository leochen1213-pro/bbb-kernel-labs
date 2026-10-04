// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define MYCHAR_NDEV	2
#define MYCHAR_BUFSZ	128

struct mychar_dev {
	char buf[MYCHAR_BUFSZ];
	size_t len;
};

static dev_t devt;
static struct cdev my_cdev;
static struct class *my_class;
static struct mychar_dev devs[MYCHAR_NDEV];

static int my_open(struct inode *inode, struct file *file)
{
	unsigned int idx = iminor(inode) - MINOR(devt);

	file->private_data = &devs[idx];
	return 0;
}

static ssize_t my_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	struct mychar_dev *d = file->private_data;

	return simple_read_from_buffer(buf, count, ppos, d->buf, d->len);
}

static ssize_t my_write(struct file *file, const char __user *ubuf,
			size_t count, loff_t *ppos)
{
	struct mychar_dev *d = file->private_data;
	size_t n = min(count, sizeof(d->buf));

	if (copy_from_user(d->buf, ubuf, n))
		return -EFAULT;

	d->len = n;
	return n;
}

static const struct file_operations my_fops = {
	.owner = THIS_MODULE,
	.open  = my_open,
	.read  = my_read,
	.write = my_write,
};

static int __init my_init(void)
{
	struct device *dev;
	int ret, i;

	ret = alloc_chrdev_region(&devt, 0, MYCHAR_NDEV, "mychar");
	if (ret)
		return ret;

	cdev_init(&my_cdev, &my_fops);
	ret = cdev_add(&my_cdev, devt, MYCHAR_NDEV);
	if (ret)
		goto err_region;

	my_class = class_create("mychar");
	if (IS_ERR(my_class)) {
		ret = PTR_ERR(my_class);
		goto err_cdev;
	}

	for (i = 0; i < MYCHAR_NDEV; i++) {
		dev = device_create(my_class, NULL, MKDEV(MAJOR(devt), MINOR(devt) + i),
				    NULL, "mychar%d", i);
		if (IS_ERR(dev)) {
			ret = PTR_ERR(dev);
			goto err_devices;
		}
	}

	pr_info("mychar: major %d, %d minors\n", MAJOR(devt), MYCHAR_NDEV);
	return 0;

err_devices:
	while (--i >= 0)
		device_destroy(my_class, MKDEV(MAJOR(devt), MINOR(devt) + i));
	class_destroy(my_class);
err_cdev:
	cdev_del(&my_cdev);
err_region:
	unregister_chrdev_region(devt, MYCHAR_NDEV);
	return ret;
}

static void __exit my_exit(void)
{
	int i;

	for (i = 0; i < MYCHAR_NDEV; i++)
		device_destroy(my_class, MKDEV(MAJOR(devt), MINOR(devt) + i));
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(devt, MYCHAR_NDEV);
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
