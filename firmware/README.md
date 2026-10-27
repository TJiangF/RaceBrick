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
- **按页自动节能**：只有需要实时 GNSS 的页面（导航/性能/定位/比赛/录制/GPS 调试）使用设定速率，其余页面自动降到 **1 Hz**（进页即切换，切换仅发 CFG-RATE/CFG-MSG，不做波特率扫描）。
- **1PPS 时间脉冲**：CFG-TP5 配置 1 Hz/100 ms 脉冲，NEO 的 TP 脚接 **GPIO8**，上升沿 ISR（IRAM，`esp_timer_get_time`）测周期；调试页/导航页显示 `PPS` 状态（实测 999 ms）。
- MPU6050：I2C 真实读数；**开机姿态 = 零点**（扣除重力/安装角），自动识别竖直轴。
- 设置持久化（NVS）：主题/亮度/LED/声音/GPS 速率等。
- Debug 页：GPS 实时页（2 页，UP/DOWN 切换，第 2 页按 SNR 排序卫星）、IMU DEBUG。
- 性能测试页（HOME 轮播页，**PUSH 进入**）：**0-100 km/h** 与 **100-0 km/h** 计时，IMU 纵向突变触发开始，GPS 速度判定完成。无 GPS fix 时提示并可直接跳到 GPS 调试页帮助搜星。
- WiFi 配网（Settings → **WIFI SETUP**）：ESP32 开 SoftAP `RaceBrick-xxxx`（密码 `racebrick`），手机连上后浏览器打开 `http://192.168.4.1`。

- **PHONE MAP**：BLE 接收 NavBridge 发来的 320×240 整帧并全屏显示；设备名 `RaceBrick`，服务 `6e400001...`，协议与 App 见仓库根 README。
- **MEDIA**：三个圆形按钮（上一首 / 播放暂停 / 下一首），通过 BLE 的 `M,PREV` / `M,TOGGLE` / `M,NEXT` 控制手机当前媒体会话；`M,STATE,0/1` 回传播放状态。
- **LOCATION 实时地图页**（HOME 轮播，与 Start Race 同级；**PUSH 进入**，未进入时 UP/DOWN 切换轮播页）：从 RaceBrick_Sat2Pixel 拉取当前位置的像素风路网图（`pal16`），heading-up 显示，自车箭头；速度右上角亮绿显示，GPS/地图状态右下角，WiFi 状态在电池胶囊内。

### 页面布局（240px 全高）
- 移除顶部时钟 + 10 颗模拟 LED、以及底部状态栏；320×240 全部给页面内容。
- LED 效果全部由真实 WS2812 实现；底部状态改为**左下角小胶囊**，电量在右下角，Toast 浮层。
- 列表随高度增加可见行数（Settings 6 行、Track List 4、History 4、History Detail 5、Track Edit 4），地图页地图高度 226。
- 调试接口：`GET /api/fb[?f=1]` 导出 320×240 RGB565 帧缓冲（`f=1` 冻结渲染避免撕裂），`GET /api/goto?n=<scid>&e=1` 跳页。

### 实时导航（Navigation）
- 接口：`POST http://47.100.182.210:2020/api/v1/nav`，通用参数 `format=pal16`、`coord=wgs84`、`size_m=400`、`zoom=0.5–4.0`、`heading=<平滑航向>`；普通模式 `240×240`，全屏模式 `320×240`。风格用 `mode` 选择：
  - `"mode":"road","theme":"gameboy"` → 像素风路网（palCnt=4，实测 877 B/帧）
  - `"mode":"satellite"` → 卫星原图（palCnt=16，240×240 实测约 37 KB/帧）
  - `"mode":"blend","sat_opacity":30|60|90` → 卫星按透明度叠在路网上（palCnt=16，约 37 KB/帧；100 = 纯卫星）
- **PUSH 菜单**（进入 LOCATION 后）：`REFRESH` 立即刷新；`STYLE` → `ROAD PIXEL` / `SATELLITE` / `BLEND 30%` / `BLEND 60%` / `BLEND 90%`；`ZOOM IN` / `ZOOM OUT` 调整地面倍率（4×→约 6,553 km 宽，可缩到中国全图，写入 NVS；菜单保持打开便于连续缩放）；`FULLSCREEN` 切换全屏 320×240 地图（写入 NVS）。选择后立即重取。未进入时 PUSH 启动、UP/DOWN 切轮播页。
- 航向：GPS COG 在 <1.5 km/h 时无效；运动时 ~0.35 s 低通平滑，静止保持上次航向，从未移动过则正北朝上（不转）。
- 失败处理：保留上一帧；429/502/503 立即停止本次重试，失败后 5/10/20/30 s 指数退避；信息栏显示 `HTTP xxx`。road/blend 依赖天地图，若服务端 `TILE_FETCH_FAILED/302010`（tk 限流）会 502，可先用 SATELLITE。
- 后台任务取图 + RLE 解码到 PSRAM 双缓冲，主循环叠加箭头与信息；移动 >12 m / 转向 >12° / 每 2.5 s 触发重取（失败重试 3 次，退避 1/2 s）。
- **传输优化**：HTTP keep-alive 复用 TCP 连接、导航页内关闭 WiFi modem sleep（`WIFI_PS_NONE`）、限流从请求开始计时并按剩余时间休眠，显著降低延迟抖动。
- **LOCATION 页信息**：速度亮绿色显示在右上角；GPS/地图状态在右下角。全屏模式下状态行使用白底胶囊框叠加在地图上；WiFi 连接状态并入右下角电池胶囊（绿色信号格=已连接，红色=未连接）；不再显示 HDG、PPS、zoom 数值和独立 WIFI/MAP OK 行。
- **刷新率（240×240 satellite）**：优化前请求 0.5–0.9 s、解码 9–17 ms、帧间隔 3.05–3.4 s；现在限流从请求开始计时，帧间隔约 2.2–2.4 s。限流 road 1.2 s / satellite-blend 2.2 s（手动刷新/缩放可立即）。放开限流理论上限约 1–2 fps（受服务端渲染/天地图配额限制）。
- **图像格式对比**（240×240）：
  - `rgb565`：115 KB/帧，无需解码、16 位色，但传输量大。
  - `pal16`：RLE，road 4 色实测 **877 字节/帧**、satellite 16 色约 37 KB/帧，解码极快 → **刷新率最高**（**已采用**）。
  - `png`/`jpeg`：需额外解码器（inflate/JPEG），CPU/Flash 开销大、延迟高，不推荐。
- 无 GPS fix 显示 `NO GPS`、无 WiFi 显示 `NO WIFI`；PUSH 手动刷新。Settings 中原静态中国地图页已改名 `CHINA MAP`。
  - 页面会**扫描并列出附近 WiFi**（下拉选择，或手动输入隐藏 SSID），选好填密码即可。
  - 保存后页面**轮询状态**：`CONNECTING...` → `CONNECTED <IP>`（失败约 20s 后显示 `FAILED`）。
  - 配网成功后写入 NVS；之后**开机自动连接**（同时保留 SoftAP 作为备用）。
  - 同一 WiFi 下的手机可用 `http://<STA_IP>/api/status`、`/api/tracks`、`/api/scan`、`POST /api/control`。

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
