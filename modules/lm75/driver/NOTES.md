## Step 3.4：OS 中斷
- OS → P9_12（gpio1_28），DT：interrupts = <28 IRQ_TYPE_LEVEL_LOW>，Linux IRQ 46
- probe 切到 interrupt 模式（Conf 0x00 → 0x02），unbind 時用 devm action 還原
- threaded IRQ + IRQF_ONESHOT：清中斷要讀 I2C（會睡眠），level 觸發要遮罩到 thread 結束

### 發現 1：晶片兩側都是「含等號」的 9-bit 比較
- 超溫：T9 >= Tos（30125 → T9 = 30.0 時觸發）
- 解除：T9 <= Thyst（28375 → T9 = 28.0 時觸發，不是低於 28.0）
- 第一版用 11-bit 溫度、temp < thyst 判斷 → 解除事件永遠不成立（bug）
- 修正：thread 用 T9 判斷，跟晶片的比較方式一致

### 發現 2：每個事件觸發 2 次 IRQ（相隔約 16 ms）
- 推測是 GPIO 控制器在遮罩期間鎖存了中斷狀態（未用邏輯分析儀證實）
- thread 讀狀態再判斷，不用 toggle，所以重複的 IRQ 不會造成影響

### 發現 3：warm reboot 不會重置 LM75
- reboot 時 3.3V 不斷電 → Conf/Tos/Thyst 都保留上一次開機的值
- 開機後第一次 probe 讀到 conf=0x02（上次留下的 interrupt 模式）
- driver 不能假設晶片是上電預設值：讀出現況，只改需要的 bit，unbind 時還原成接手時的值


#### NOTES
1. 晶片兩側都是含等號的 9-bit 比較：超溫是 9-bit 溫度 ≥ Tos，解除是 9-bit 溫度 ≤ Thyst
2. 每個事件會觸發 2 次 IRQ
3. warm reboot 不會重置 LM75
