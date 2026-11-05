# Race 板块圈速功能实现逻辑

> 对应固件当前 Race 菜单：`START RACE`、`RACE LOG`、`TRACKS`、`PERFORMANCE`。
> 核心代码：`firmware/src/sim.c`、`firmware/src/screens.c`、`firmware/src/app.c`、`firmware/src/tstore.c`。

## 1. 入口结构

- `START RACE` → `SC_MODE_SELECT`
  - 新赛道：`SC_NEW_TRACK_MARK_START`
  - 已记录赛道：`SC_TRACK_LIST`
- `RACE LOG` → `SC_HISTORY` → `SC_HISTORY_DETAIL`
- `TRACKS` → `SC_TRACK_EDIT`
- `PERFORMANCE` → `SC_PERF`

`PERFORMANCE` 是独立的 0-100 / 100-0 测试，不参与圈速记录。

## 2. 赛道数据模型

`track_t`：

- 固定 `TRK_N = 360` 个归一化闭环点。
- `lengthM`：实际赛道周长。
- `curvNorm[]`：曲率。
- `speedFactor[]`：每个采样点的目标速度系数。
- `sections[MAX_SECTIONS]`：分段比例。
- `geo`：是否为真实 GPS 记录的赛道。
- `glat0/glon0`：GPS/米制坐标原点。
- `nscale/nox/noy`：米制坐标 → 归一化坐标的变换。
- `line_lat/line_lon/line_hdg`：可编辑的起终线位置与方向。
- `store_slot`：持久化槽位。

`lap_t`：

- `time`：本圈时间。
- `times[TRK_N]`：每个赛道采样点对应的时间。
- `trace[TRACE_MAX]`：融合轨迹；`TRACE_MAX = 160`。
- `gps[GPS_MAX]`：原始 GPS 轨迹；`GPS_MAX = 80`。
- `valid`：模拟圈要求 `> 1s`；真实 GNSS 圈要求 `> 2s`。

## 3. 模拟圈速核心

`engine_step()`：

- 每步最大 30ms；主循环把一帧时间拆成多个子步。
- 当前进度 `s ∈ [0, N)`。
- 目标速度 = `speedFactor[idx] * vTop`。
- 按 `accel/brake` 限制速度变化。
- `s += v * dt`。
- 跨越整数采样点时写入 `currentTimes[idx] = elapsed`。
- 每 3 个采样点记录一次轨迹到 `lapTrace`。
- `s >= N` 时完成一圈。

`engine_complete_lap()`：

- `engine_fill_times()` 对缺失的 `currentTimes` 做线性插值。
- 保存本圈时间、逐点时间、轨迹、GPS 点。
- 更新最佳圈：`hasBestLap`、`bestTime`、`bestIndex`。
- 清空本圈状态，进入下一圈。

实时差值：

- `engine_compute_diff()` 用当前 `s` 对应的最佳圈时间计算 `elapsed - best`。
- 结果驱动比赛页面的 LED 差值和 G-G 图相关状态。

## 4. 真实 GNSS 圈速核心

`engine_update_gnss()`：

- GPS → 米制坐标 → 归一化赛道坐标。
- 首次直接全局投影。
- 之后只在上一位置 ±`N/8` 范围内投影，避免自交赛道跳段。
- 离赛道超过 25m 的样本直接丢弃。
- 速度使用 GPS 地速换算。
- 起终线判定：
  - 使用可编辑的 `line_lat/line_lon/line_hdg`。
  - 计算沿线方向 `along` 和横向偏差 `latd`。
  - 跑过半圈后 `lapArmed = true`。
  - 必须满足 `alongPrev < 0 && along >= 0`。
  - 横向偏差 `< 3m`。
  - 本圈时间 `> 2s`。
  - 才调用 `engine_gnss_lap()`。
- 真实圈会按赛道采样点记录 `currentTimes`；完成圈时 `engine_fill_times()` 补齐并复制到 `lap->times`，因此最佳圈逐点时间和分段差值可用。
- 每个 GNSS 更新都会调用 `engine_compute_diff()`；第二圈开始即可用最佳圈的同样采样点时间计算实时 delta，驱动 Race 页面红/绿 LED。

