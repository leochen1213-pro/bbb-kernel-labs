# I/O memory lab review

## Step 1: DT verification (running kernel)
- Board DTS only overrides &uart2/&uart4 (compatible, status, pinctrl-*); reg/interrupts/clock-frequency/dmas inherited from SoC .dtsi
- Runtime DT: /sys/firmware/devicetree/base (/proc/device-tree is a symlink; find does not follow it)
- BusyBox grep has no --include: use find -name compatible -exec grep -l ... {} \;
- Address translation via ranges:
  serial@0 reg <0x0 0x1000> -> target-module@24000 <0x0 0x24000 0x1000>
  -> segment@0 <0x24000 0x24000 0x1000> -> interconnect@48000000 => 0x48024000
  UART4: 0x48000000 + 0x100000 + 0xa8000 = 0x481a8000
- clock-frequency = 0x02dc6c00 = 48 MHz
- /proc/iomem lists only regions claimed by a driver (request_mem_region), not all hardware
- Pinmux values: UART2 RX 0x44e10950=0x31, TX 0x44e10954=0x09; UART4 RX 0x44e10870=0x36 (pin 28), TX 0x44e10874=0x0e (pin 29)

## Step 2: skeleton + of_match_table
- One driver, two devices -> probe() called once per device (48024000.serial, 481a8000.serial)
- remove() order is reversed (481a8000 first)
- Pinmux applied by driver core before probe (pinctrl_bind_pins), released after remove
- /sys/bus/platform/drivers/serial/ exists only while driver is registered;
  device symlinks appear only when compatible matches AND probe() returns 0
- bind/unbind in sysfs re-runs probe/remove for a single device without rmmod
- Pitfall: cd failure + echo > unbind created plain files in ~ ; use full paths in /sys

## Step 3: private struct + ioremap
- devm_kzalloc per device; devm_platform_ioremap_resource = get resource + request_mem_region + ioremap
- /proc/iomem shows "48024000-48024fff : 48024000.serial serial@0" while bound, gone after rmmod
- Virtual address is FIXED: phys 0x48xxxxxx -> virt 0xfaxxxxxx (OMAP static iotable for L4_PER),
  because ARM ioremap reuses static mappings (find_static_vm_paddr)
- %p is hashed with a per-boot key; %px shows the raw value (debug only)

