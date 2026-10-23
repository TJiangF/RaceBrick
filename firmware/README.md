# RaceBrick 固件（ESP32-S3 + ST7789）

基于 **ESP-IDF / PlatformIO** 的实机固件，UI 与交互移植自仓库根目录的 Web 原型。
代码在本 `firmware/` 子目录；Web 原型见根目录 `README.md`。

## 构建 / 烧录

```bash
cd firmware
pio run                                   # 构建
pio run -t upload --upload-port /dev/cu.wchusbserial1140   # 端口以实际为准
pio device monitor -b 115200              # 串口日志
```

## 硬件（生产板 Rev.B）

| 功能 | 引脚 |
|---|---|
| LCD ST7789 CS / SCK / MOSI / DC / BL | 12 / 11 / 14 / 9 / 10 |
| 按键 UP / DOWN / PUSH / OK / BACK | 40 / 42 / 41 / 5 / 4（UP/DOWN 已按滚轮方向交换） |
| I2C SCL / SDA（MPU6050 @0x68） | 16 / 17 |
| MPU6050 INT | 15 |
| GNSS NEO-M9N：ESP TX / ESP RX / TP | 19 / 20 / 8（`NEO-RX`=ESP TX，`NEO-TX`=ESP RX） |
| 蜂鸣器 BEE / WS2812 DI | 47 / 48 |
| VBAT 检测使能 / ADC | 21 / 1（使能拉高采样，读数 ×2） |
| 外置 Flash CS / MISO / SCK / MOSI | 39 / 38 / 37 / 36（W25Q128，SPI3）——**当前未使用**（与 Octal PSRAM 引脚冲突，见下） |

## 已实现

- 像素 UI（原始帧缓冲）：HOME / 赛道 / 设置 / 历史 / 定位 / 调试等页面与状态机。
- ST7789（40 MHz）、WS2812（RMT）灯效、蜂鸣器（无源 PWM 旋律）。
- NEO-M9N：UART（自动扫描波特率找到模块），UBX 配置 1/5/10/25 Hz（Settings → GPS RATE）。
- MPU6050：I2C 真实读数；**开机姿态 = 零点**（扣除重力/安装角），自动识别竖直轴。
- 设置持久化（NVS）：主题/亮度/LED/声音/GPS 速率等。
- Debug 页：GPS 实时页（2 页，UP/DOWN 切换，第 2 页按 SNR 排序卫星）、IMU DEBUG。
- 性能测试页（HOME 轮播页）：**0-100 km/h** 与 **100-0 km/h** 计时，IMU 纵向突变触发开始，GPS 速度判定完成。无 GPS fix 时提示并可直接跳到 GPS 调试页帮助搜星。
- WiFi 配网（Settings → **WIFI SETUP**）：ESP32 开 SoftAP `RaceBrick-xxxx`（密码 `racebrick`），手机连上后浏览器打开 `http://192.168.4.1` 填家里 WiFi 的 SSID/密码，保存到 NVS 并连接；页面/设备显示状态与 IP。

### 内存 / Flash
- 320×240 RGB565 帧缓冲（150 KB）放在 **8 MB Octal PSRAM（40 MHz）**，内部 DRAM 留给 WiFi（当前 RAM 46%）。
- app 分区 **3 MB** + `storage` 2 MB（`partitions.csv` + `board_build.partitions`），Flash 约 32%。
- PSRAM 相关的坑已处理：UART/ADC/I2C/SPI/RMT 的 ISR 放进 IRAM，否则会在 PSRAM 存取时卡死 WDT。

## 计时精度
- MCU 侧：`esp_timer` **1 µs** 分辨率、单调递增，时间戳足够精确。
- 触发：性能测试待触发时以 **~2 ms** 间隔快速采样 MPU6050，触发时间戳 ~2 ms。
- 完成判定：受 **GPS 速度更新率**限制（10/25 Hz → 40–100 ms）与速度精度（~0.1 m/s）。
- 结论：计时器本身很准，实际误差主要来自 GPS 速度采样率；**25 Hz 下 0-100 典型误差约 ±40–60 ms**。

## 真实 GNSS 赛道模式（进行中）

目标：用 **NEO-M9N 的真实定位**替代原来的模拟轨迹。当前阶段**只使用 GNSS**（不融合 IMU）来建立轨迹、计算速度与圈速。

