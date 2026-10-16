# Misc driver review (Output-only serial port driver)

## M1 - misc_register with stub fops
- struct miscdevice EMBEDDED in struct serial_dev (needed by container_of)
- MISC_DYNAMIC_MINOR: minors allocated high -> low (124, 123), major 10
- name "serial-%x" from res->start -> /dev/serial-48024000, /dev/serial-481a8000
- .parent = &pdev->dev -> /sys/class/misc/serial-xxx/device -> 48024000.serial
- misc_register LAST in probe (device file usable as soon as it returns);
  misc_deregister FIRST in remove
- stub -EINVAL -> "write error: Invalid argument"

## M2 - write()
- misc_open() sets file->private_data = struct miscdevice *
  -> container_of(miscdev, struct serial_dev, miscdev)
- get_user() for every byte, never dereference __user directly
  - gcc compiles buf[i]; sparse (make C=1) warns "dereference of noderef expression"
  - PAN (CPU_SW_DOMAIN_PAN): direct access faults; without PAN: security hole
    (no access_ok, no fault handling)
- '\n' -> "\n\r" (like tty ONLCR)
- return sz; return 0 => echo retries forever (0 = "nothing written")
- Analyzer on P9_21: echo test -> 74 65 73 74 0A 0D

## M3 - module refcount (.owner)
- No .owner: try_module_get(NULL) succeeds without counting
  - refcnt 0 with file open, rmmod OK, /dev nodes gone
  - kill -> close -> filp_close -> filp_flush reads file->f_op->flush
    -> Oops "paging request at bf004194" = &serial_fops + 0x38
  - Clues: bf0xxxxx address (module area), pte=0,
    "Modules linked in: [last unloaded: serial(O)]"
  - Close path: filp_close -> filp_flush (->flush) -> fput -> __fput (->release)
  - Oops in do_exit -> "Fixing recursive fault but reboot is needed!"
- With .owner = THIS_MODULE: refcnt 1, rmmod -> -EAGAIN (busybox O_NONBLOCK)
- .owner protects MODULE CODE lifetime, NOT device lifetime:
  sysfs unbind still runs remove() + devm frees struct serial_dev
  while the file is open -> use-after-free (needs kref / other handling)
- After any Oops: reboot (kernel tainted D)

## M4 - ioctl
- long (*unlocked_ioctl)(struct file *, unsigned int cmd, unsigned long arg)
  "unlocked" = no Big Kernel Lock (historical)
- arg carries a user address -> (unsigned int __user *)arg + put_user()
- unknown cmd -> -ENOTTY ("Inappropriate ioctl for device");
  -EINVAL = known cmd, bad argument
- Counter per device (in struct serial_dev): UART4 stays 0
- Results: 0 -> echo test 6 (t e s t \n \r) -> echo -n abc 9 -> reset 0
- Real drivers: _IO/_IOR/_IOW/_IOWR(magic, nr, type) in a uapi header
  - cmd = dir | size | type | nr; R/W from USER point of view
  - avoids collisions, VFS handles FIOCLEX/FIONBIO/... before the driver
  - registry: Documentation/userspace-api/ioctl/ioctl-number.rst
- txcount++ = ldr/add/str -> lost update vs RESET on SMP/PREEMPT
  (UP + non-preemptible BBB hides it); fix: spinlock or atomic_t
