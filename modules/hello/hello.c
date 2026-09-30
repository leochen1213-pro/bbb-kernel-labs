// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

static int __init hello_init(void)
{
    pr_alert("Hello from the BeagleBone Black!\n");
    return 0;
}

static void __exit hello_exit(void)
{
    pr_alert("Goodbye, cruel world!\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("First test module");
MODULE_AUTHOR("Leo");
