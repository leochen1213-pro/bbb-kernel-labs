# ssd1306 — 0.91" 128x32 I2C OLED driver (BeagleBone Black)

Own I2C driver for an SSD1306 OLED, built up one commit per step, ending with
the LM75 temperature shown on the display via `user/oled_temp`.

## Hardware

| Item | Value |
|---|---|
| Module | 0.91" I2C OLED, 128x32, SSD1306, pins `GND VCC SCL SDA` |
| Address | `0x3C` |
| Bus | BBB I2C1 → `/dev/i2c-1`, P9_17 SCL / P9_18 SDA |
| Shared with | LM75 `0x48`, Nunchuk `0x52` |
| Debug | FX2 logic analyzer (CH0 SCL, CH1 SDA, GND P9_2) |

DTS changes live in `../../kernel-patches/` (exported with `git format-patch`).

## Layout

```
tools/oled_init.sh   stage 1: bring-up with i2ctransfer (clear | fill XX | ramp | off)
user/oled_temp.c     reads LM75 hwmon temp1_input, writes 512-byte frames to /dev/oled
user/Makefile        native or CROSS_COMPILE=arm-linux-gnueabihf-
docs/                notes and logic analyzer captures
```

## Roadmap

- [x] **1 User space** — i2cdetect, i2ctransfer init sequence, display lit
      ([notes](docs/stage1-notes.md))
- [ ] 1b Logic analyzer: `0x00` (command) vs `0x40` (data) control byte, time a 513-byte write
- [x] **2 Upstream driver** — DT node `solomon,ssd1306` + `CONFIG_DRM_SSD130X_I2C`, then disable ([notes](docs/stage2-notes.md))
- [ ] **3 Own driver**
  - [ ] 3.1 `i2c_driver` skeleton, `of_match_table("leo,ssd1306")`, `module_i2c_driver`
  - [ ] 3.2 `probe()` sends init sequence, `remove()` turns display off
  - [ ] 3.3 `struct oled` + misc device + mutex, `write()` takes 512 bytes → `oled_temp` ⭐
  - [ ] 3.4 sysfs: `contrast` / `invert` / `display`
  - [ ] 3.5 Performance: 100 kHz vs 400 kHz, send only dirty pages
  - [ ] 3.6 (optional) fbdev deferred I/O: mmap + page fault + workqueue
  - [ ] 3.7 (optional) compare with upstream `drivers/gpu/drm/solomon/ssd130x.c`
