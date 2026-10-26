# RaceBrick 导航图像拉取（当前实现）

- 固件仓库：<https://github.com/TJiangF/RaceBrick>（固件在 `firmware/`）
- 服务端仓库：<https://github.com/TJiangF/RaceBrick_Sat2Pixel>（接口文档见其 `docs/API.md`）
- 本文只描述**导航页拉取像素地图**的链路。

## 1. 相关文件

| 文件 | 作用 |
|---|---|
| `firmware/src/nav.c` / `nav.h` | 后台拉图任务、请求调度、pal16 解码、双缓冲 |
| `firmware/src/screens.c` | 导航页渲染、PUSH 菜单（Refresh / Style / Zoom / Fullscreen） |
| `firmware/src/netcfg.c` / `.h` | WiFi/HTTP 服务、调试接口 |
| `firmware/src/main.c` | 主循环、远程命令 |
| `firmware/src/config.h` | 屏幕几何（320×240） |

## 2. 数据流

```
GPS(UART NMEA) → gps_state{lat,lon,course,speed,fix}
        │  (nav_task 每 ~80–100 ms 读一次)
        ▼
 航向滤波 → 是否取新图 → POST /api/v1/nav
        → pal16 解码到 s_back → 加锁交换 s_front/s_back
        ▼
 主循环 render_nav(): gfx_blit（240×240 或全屏 320×240）+ 中心箭头 + 速度/状态 → lcd_flush
```

- 仅在导航页激活时工作（`nav_set_active(true)`）；无 WiFi → 状态 4，无 GPS（`fix_type<1`）→ 状态 5。
- 普通导航上屏 **240×240**（`NAV_IMG_SIZE`）；FULLSCREEN 模式请求并显示 **320×240**。
- `zoom` 只改变**地面覆盖范围**（`size_m / zoom`），不改变像素尺寸。
- 两张 **320×240 RGB565（153,600 B/张）** 缓冲（普通模式只用左上 240×240）+ **96 KB** 响应体，全部在 PSRAM。

### 导航页 UI

- 普通模式：左侧 240×240 地图；速度（亮绿色）在右上角；GPS/地图状态在右下角。
- 全屏模式（PUSH → FULLSCREEN）：320×240 地图铺满整页；速度仍在右上角，其余状态用白底胶囊框叠在右下角。
- WiFi 状态并入右下角电池胶囊左侧：绿色信号格=已连接，红色=未连接。
- 不再显示 HDG、PPS、zoom 数值、独立 WIFI/MAP OK 行；保留 GPS 质量与地图新鲜度。
- 相关实现：`screens.c` `render_nav()`、`app.c` `render_battery()`。

## 3. 请求

`POST http://47.100.182.210:2020/api/v1/nav`，`Content-Type: application/json`

```json
{
  "device_id": "esp32s3-0E259C",
  "seq": 3940360031,
  "lng": 121.4337500, "lat": 31.1987500,
  "size_m": 400, "heading": 90.0, "size": 240,
  "width": 240, "height": 240, "zoom": 1.50,
  "format": "pal16", "coord": "wgs84",
  "mode": "road", "theme": "gameboy"
}
```

| 字段 | 说明 |
|---|---|
| `device_id` | 由 MAC 派生（每台唯一） |
| `seq` | 启动用 `esp_random()` 作基址；**新请求 +1，重试复用同一 seq** |
| `size` / `width` / `height` | 普通 240×240；全屏 320×240。`size_m` 为基准地面宽度 |
| `zoom` | 缩放倍率；设备范围 **0.5–4.0，步进 0.5**，`>1` 放大 |
| `heading` | 平滑后的航向；服务端按此旋转成 heading-up |
| `format` | 固定 `pal16` |
| `coord` | 固定 `wgs84`（服务端自行换算 GCJ-02） |

`mode`（设备端用）：

| mode | 参数 | 输出 |
|---|---|---|
| `road` | `theme`（默认 `gameboy`） | 像素风路网，palCnt=4 |
| `satellite` | — | 卫星原图，palCnt=16 |
| `blend` | `sat_opacity=30/60/90` | 卫星按透明度叠加，palCnt=16 |

> 设备不再发送 `layer/style/process`。切模式、缩放、手动刷新都会强制立即取一帧；
> 缩放只更新 `zoom` 并递增 `seq`，服务端按新倍率重新渲染。

## 4. 响应（pal16）

小端：

```
[w u16][h u16][palCnt u16][palCnt × RGB565][RLE...]
```

RLE：`byte = (run-1)<<4 | idx`；当 `(byte>>4)==15` 时，下一字节 `run = 16 + next`。

服务端响应头新增：`X-Nav-Zoom-Factor`（请求倍率）、`X-Nav-Span-M`（实际地面宽度）；
解码应以 `X-Nav-Width/Height` 或包头 `w/h` 为准；设备支持 240×240 与 320×240。

解码校验：`w ≤ 320`、`h ≤ 240`、`palCnt ≤ 16`、解完必须正好 `w*h` 像素，否则判解码失败。

实测大小：road **877 B**/帧；satellite/blend（240×240）**约 37.4–37.9 KB**/帧。
全屏 320×240 约 1.33× 面积，卫星包预计约 50 KB（在 96 KB 上限内）。

## 5. 调度、限流与容错

| 项 | 当前值 |
|---|---|
| 最小请求间隔 | road **1.2 s**、satellite/blend **2.2 s**（**从请求开始算**，传输耗时不再拉长周期） |
| 立即刷新 | PUSH→REFRESH、切模式、切 zoom、进页（`s_force`） |
| 触发新图 | 位移 >12 m、航向变化 >12°、距上次 >2.5 s |
| 单次重试 | 最多 3 次（退避 1/2 s） |
| 5xx/429 | **不重试**，直接进入失败退避 |
| 失败退避 | 连续失败 5/10/20/30 s（上限 30 s），成功清零 |
| 航向 | <1.5 km/h 保持上次；从未移动→0°（正北朝上）；运动时 ~0.35 s 低通 |