## 5. 新赛道录制

`SC_NEW_TRACK_MARK_START`：

- 必须有 3D fix。
- 第一次有效定位作为原点。
- 移动累计 ≥2m 才记录一个点，最多 `REC_MAX = 400` 点。
- 用前 6 个点估算起跑方向。
- 离开起点超过 20m 后武装起终线。
- 再次正向穿过起终线、横向误差 <3m、圈时 >2s：
  - 用当前点构建 `sessionTrack`。
  - `rec_laps++`。
  - 重新开始下一点集。
- 结束时：
  - 有完整圈：使用最后一圈构建赛道。
  - 没有完整圈：至少 20 点且累计 20m 也能构建。
  - 进入赛道命名。

`track_build_geo()`：

- 按闭环重采样到 360 点。
- 归一化坐标。
- 计算周长、曲率、速度系数。
- 把起点设为默认起终线。

## 6. 已记录赛道流程

`SC_TRACK_LIST` 选赛道后：

- `geo == true` → 先进入 `SC_TRACK_START` 调整起终线。
- 否则 → 进入 `SC_MOUNT_SELECT`。
- `SC_MOUNT_SELECT` 设置固定安装 / 转向安装，更新融合参数。
- 进入 `SC_RACING_RECORDED`。

`SC_TRACK_START`：

- 左右键按赛道比例移动起终线。
- PUSH 保存 `line_lat/line_lon/line_hdg`。
- 然后进入安装方式选择。

## 7. 圈速选择 / 赛道平均

`SC_LAP_SELECT`：

- 至少选中 2 圈。
- 每圈轨迹按 120 点等距重采样后求平均。
- 用平均轨迹重建赛道。
- 然后询问是否标记分段。

## 8. 分段计时

`SC_SECTION_MARKING`：

- `sec_pos` 从 3% 开始，左右键调整，长按加速。
- PUSH 设置候选分段点。
- 双击 PUSH 提交，必须大于上一段。
- 最后追加 100% 作为终点。
- `engine_section_deltas()` 用当前时间与最佳圈同采样点时间比较，得到每段差值。

## 9. Race Log

`app_record_race()`：

- 比赛结束时复制赛道名、时间、圈数、每圈时间、最佳圈、分段、最佳轨迹到 `histrec_t`。
- `HISTORY_RECORDS = 5`。
- `SC_HISTORY` 列表 → `SC_HISTORY_DETAIL` 查看轨迹和圈速。

当前 Race Log 是运行期记录/演示种子，不会像赛道一样持久化；赛道本身通过 `tstore` 保存。

## 10. 当前限制

- 圈数上限：`MAX_LAPS = 8`。
- 新赛道录制最多 `REC_MAX = 400` 点，约 800m。
- 赛道存储 `TSTORE_SLOTS = 5`，当前使用内部 flash。
- 外置 W25Q128 已能探测，但尚未用于赛道存储。
- 模拟赛道走 `engine_update()`；真实 GNSS 赛道走 `engine_update_gnss()`。

## 11. 关键代码位置

| 逻辑 | 位置 |
|---|---|
| 赛道构建/投影 | `firmware/src/sim.c: track_build_geo/track_project` |
| 模拟圈速 | `firmware/src/sim.c: engine_step/engine_complete_lap` |
| 真实 GNSS 圈速 | `firmware/src/sim.c: engine_update_gnss` |
| 分段差值 | `firmware/src/sim.c: engine_section_deltas` |
| 新赛道录制 | `firmware/src/screens.c: SC_NEW_TRACK_MARK_START` |
| 起终线编辑 | `firmware/src/screens.c: SC_TRACK_START` |
| 圈速选择 | `firmware/src/screens.c: SC_LAP_SELECT` |
| 分段标记 | `firmware/src/screens.c: SC_SECTION_MARKING` |
| Race Log | `firmware/src/app.c: app_record_race` |
| 赛道持久化 | `firmware/src/tstore.c` |