### 地图朝向
- 地图固定**北向上**（GPS 东 = 屏幕右、北 = 屏幕上）；屏幕 y 向下，渲染时把 y 翻转，保证左右与实际一致。投影/判圈仍在北为正的米制系里，不受显示翻转影响。

### 数据链路
```
lat/lon ──geo_meters()──▶ 局部米制 (x=东, y=北)
        ──归一化──▶ 赛道坐标 [0,1]
        ──最近线段投影──▶ 赛道进度 s、横向偏差、过线判圈
```

### 录制流程（NEW TRACK，自动判圈）
1. 必须处于 **3D fix**，否则提示「当前没有定位」并拒绝开始。
2. 在起点按 **PUSH**：标记起终线（位置=当前点；朝向由起步后前几个点自动确定），开始第 1 圈。
3. 回到起终线附近（6 m 宽，正向穿过）会**自动完成本圈并立即开始下一圈**，同时显示 当前圈时 / 上圈 / 最佳圈 / Delta。
4. 再按 **PUSH** 结束录制：用**最后一个完成的圈**生成赛道（origin、比例、真实周长、起终线），进入命名保存。

### 起终点判定
- **新赛道**：手动标注起点；以该点处「与前进方向垂直的切线」为起终线，左右各延伸 3 m（共 6 m）；再次穿过该线判定完成一圈。
- **已录制赛道**：可手动修改起点，交互类似 Section 的前后拖动。

### 存储
- 赛道保存到**内部 flash 的 `storage` 分区**（2 MiB，data/spiffs）：固定 5 个 4 KiB 槽，每槽一条赛道 + FNV 校验；结构约 3000 字节。
- **外部 W25Q128 暂时不用**：Octal PSRAM 占用 GPIO33–37，与外部 Flash 的 SCK=37 / MOSI=36 冲突（初始化 SPI3 会破坏 PSRAM）。若以后要用外部 Flash，需改板把它的引脚挪开，或改用 Quad PSRAM。
- RAM 中最多 6 条赛道（1 条样例 + 5 条录制），开机从 flash 加载。
- 「Recorded Track」(TRACKS) 页面显示 **`FLASH n/5`** 与剩余 KB。

### G 值 / 安装姿态
- PCB 为**竖直安装**（垂直于地面），MPU6050 丝印**反向（头朝下）**。
- 因此前后 / 左右加速度方向需要修正（见 `config.h` 的 `IMU_SIGN_*` / `IMU_SWAP_*`，可现场微调）。
- 本阶段**暂不把 IMU 融合进轨迹**；G 值先在 IMU DEBUG 页验证方向。

### 路线图
- [x] Step 1：经纬度→米制、真实轨迹采样（早期用 GPS TRACK 预览页验证，验证后已移除）。
- [x] Step 2：赛道地理基准（origin / 比例 / 真实周长）+ 最近线段投影。
- [x] Step 3：真实赛道录制（NEW TRACK）：3D fix 门控，起点 PUSH 开始，跑一圈回起点 PUSH 结束并生成赛道（记录 origin/比例/真实周长/起始朝向）。当前保存在 RAM。起终线 6 m 判圈在 Step 4 使用。
- [x] Step 4：真实 GNSS 比赛：把实时 fix 投影到赛道得到进度 s，6 m 起终线判圈，速度来自 GPS，记录圈速/最佳圈与轨迹。当前不含 IMU 融合，G-meter 暂为空。
- [x] Step 5：赛道持久化到**内部 flash `storage` 分区**（5 个 4 KiB 槽，FNV 校验），开机自动加载；TRACKS 页右上角显示 `FLASH n/5 · xK FREE`；已录赛道可进入 **START LINE** 页用 UP/DOWN 拖动起终点、PUSH 保存（写回内部 flash）。
- [x] Step 6：真实 IMU 接入赛车 **G-meter**（G-G 图，横向/纵向按安装轴向映射，平滑 + 拖尾）。
- [x] Step 7：GNSS 比赛鲁棒性：投影**连续性窗口**（只在上次进度附近搜索，避免自交叉/近邻路段跳段）+ **离轨剔除**（偏离赛道 >25 m 的样本忽略）。

## 约定 / 注意事项
- 只保留 **一条** 旧的模拟赛道作为样例；其余真实赛道来自录制。
- 开机时请保持板子/车**静止**，以获得准确的 IMU 零点与竖直轴。
- GPS 约 1–2 m 噪声、偶发丢星；会做平滑与离群剔除，本期接受该精度。
