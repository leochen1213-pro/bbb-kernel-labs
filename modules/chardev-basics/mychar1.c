// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>

static dev_t devt;
static struct cdev my_cdev;
static struct class *my_class;

static const char msg[] = "Hello from a raw char device\n";

static ssize_t my_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	size_t len = sizeof(msg) - 1;

	/* 已經讀到結尾,回傳 0 代表 EOF */
	if (*ppos >= len)
		return 0;

	/* 不要超過剩下的資料長度 */
	if (count > len - *ppos)
		count = len - *ppos;

	/* 把資料從 kernel 複製到 user space */
	if (copy_to_user(buf, msg + *ppos, count))
		return -EFAULT;

	/* 更新讀取位置,下次從這裡繼續 */
	*ppos += count;
	return count;

	//return simple_read_from_buffer(buf, count, ppos, msg, sizeof(msg) - 1);
}

static const struct file_operations my_fops = {
	.owner = THIS_MODULE,
	.read  = my_read,
};

static int __init my_init(void)
{
	struct device *dev;
	int ret;

	ret = alloc_chrdev_region(&devt, 0, 1, "mychar");
	if (ret)
		return ret;

	cdev_init(&my_cdev, &my_fops);
	ret = cdev_add(&my_cdev, devt, 1);
	if (ret)
		goto err_region;

	my_class = class_create("mychar");
	if (IS_ERR(my_class)) {
		ret = PTR_ERR(my_class);
		goto err_cdev;
	}

	dev = device_create(my_class, NULL, devt, NULL, "mychar0");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	pr_info("mychar: major %d, minor %d\n", MAJOR(devt), MINOR(devt));
	return 0;

err_class:
	class_destroy(my_class);
err_cdev:
	cdev_del(&my_cdev);
err_region:
	unregister_chrdev_region(devt, 1);
	return ret;
}

static void __exit my_exit(void)
{
	device_destroy(my_class, devt);
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(devt, 1);
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
