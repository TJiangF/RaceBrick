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
| 外置 Flash CS / MISO / SCK / MOSI | 39 / 38 / 37 / 36（W25Q128，SPI3） |

## 已实现

- 像素 UI（原始帧缓冲）：HOME / 赛道 / 设置 / 历史 / 定位 / 调试等页面与状态机。
- ST7789（40 MHz）、外置 W25Q128 自检、WS2812（RMT）灯效、蜂鸣器（无源 PWM 旋律）。
- NEO-M9N：UART（自动扫描波特率找到模块），UBX 配置 1/5/10/25 Hz（Settings → GPS RATE）。
- MPU6050：I2C 真实读数；**开机姿态 = 零点**（扣除重力/安装角），自动识别竖直轴。
- 设置持久化（NVS）：主题/亮度/LED/声音/GPS 速率等。
- Debug 页：GPS 实时页（2 页，UP/DOWN 切换，第 2 页按 SNR 排序卫星）、IMU DEBUG。

## 真实 GNSS 赛道模式（进行中）

目标：用 **NEO-M9N 的真实定位**替代原来的模拟轨迹。当前阶段**只使用 GNSS**（不融合 IMU）来建立轨迹、计算速度与圈速。

### 数据链路
```
lat/lon ──geo_meters()──▶ 局部米制 (x=东, y=北)
        ──归一化──▶ 赛道坐标 [0,1]
        ──最近线段投影──▶ 赛道进度 s、横向偏差、过线判圈
```

### 录制流程（NEW TRACK）
1. 必须处于 **3D fix**，否则提示「当前没有定位」并拒绝开始。
2. 在起点按 **PUSH** 开始记录，沿赛道跑一圈。
3. 回到起点按 **PUSH** 结束，用真实米制点生成赛道（保存 origin、比例、真实周长）。

### 起终点判定
- **新赛道**：手动标注起点；以该点处「与前进方向垂直的切线」为起终线，左右各延伸 3 m（共 6 m）；再次穿过该线判定完成一圈。
- **已录制赛道**：可手动修改起点，交互类似 Section 的前后拖动。

### 存储
- 赛道保存到**外置 W25Q128 flash**（掉电保留）：固定 5 个 4 KiB 槽，每槽一条赛道 + FNV 校验；结构约 3000 字节。
- RAM 中最多 6 条赛道（1 条样例 + 5 条录制），开机从 flash 加载。
- 「Recorded Track」(TRACKS) 页面显示 **`FLASH n/5`** 与剩余 KB。

### G 值 / 安装姿态
- PCB 为**竖直安装**（垂直于地面），MPU6050 丝印**反向（头朝下）**。
- 因此前后 / 左右加速度方向需要修正（见 `config.h` 的 `IMU_SIGN_*` / `IMU_SWAP_*`，可现场微调）。
- 本阶段**暂不把 IMU 融合进轨迹**；G 值先在 IMU DEBUG 页验证方向。

### 路线图
- [x] Step 1：经纬度→米制、真实轨迹采样、GPS TRACK 预览页（Settings → GPS TRACK）。
- [x] Step 2：赛道地理基准（origin / 比例 / 真实周长）+ 最近线段投影。
- [x] Step 3：真实赛道录制（NEW TRACK）：3D fix 门控，起点 PUSH 开始，跑一圈回起点 PUSH 结束并生成赛道（记录 origin/比例/真实周长/起始朝向）。当前保存在 RAM。起终线 6 m 判圈在 Step 4 使用。
- [x] Step 4：真实 GNSS 比赛：把实时 fix 投影到赛道得到进度 s，6 m 起终线判圈，速度来自 GPS，记录圈速/最佳圈与轨迹。当前不含 IMU 融合，G-meter 暂为空。
- [x] Step 5：赛道持久化到外置 W25Q128（5 个 4 KiB 槽，FNV 校验；写入/读回比对/擦除自检 PASS），开机自动加载；TRACKS 页右上角显示 `FLASH n/5 · xK FREE`；已录赛道可进入 **START LINE** 页用 UP/DOWN 拖动起终点、PUSH 保存（写回 flash）。
- [x] Step 6：真实 IMU 接入赛车 **G-meter**（G-G 图，横向/纵向按安装轴向映射，平滑 + 拖尾）。

## 约定 / 注意事项
- 只保留 **一条** 旧的模拟赛道作为样例；其余真实赛道来自录制。
- 开机时请保持板子/车**静止**，以获得准确的 IMU 零点与竖直轴。
- GPS 约 1–2 m 噪声、偶发丢星；会做平滑与离群剔除，本期接受该精度。
