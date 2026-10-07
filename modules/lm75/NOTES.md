# LM75B on BeagleBone Black — 實驗筆記

## 硬體

- NXP LM75BD，紫色 5-pin 模組（VCC GND SDA SCL OS）
- 模組零件：去耦電容、10k×2（SDA/SCL pull-up 到模組 VCC）、200k（OS pull-up）、1k + 綠色 LED（OS active low 時亮）
- 接線：VCC→P9_3（3.3V）、GND→P9_1、SDA→P9_18、SCL→P9_17（I2C1，bus 1，與 Nunchuk 0x52 共用）
- **VCC 只能接 3.3V**：pull-up 拉到模組 VCC，接 5V 會把 5V 灌進 AM335x 的 I/O
- 邏輯分析儀：FX2（fx2lafw）D0=SCL、D1=SDA，2 MHz

## Step 1：user space 驗證（2026-10-07）

### 位址
- `i2cdetect -y -r 1` → **0x48**（A2..A0 在模組上接地；位址格式 `1001 A2 A1 A0`）
- Linux 一律使用 7-bit 位址；線上的位址 byte 為 0x90（W）/ 0x91（R）

### 上電預設值（實測）
| 暫存器 | ptr | i2cget | 實際值 | 意義 |
|---|---|---|---|---|
| Conf  | 0x01 | `0x00`   | 0x00   | normal, comparator, OS active low, fault queue 1 |
| Thyst | 0x02 | `0x004b` | 0x4B00 | 75 °C |
| Tos   | 0x03 | `0x0050` | 0x5000 | 80 °C |
| Temp  | 0x00 | `0x401b` | 0x1B40 | 27.25 °C |

### Byte 順序
- SMBus word = little-endian（線上第一個 byte 當 low byte）；LM75 = MSByte 先傳
- → i2cget/i2cset 的 word 值都是 byte 對調過的
- 波形實證：`i2cget ... 0x00 w` 顯示 `0xc01a`，線上順序為 `1A C0`
- kernel 對策：`i2c_smbus_read_word_swapped()` / `i2c_smbus_write_word_swapped()`

### 溫度換算
- Temp：11-bit 二補數，`(s16)reg >> 5`，× 0.125 °C → 毫度：`((s16)reg >> 5) * 125`
- Tos/Thyst：9-bit 二補數，`(s16)reg >> 7`，× 0.5 °C
- 整數 °C 寫入 Tos/Thyst：`reg = T << 8`

### OS / LED 行為（comparator mode）
- Thyst=28 / Tos=30：超過 30 → LED 亮，降到 28 以下 → LED 滅 ✔（遲滯）
- **邊界**：Tos=31.0，Temp 最高 31.125（9-bit = 31.0，等於 Tos）時 LED 已亮
  - 結論：等於即觸發（>=），或比較時使用比 9 bit 更高的解析度；datasheet 只寫 "exceeds"
  - driver 影響：temp1_max 寫 31000 時，警報點約在 31.0 °C
- 室溫約 27 °C 時 Thyst=28 太接近室溫，加上 LED/OS 灌電流自熱，降溫到 Thyst 以下非常慢 → 門檻要離室溫遠一點
- 更新門檻的順序：往下調先降 Thyst，往上調先升 Tos，避免中間出現 Thyst >= Tos（datasheet 7.6：未定義行為）
- Tos/Thyst 斷電後回到 POR 預設值（80/75）

### I2C 波形（captures/*.sr）
| 檔案 | 指令 | 解碼結果 |
|---|---|---|
| read_temp.sr | `i2cget -y 1 0x48 0x00 w` | `S 48W A 00 A Sr 48R A 1A A A0 N P` |
| write_tos.sr | `i2cset -y 1 0x48 0x03 0x0050 w` | `S 48W A 03 A 50 A 00 A P` |
| recv_byte.sr | `i2cget -y 1 0x48`（pointer 停在 0x03） | `S 48R A 50 N P` |

- 讀 word：master 對第一個 byte 回 ACK，最後一個回 NACK；寫入時每個 byte 都由 slave 回 ACK
- Read word data = 一次 `i2c_transfer()`、兩個 `i2c_msg`（`smbus_xfer_emulated()`）→ 中間是 Sr 不是 Stop
- Pointer 會鎖存：不帶 pointer 的讀取會讀到上次指定的暫存器 → driver 每次讀取都要帶 pointer
- 擷取陷阱：trigger 在 SDA 下降沿時沒有 pre-trigger，會漏掉第一個 START，decoder 把 Sr 誤認成 Start、丟掉 pointer write
  - 解法：`--config samplerate=2m:captureratio=10`

### 擷取指令
```sh
sigrok-cli -d fx2lafw --config samplerate=2m:captureratio=10 \
    -C D0,D1 -t D1=f -w --samples 400k -o <name>.sr
sigrok-cli -i <name>.sr -P i2c:scl=D0:sda=D1 \
    -A i2c=start:repeat-start:stop:ack:nack:address-read:address-write:data-read:data-write
```

## 其他觀察
- Nunchuk driver 沒載入時 i2cdetect 顯示 `52`；載入並綁定後顯示 `UU`
- 綁定後 i2cget 對該位址會回 `Device or resource busy`（除非 `-f`）
