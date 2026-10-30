# NavBridge

Android MVP：用高德 SDK 算路/导航，把 320×240 全帧（地图 + 路线 + 车标 + HUD）通过 BLE 推给 RaceBrick。
也保留 MotoNav 圆屏的底图/路线协议。

协议对齐 [MotoNav](https://github.com/zyn950929/MotoNav) 的 UART 服务（`6e400001...`、`M` 大块头、`#p` 定位、`R/L/S` 转向）。
RaceBrick 使用同一套服务接收 **type 5 整帧**，在 `PHONE MAP` 页面全屏显示。

> 状态：已在 **JDK 21 + Gradle 8.10.2 + AGP 8.7.3 + 高德 10.1.600** 下编译通过并生成 debug APK；模拟导航和 HUD 已在 Android 模拟器验证。仅供个人非商业使用。
>
> APK 约 54 MB，主要是高德导航/3D 地图/定位/搜索 SDK 的 arm64 原生库；业务代码很小，当前功能下无法显著裁剪。
>
> 每次重新出包前递增 `app/build.gradle.kts` 的 `versionCode` / `versionName`；首页会显示版本号。
>
> 速度显示优先用设备 NEO 的 `#G`（3 秒内有效），没有设备速度时回退到高德导航回调速度。
>
> 媒体控制需要“通知使用权”；首页有授权按钮。debug 包可用 `adb shell am start -n com.example.navbridge/.MainActivity --es media_cmd TOGGLE` 测试。

## 功能

- 输入目的地关键字 → 高德 Inputtips 联想 → 选中 POI → 高德驾车算路 → 真实 GPS 导航
- 建议没有坐标时自动用 GeocodeSearch 兜底解析
- 手机端旁听高德导航回调：转向方向/距离、剩余里程/时间、语音播报文本
- MotoNav 风格：天地图 `vec_w` 800×800 底图（瓦片层级 +1）→ 4 色分类/RLE；不再依赖高德截图
- SDK 原图：高德底图离屏截图 → 降采样/压色/RLE（约 20~80 KB/块）
- 开出当前地图块约 600 m 后自动取下一块（向前偏 350 m）
- BLE 扫描/连接/配对、MTU 协商、大块分片、`#p` 定位、路线包
- 手机界面显示 **320×240 横向**导航画面模拟：官方转向图标、到下一动作距离、下一条路、剩余距离/时间、车速
- 推荐车道：每条车道的背景方向 + 推荐车道高亮（绿色）
- 限速：顶部中间显示最近测速电子眼的限速
- 电子眼：在地图对应位置标出（测速/监控/违章等）
- 红绿灯：只在地图对应点位画一个小型红绿灯图标；不显示距离、数量、相位或倒计时
- 拥堵着色：路线按畅通/缓行/拥堵/严重拥堵显示绿/黄/红/深红
- 视图大小 0.75×~2×：手机按钮切换，设备也可发 `#Z` 选择
- 4 种风格：掌机像素（默认）、罪恶都市、原版、极简黑白；主页面“风格”按钮循环切换并记住选择
- 3 种出行工具：汽车（默认）、摩托车、自行车；主页面“工具”按钮切换并记住选择。汽车走高德驾车策略，摩托车走高德摩托策略，自行车走高德骑行路线；模拟速度 90/50/20 km/h
- MotoNav 风格底图：天地图 `vec_w` 矢量瓦片（WGS-84）；高德路线/定位/电子眼/拥堵自动 `GCJ-02 → WGS-84`
- SDK 原图：高德离屏截图；两种预览可切换，切换时会重新取对应底图
- 天地图直连需要 Key IP 白名单；失败自动回退 `https://racebrick.tfworks.cn/api/tile?layer=road` 代理
- 预览可在“SDK 原图”和 MotoNav 风格之间切换；MotoNav 风格可再选 4 种配色
- 主页面简化：连接设备、开始/停止导航、模拟导航、视图缩放；Key/包名/SHA1/媒体权限状态在设置页
- 视角可在“车头朝上”和“朝北”之间切换
- 车头朝上**不放大**：地图块抓 7500×7500m，可见画面仍是 2400×1800m（7.5m/px），旋转时用块四周的余量补角
- 高德**模拟导航**：用当前手机定位作为起点，沿算好的路线模拟到目的地；必须选择目的地，模拟速度 90 km/h
- HUD 颜色：SDK 原图模式为黑色；深色 MotoNav 风格为白色；浅色风格为白色并带黑色阴影；左上角使用单个箭头字符，避免官方图标阴影层造成双箭头
- 左上角转向信息、右下角剩余距离/时间加白色信息面板；浅色风格加黑色边框
- 速度圆牌统一为白底黑边黑字；车标为 MotoNav 白色箭头 + 黑色描边
- 手机端 2× 超采样后缩到 320×240，路线/文字边缘更柔和
- **音乐控制**：ESP 的 MEDIA 页发送 `M,PREV` / `M,NEXT` / `M,TOGGLE`，App 控制当前媒体会话，并回传 `M,STATE,0/1`；需要开启通知使用权
- **设备 NEO 速度**：ESP32 通过 BLE `#G` 上报 WGS84 定位/速度；导航定位仍用手机 GPS，HUD 速度在 `#G` 新鲜时优先用设备速度
- **手机整帧渲染**：320×240 全帧（地图+路线+车标+HUD）→ 风格对应 16 色量化 → 调色板 RLE，始终作为 type 5 帧发送（RaceBrick）
- **后台保活**：开始导航启动前台服务 + PARTIAL_WAKE_LOCK，切换 App 后继续 GPS/渲染/BLE 推流；需授权“始终允许定位”和关闭电池限制
- **LED 导航进度**：App 发送 `#P,remain,total`；导航页 WS2812 彩虹呼吸，距离越近亮起越多，到达全亮；真实/模拟导航均生效
- 没连设备也能先导航；底图/路线/整帧会缓存，连上设备后自动补发

## 架构

```
高德 SDK（定位/算路/导航）
        │  回调
        ▼
  NavEngine ── 转向/距离 ──────────────┐
      │                               │
      ▼                               ▼
  MapCapture（离屏 TextureMapView）   BleClient（BLE 中心）
      │ 1200×1200 截图                │  ASCII 指令 / M 大块分片
      ▼                               ▼
  RasterCodec（4 色 + RLE）──────► ESP32-S3 圆屏
```

圆屏端不是收视频流：它收一张低频底图位图 + 高频位置/路线矢量，然后在本地合成车标、转向环、路线。

## 前置条件

- Android Studio（自带 JDK 17）
- 一台 Android 8.0+ 手机
- 微雪 ESP32-S3 1.75 英寸 AMOLED 圆屏，刷 MotoNav 1.75 固件
- 高德开放平台账号 + Android 平台 Key

## 一、获取签名 SHA1（申请高德 Key 要用）

高德 Android Key 绑定 **包名 + 签名 SHA1**，两者都对才能用。

### 最省事：让 Android Studio 算

1. 用 Android Studio 打开本工程，等 Gradle Sync 完成。
2. 右侧 Gradle 面板 → `app` → `Tasks` → `android` → 双击 `signingReport`。
3. 输出里找 `Variant: debug` 下面的 `SHA1:`。

或者命令行（在工程根目录）：

```bash
./gradlew signingReport
```

### 用 keytool 算（macOS）

Android Studio 自带 JDK，keytool 路径：

```bash
"/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/keytool" \
  -list -v -keystore ~/.android/debug.keystore \
  -alias androiddebugkey -storepass android -keypass android
```

输出里的 `SHA1:` 就是。

> 第一次没编译过时 `~/.android/debug.keystore` 可能不存在，先在 Android Studio 里 Run 一次（或点 Build），会自动生成。

### release 包

用你自己的签名文件：

```bash
keytool -list -v -keystore /path/to/your.jks -alias yourAlias
```

如果用 Google Play 签名的，去 Play Console → 应用完整性 → 应用签名密钥证书里看 SHA-1。

> 换签名 / 换包名 → 旧 Key 立即失效，要回高德控制台新建 Key。

## 二、申请高德 Key

1. 打开 https://console.amap.com/dev/key/app
2. 创建应用 → 添加 Key → 服务平台选 **Android 平台**
3. 填：
   - **Package Name**：`com.example.navbridge`（想改就改 `app/build.gradle.kts` 里的 `applicationId`，两处要一致）
   - **SHA1**：上一步拿到的
4. 复制生成的 Key。

## 三、配置 Key

二选一：

**A. 写进 local.properties（推荐，编译期注入）**

复制 `local.properties.example` 为 `local.properties`，填：

```properties
sdk.dir=/Users/you/Library/Android/sdk
AMAP_KEY=你的key
```

**B. App 里填**

装好后在首页“高德 Android Key”输入框粘贴，点“保存 Key 并重启 App”。

首页会显示当前包名和签名 SHA1，方便复制去高德控制台核对。

## 四、编译（重要：JDK 版本）

Android Studio 2026.2 自带的 JBR 是 **Java 25**，而本工程用的 Gradle 8.10.2 解析不了 25，会报：

```
IllegalArgumentException: 25.0.3
```

把 Gradle JDK 换成 **21**（或 17）即可：

1. Android Studio → `Settings`（macOS 是 `Android Studio → Settings`）
2. `Build, Execution, Deployment` → `Build Tools` → `Gradle`
3. `Gradle JDK` → 下拉选 `Download JDK…` → 版本选 **21** → Download
4. 回到 Gradle JDK 选中刚下载的 21 → Apply → 重新 Sync

或者命令行自己装一个 21：

```bash
brew install openjdk@21
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
./gradlew assembleDebug
```

输出：`app/build/outputs/apk/debug/app-debug.apk`（约 57 MB，含 arm64 高德 so）。

> 本机已经生成过一版：`app/build/outputs/apk/debug/app-debug.apk`，签名 SHA1 为
> `CC:FD:DF:16:17:AA:96:47:FE:1A:F5:AC:5D:76:73:5D:C7:B5:E2:DC`。
> 对应 `~/.android/debug.keystore`，**不要删除**，否则以后重装包签名会变、高德 Key 失效。

## 五、使用

1. 圆屏刷 MotoNav 1.75 固件并开机。
2. 手机打开 NavBridge，授予定位、蓝牙、通知权限。
3. 点“连接圆屏”，在系统配对框输入圆屏上显示的 6 位数字。
4. 在“输入目的地”框输入地名，点联想结果选中目的地，点“开始导航”。
5. 圆屏开始显示底图、路线、车标、转向提示。

> 联想结果带坐标时直接使用（高德坐标本来就是 GCJ-02，和底图/算路一致）；没有坐标的公交线/路段会用地名自动地理编码。

## 协议（与 MotoNav 固件一致）

BLE 服务 `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- 写：`6e400002-...`（手机 → 设备）
- 通知：`6e400003-...`（设备 → 手机）

### ASCII（一次写一条，无换行）

| 指令 | 含义 |
|---|---|
| `#p,frame,xdm,ydm,heading,kmh` | 定位；x/y 是本地平面坐标，单位分米 |
| `L,300` / `R,300` / `S,300` | 转向；方向码 + 剩余米 |
| `SL/R/SR/U/A` | 左前/右前/掉头/到达 |
| `#t,0,1` | 选择原版配色（第三个字段表示采用新样式数据） |
| `#m,1` / `#n,0` | 开小地图 / 白天配色 |
| `#GB?` | 查询能力（配对后兜底触发 `U,1`） |
| `C` | 回到待机 |

圆屏 → 手机：

| 指令 | 含义 |
|---|---|
| `U,1` | 配对验证通过 |
| `K,type,id` | 大块数据收齐 |
| `#G,lat,lng,bearing,speed,accuracy,alt,time` | ESP32 的 NEO 定位。lat/lng 为 **WGS84**，speed 单位 m/s，bearing 度，accuracy 米，alt 米，time 为 epoch 毫秒（0=用手机时间） |
| `#Z,75` / `#Z,0.75` | 设置视图大小。大于 10 按百分比（75=0.75×、150=1.5×），否则按倍率；范围 0.75×~2× |
| `M,PREV` / `M,NEXT` / `M,TOGGLE` | 控制手机当前媒体会话（上一首/下一首/播放暂停）|

手机 → 设备：

| 指令 | 含义 |
|---|---|
| `M,STATE,0` / `M,STATE,1` | 当前媒体暂停/播放，用于设备 MEDIA 页中间图标 |

手机收到 `#G` 后会调用 `AMapNavi.setExtraGPSData(1, location)`（type=1 表示 WGS84）。
导航固定使用手机 GPS；设备 `#G` 仅用于速度显示（NEO 有 fix 时优先），不再提供设备 GPS 导航开关。

### 大块数据

15 字节头 + 分片：

```
'M' type id parts(LE16) seq(LE16) total(LE32) offset(LE32) payload
```

- `type=3` 底图
- `type=2` 路线
- `type=5` 手机整帧（320×240、16 色、调色板 RLE）

### 整帧包（type 5）

手机把底图、路线、车标、HUD 合成成 320×240 一帧，按 16 色调色板量化后 RLE：

```
u16 frameId, i32 0, i32 0, u16 320, u16 240, u16 (1000|0x8000),
16 × u16 RGB565 palette,
RLE(低4位=颜色索引, 高4位=run-1; 15 时再读 1 字节 = run-16)
```

约 10~40 KB/帧，建议 1 帧/秒。设备端只需解码到 320×240 的 4-bit 缓冲，再刷 ST7789。
App 现在始终发送整帧（type 5）；旧 MotoNav 圆屏固件会忽略 type 5。

### 底图包

```
u16 frameId, i32 x0, i32 y0, u16 w, u16 h, u16 mmPerPx,
then RLE: 低4位=颜色, 高4位=run-1; 高4位=15 时再读 1 字节 = run-16
```

颜色：0 街区、1 道路、2 绿地、3 水面。本工程 `w=h=1200`、`mmPerPx≈3875`（约 4.65 km 见方）。

> 因为车头朝上不放大，地图块从 2.4 km 扩大到了 7.5 km。旧 MotoNav 圆屏固件收到后会显示更宽的范围；新的 320×240 整帧固件不受影响，可见画面仍是 2400×1800m。

### 路线包

```
u16 frameId, i32 x0, i32 y0, u16 n, then (n-1) × (i16 dx, i16 dy)
```

单位米，相对上一点的差分。

## 已知限制 / 待办

- 已编译通过，但**未在真机和圆屏上联调**；首次上车前建议先原地模拟。
- 只在 `arm64-v8a` 打包；32 位老手机需要加 `armeabi-v7a`。
- 后台运行使用前台服务；Android 11+ 的“始终允许定位”需在系统设置里手动确认，部分厂商还需要允许自启动/后台运行。
- 底图取图依赖高德 `getMapScreenShot` 的离屏渲染，部分机型/系统可能有延迟或空白，代码里已有 3 次重试。
- 没有做 POI 搜索、常用地点、天气、轨迹、电子眼。
- 红绿灯只显示地图点位小图标；高德开放 SDK 不提供红绿灯相位和秒数，无法显示真实红/绿状态或倒计时。
- 限速来自最近测速电子眼，不是全路段限速；没有电子眼数据时不显示。
- 车道/电子眼/拥堵数据依赖高德覆盖，部分道路可能没有。
- 旧的 MotoNav 圆屏固件不会显示这些新元素；它们只在新的 320×240 整帧（type 5）里。
- 高德个人 Key 的配额和条款以高德开放平台为准；本项目仅供个人学习/非商业使用。

## 目录

```
app/src/main/java/com/example/navbridge/
  App.kt             高德隐私声明 + Key 注入
  Keys.kt            Key 保存/注入、签名 SHA1
  MainActivity.kt    UI + 主流程
  NavEngine.kt       算路/导航/回调/TTS
  SearchEngine.kt    Inputtips 目的地联想 + Geocode 兜底
  MapCapture.kt      离屏高德地图截图
  RasterCodec.kt     压色/RLE/路线编码/预览
  TiandituTiles.kt   天地图 vec_w 瓦片抓取/代理回退/拼接
  CoordConv.kt       WGS-84 ↔ GCJ-02
  GeoFrame.kt        经纬度 ↔ 本地米制坐标
  BleClient.kt       BLE 扫描/配对/分片发送
```