状态码：`0` 加载、`1` OK、`2` 取图失败（界面显示 `HTTP xxx`）、`3` 解码失败、`4` 无 WiFi、`5` 无 GPS。失败时保留上一帧。

## 6. 传输稳定性优化

- **HTTP keep-alive**：`esp_http_client` 句柄只创建一次，后续帧复用同一条 TCP 连接，省掉每帧握手/慢启动，显著降低请求耗时抖动。
- **WiFi 低延迟**：进入导航页时 `WIFI_PS_NONE`，退出后恢复 `WIFI_PS_MIN_MODEM`，减少 modem sleep 引入的几十~几百 ms 抖动。
- **从请求开始限流**：帧周期不再被上一帧传输时间叠加，帧间隔更稳定。
- **等待对齐**：等待下一帧时按剩余时间休眠（10–50 ms），不再固定 80 ms 空转。
- **服务端瓦片缓存 600 s**：连续移动/缩放时复用上游瓦片，减少渲染耗时波动。
- **服务端拉图上限**：源图最多 1024 px，超大 `size` 不会拖垮服务端。
- 双缓冲 + 最新帧覆盖旧帧，不排队，天然丢弃过期帧。

## 7. 性能参考（240×240 satellite，优化前实测）

- 请求（含服务端渲染+传输）：**496–843 ms**
- 解码：**9–17 ms**（可忽略）
- 优化前帧间隔：**3.05–3.4 s**（2.2 s 限流 + 0.6 s 请求 + 80 ms 循环）
- 优化后周期从请求开始计算，理论上限约为 **2.2–2.4 s**/帧（受服务端渲染与配额限制）

瓶颈：① 设备侧限流（主导）② 服务端渲染 + 天地图取图 ③ 37 KB WiFi 传输。放开限流理论上限约 **1–2 fps**，再快受服务端与配额限制。

## 8. 服务端注意点

- 幂等缓存 key：`(device_id, seq, 请求指纹)`，TTL 60 s + 有界 LRU。
  - 同 seq + 同位置 → 命中、字节一致；同 seq + 新位置 → 不命中（已验证）。
  - 指纹已含 `size`、`zoom`、模式解析后的 `layer/style/theme/sat_opacity`。
- 服务端瓦片缓存 600 s（按 layer/zoom/x/y），连续移动/缩放显著减少上游请求。
- **road/blend 依赖天地图**：`302010 该tk已限流` → 服务端 502 `TILE_FETCH_FAILED`。个人 key 约 **1 万次/天**，企业 300 万/天；satellite 用另一路瓦片，通常正常。
- 设备目前只显示 `HTTP 502`，未解析错误码，也不会自动降级到 satellite。

## 9. 调试 / 验证

- `GET /api/fb?f=1` → 153,600 B 原生 RGB565 帧缓冲（冻结渲染，防撕裂）；`/api/fb?f=0` 解冻。
- `GET /api/goto?n=<scid>&e=1` 跳页（`SC_NAV=23`，枚举见 `app.h`）。
- `GET /api/key?b=up|down|push|back`：注入一次短按，用于无实体按键的自动化验证。
- `GET /api/gps?lat=&lon=&speed=&course=`：临时覆盖 GNSS 状态（真实 GNSS 会覆盖，测试时需连续调用）。
- `POST /api/control`：`cmd=home|perf|race|debug|nav`。
- `GET /api/status` 额外返回 `nav`（状态码）、`nav_age`、`nav_frames`、`nav_bytes`。
- 串口：`frame N: 320x240, <bytes>, mode=x/y z=<zoom> full=1, req=<ms> dec=<ms>, heap <n>`；失败：`fetch failed (HTTP <n>, heap <n>)`。
- 直接验证服务端：
```bash
curl -s -X POST http://47.100.182.210:2020/api/v1/nav \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"probe","seq":1,"lng":121.43375,"lat":31.19875,
       "size_m":400,"width":240,"height":240,"zoom":1.0,"format":"pal16",
       "mode":"satellite","coord":"wgs84"}' -D - -o /tmp/nav.bin
```

## 10. 常改位置

| 需求 | 位置 |
|---|---|
| 图幅/像素 | `nav.h` `NAV_IMG_SIZE` / `NAV_FULL_W` / `NAV_FULL_H`；`nav.c` `NAV_SIZE_M` |
| 全屏开关/持久化 | `nav.c` `nav_set_fullscreen()`；`app.c` NVS key `navfull` |
| 缩放范围/步进 | `nav.h` `NAV_ZOOM_MIN_PCT` / `MAX` / `STEP` |
| 缩放持久化 | `app.c` NVS key `navzoom` |
| 响应体上限 | `nav.c` `NAV_BODY_MAX`（现 72 KB） |
| 限流/触发/退避 | `nav.c` `nav_task()` |
| 请求 JSON / mode / zoom | `nav.c` `nav_task()` 内 `modejson`、`s_zoom` |
| HTTP keep-alive / 读取 | `nav.c` `http_fetch()` |
| 低延迟 WiFi | `nav.c` `nav_set_active()`；`netcfg.c` `netcfg_set_low_latency()` |
| URL | `nav.c` `NAV_URL` |
| 解码 | `nav.c` `pal16_decode()` |
| 导航页布局/菜单/缩放 | `screens.c` `render_nav()` / `draw_nav_menu()` / `nav_apply_zoom()` |
| WiFi/电池指示 | `app.c` `render_battery()` |
| 调试接口 | `netcfg.c` `h_api_fb` / `h_api_goto` |
