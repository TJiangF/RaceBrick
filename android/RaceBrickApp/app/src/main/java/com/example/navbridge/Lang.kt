package com.example.navbridge

import android.app.Dialog
import android.content.Context
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView

object Lang {
    private const val PREFS = "racebridge"
    private const val KEY_ENGLISH = "ui_english"

    @Volatile var isEnglish: Boolean = false
        private set

    fun init(context: Context) {
        isEnglish = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENGLISH, false)
    }

    fun setEnglish(context: Context, english: Boolean) {
        isEnglish = english
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_ENGLISH, english).commit()
    }

    private val exact = mapOf(
        "返回" to "Back",
        "编辑" to "Edit",
        "删除" to "Delete",
        "详情" to "Details",
        "赛道" to "Tracks",
        "设备未连接" to "Device not connected",
        "设备地址" to "Device address",
        "直连 ESP WiFi" to "Direct ESP WiFi",
        "断开 ESP 直连" to "Disconnect ESP WiFi",
        "连接并刷新" to "Connect & Refresh",
        "已连接设备" to "Device connected",
        "输入目的地，例如：天安门" to "Enter destination, e.g. Tiananmen",
        "尚未选择目的地" to "No destination selected",
        "开始导航" to "Start navigation",
        "停止" to "Stop",
        "模拟导航（当前位置 → 目的地）" to "Simulate route (current → destination)",
        "工具：汽车" to "Vehicle: Car",
        "风格：掌机像素" to "Style: Pixel",
        "320×240 导航画面模拟预览" to "320×240 navigation preview",
        "预览：SDK 原图" to "Preview: SDK original",
        "视角：车头朝上" to "Heading: Up",
        "未连接 RaceBrick" to "RaceBrick not connected",
        "连接 RaceBrick" to "Connect RaceBrick",
        "连接成功后进入导航、媒体与设备设置" to "Connect to open navigation, media and device settings",
        "RaceBrick 连接状态" to "RaceBrick connection",
        "ESP 与手机在同一 WiFi 时填 ESP 的局域网 IP；AP 模式默认 192.168.4.1。"
            to "Enter the ESP LAN IP when both are on the same WiFi; AP mode defaults to 192.168.4.1.",
        "点按后系统会连接 RaceBrick-XXXX（密码 racebrick）；直连期间手机可能没有互联网，离开 RACE 会自动断开。"
            to "The system connects to RaceBrick-XXXX (password: racebrick). Internet may be unavailable while connected and is released when leaving RACE.",
        "高德 Android Key" to "AMap Android Key",
        "粘贴高德 Key" to "Paste AMap Key",
        "保存 Key 并重启 App" to "Save Key & Restart App",
        "Race 数据连接" to "Race Data Connection",
        "保存设备地址" to "Save Device Address",
        "同一 WiFi 时填 ESP 局域网 IP；ESP AP 模式默认 192.168.4.1。"
            to "Enter the ESP LAN IP on the same WiFi; AP mode defaults to 192.168.4.1.",
        "媒体控制" to "Media Control",
        "首次启动会自动打开系统“通知使用权”设置；开启后 ESP 的 MEDIA 页才能控制手机音乐。"
            to "The system notification-access page opens on first launch. Enable it so the ESP MEDIA page can control phone music.",
        "后台运行" to "Background",
        "开始导航后 App 会启动前台服务。为保证切换 App 后仍持续定位/渲染/蓝牙推流，请在应用详情里把定位设为“始终允许”，电池设为“不受限制”。"
            to "A foreground service keeps location, rendering and BLE streaming running. Set location to Always allow and battery to Unrestricted.",
        "打开应用权限/电池设置" to "Open App Permissions / Battery",
        "整帧发送始终开启：RaceBrick 使用 320×240 整帧；旧 MotoNav 底图/路线分包模式已不再暴露。"
            to "Full-frame streaming is always enabled: RaceBrick uses 320×240 frames. Legacy MotoNav chunked map/route mode is no longer exposed.",
        "语言 / Language" to "Language / 语言",
        "当前语言：中文" to "Language: Chinese",
        "当前语言：英文" to "Language: English",
        "当前语言：英文" to "Language: English",
        "设备地址已保存" to "Device address saved",
        "Key 不能为空" to "Key cannot be empty",
        "保存失败" to "Save failed",
        "已保存，正在重启" to "Saved, restarting",
        "通知使用权：已授权（ESP 可控制音乐）" to "Notification access: granted (ESP can control music)",
        "通知使用权：未授权（请在系统设置中开启）" to "Notification access: not granted (enable it in system settings)",
        "ESP WiFi 配网" to "ESP WiFi Setup",
        "正在读取设备 WiFi 状态…" to "Reading device WiFi status…",
        "扫描附近网络" to "Scan nearby networks",
        "已保存网络" to "Saved networks",
        "刷新状态" to "Refresh status",
        "WiFi 名称（SSID）" to "WiFi name (SSID)",
        "WiFi 密码（开放网络留空）" to "WiFi password (leave empty if open)",
        "连接并保存到 ESP" to "Connect and save to ESP",
        "请输入 WiFi 名称" to "Enter a WiFi name",
        "ESP 正在扫描附近 WiFi…" to "ESP is scanning nearby WiFi…",
        "读取 ESP 已保存的网络…" to "Reading saved networks from ESP…",
        "未知状态" to "Unknown state",
        "清除" to "Clear",
        "测试" to "Test",
        "完成" to "Done",
        "赛道轨迹 / 起始线预览" to "Track path / start line preview",
        "起始线位置" to "Start line position",
        "起始线宽度" to "Start line width",
        "起点左右偏移（负=左，正=右）" to "Start line offset (negative=left, positive=right)",
        "反向起点方向（CCW）" to "Reverse start direction (CCW)",
        "保存后会写入 ESP，并立即用于下一场计圈。" to "Saved to ESP and used for the next race.",
        "编辑赛道" to "Edit Track",
        "赛道轨迹尚未加载" to "Track geometry is not loaded",
        "点击预览可查看卫星图叠加；同步后设备端同一赛道也会显示。"
            to "Tap the preview to see the satellite overlay; syncing also shows it on the device.",
        "该赛道没有 GPS 基准，或卫星图加载失败。" to "This track has no GPS reference or the satellite image failed to load.",
        "赛道卫星预览" to "Track Satellite Preview",
        "同步到设备" to "Sync to Device",
        "同步卫星图到设备" to "Sync Satellite to Device",
        "没有可同步的卫星图" to "No satellite image to sync",
        "正在同步卫星图到设备…" to "Syncing satellite image to device…",
        "卫星图已同步到设备" to "Satellite image synced to device",
        "删除赛道" to "Delete Track",
        "正在删除赛道" to "Deleting track",
        "已删除赛道" to "Track deleted",
        "没有可删除的记录" to "No records to delete",
        "清空该赛道 Race Log" to "Clear Track Race Log",
        "删除场次" to "Delete Session",
        "已删除场次" to "Session deleted",
        "删除单圈" to "Delete Lap",
        "正在下载 CSV…" to "Downloading CSV…",
        "导出到 RaceChrono / 文件" to "Export to RaceChrono / File",
        "时间未同步" to "Time not synchronized",
        "Race Log 详情" to "Race Log Details",
        "圈速详情" to "Lap Details",
        "关联到赛道" to "Associate with Track",
        "没有可关联的赛道" to "No track available for association",
        "把 Race Log 关联到赛道" to "Associate Race Log with Track",
        "显示全部" to "Show all",
        "没有有效圈速。" to "No valid lap times.",
        "无最佳圈" to "No best lap",
        "有 GPS 基准" to "GPS referenced",
        "无 GPS 基准" to "No GPS reference",
        "无 GPS 基准赛道" to "No GPS-referenced tracks",
        "赛道详情读取失败，点“编辑”重试" to "Failed to read track details. Tap Edit to retry.",
        "该赛道缺少 GPS 基准，无法生成卫星图" to "This track has no GPS reference; satellite imagery is unavailable.",
        "卫星图未加载，点地图或编辑重试" to "Satellite image not loaded. Tap the map or Edit to retry.",
        "卫星图请求失败" to "Satellite request failed",
        "卫星图解码失败" to "Satellite decode failed",
        "直连 ESP 时没有可用的互联网网络" to "No internet network available while directly connected to ESP",
        "删除这一圈" to "Delete This Lap",
        "可勾选单圈或多圈叠加；单选时显示加减速颜色和最高速位置。"
            to "Select one or more laps; single-lap mode shows acceleration colors and the maximum-speed point.",
        "绿=加速  红=减速  黄点=最高速" to "Green=accelerate  Red=brake  Yellow=max speed",
        "删除这整场原始数据？此操作不可恢复。" to "Delete this entire raw session? This cannot be undone.",
        "删除赛道几何？已保存场次不会删除。" to "Delete the track geometry? Saved sessions are kept.",
        "请先连接 RaceBrick 蓝牙" to "Connect RaceBrick via Bluetooth first",
        "请先在设置里填写高德 Key" to "Set the AMap Key in Settings first",
        "模拟导航中" to "Simulating navigation",
        "导航中" to "Navigating",
        "已停止导航" to "Navigation stopped",
        "已到达目的地" to "Destination reached",
        "已到达" to "Arrived",
        "请开启通知使用权，ESP 才能控制其他音乐 App" to "Enable notification access so the ESP can control other music apps.",
        "请开启 NavBridge 的通知使用权，ESP 才能控制手机音乐" to "Enable notification access for NavBridge so the ESP can control phone music.",
        "音乐控制失败，请开启通知使用权" to "Music control failed. Enable notification access.",
        "未连接 RaceBrick：先手机端模拟，连接后自动补发" to "RaceBrick not connected: simulating on phone; sending automatically after connection.",
        "没有 Race Log。设备端进入 RACE 并在 GPS 有定位后开始比赛，结束后会自动保存完整记录。"
            to "No Race Log. Start a race on the device with GPS available; the complete session is saved automatically.",
        "这条赛道还没有 Race Log。" to "This track has no Race Log yet.",
        "没有赛道。设备首次启动会写入一条模拟赛道；也可以先在设备端保存赛道。"
            to "No tracks. A sample track is written on first boot; you can also save a track on the device.",
        "该 Race Log 没有保存圈速。" to "This Race Log has no saved lap times.",
        "筛选：" to "Filter: ",
        "已删除圈：" to "Deleted laps: ",
        "赛车" to "Race",
        "汽车" to "Car",
        "摩托车" to "Motorcycle",
        "自行车" to "Bicycle",
        "掌机像素" to "Handheld Pixel",
        "极简黑白" to "Minimal Mono",
        "罪恶都市" to "Vice",
        "原版" to "Original",
    )

    private val phrases: List<Pair<String, String>> = listOf(
        "正在读取赛道、Race Log 和原始场次，请稍候…" to "Reading tracks, Race Logs and raw sessions…",
        "正在从设备删除" to "Deleting from device: ",
        "正在同步设备数据" to "Syncing device data",
        "正在读取赛道和场次…" to "Reading tracks and sessions…",
        "正在读取赛道轨迹…" to "Reading track geometry…",
        "正在读取 Race Log 轨迹…" to "Reading Race Log trace…",
        "正在读取 Race Log 详情…" to "Reading Race Log details…",
        "正在读取设备 WiFi 状态…" to "Reading device WiFi status…",
        "正在请求直连 RaceBrick-XXXX…" to "Requesting direct RaceBrick-XXXX connection…",
        "正在保存赛道设置…" to "Saving track settings…",
        "正在请求 ESP 连接" to "Requesting ESP connection ",
        "正在重新连接…" to "Reconnecting…",
        "正在搜索 RaceBrick…" to "Searching for RaceBrick…",
        "正在定位…" to "Locating…",
        "正在取底图…" to "Fetching base map…",
        "正在解析" to "Resolving ",
        "正在规划路线…" to "Calculating route…",
        "读取轨迹失败：" to "Failed to read track: ",
        "读取详情失败：" to "Failed to read details: ",
        "连接失败：" to "Connection failed: ",
        "保存失败：" to "Save failed: ",
        "删除失败：" to "Delete failed: ",
        "同步失败：" to "Sync failed: ",
        "导出失败：" to "Export failed: ",
        "关联失败：" to "Association failed: ",
        "赛道设置已保存并同步" to "Track settings saved and synchronized",
        "设备未确认新名称" to "The device did not confirm the new name",
        "请填写名称" to "Please enter a name",
        "没有赛道。" to "No tracks. ",
        "没有 Race Log。" to "No Race Log. ",
        "没有 Race Log 记录。" to "No Race Log records. ",
        "这条赛道还没有 Race Log。" to "This track has no Race Log yet.",
        "没有可用的轨迹数据" to "No trace data available",
        "卫星图覆盖" to "Satellite covers ",
        "透明度 50%" to "50% opacity",
        "正在加载卫星图" to "Loading satellite",
        "已读取" to "Loaded ",
        "正在读取圈轨迹" to "Loading lap traces",
        "正在读取赛道几何" to "Loading track geometry",
        "赛道 " to "Tracks ",
        "Race Log " to "Race Logs ",
        "卫星图 " to "Satellite ",
        "起始线" to "start line",
        "起点" to "start",
        "有 GPS 基准" to "GPS referenced",
        "无 GPS 基准" to "no GPS reference",
        "圈  ·  " to " laps  ·  ",
        "已删除" to "deleted",
        "最佳" to "best",
        "距离" to "Distance",
        "均速" to "Avg",
        "最高" to "Max",
        "删除" to "Delete",
        "通知使用权：" to "Notification access: ",
        "工具：" to "Vehicle: ",
        "风格：" to "Style: ",
        "地图风格：" to "Map style: ",
        "出行工具：" to "Vehicle: ",
        "预览：" to "Preview: ",
        "视角：" to "Heading: ",
        "目的地：" to "Destination: ",
        "当前语言：" to "Language: ",
        "包名：" to "Package: ",
        "圈速" to "lap time",
        "最佳圈" to "best lap",
    ).sortedByDescending { it.first.length }

    private val patterns = listOf(
        Regex("^赛道 (\\d+) 条，Race Log (\\d+) 条，卫星图 (\\d+)/(\\d+) 张(.*)$") to
            "Tracks $1, Race Logs $2, Satellite $3/$4$5",
        Regex("^正在加载卫星图 (\\d+)/(\\d+)$") to "Loading satellite $1/$2",
        Regex("^已读取 (\\d+) 条赛道$") to "Loaded $1 tracks",
        Regex("^已读取 (\\d+) 条 Race Log$") to "Loaded $1 session(s)",
        Regex("^正在读取圈轨迹 (\\d+)/(\\d+)$") to "Loading lap traces $1/$2",
        Regex("^正在读取赛道几何 (\\d+)/(\\d+)$") to "Loading track geometry $1/$2",
        Regex("^第 (\\d+) 圈(.*)$") to "Lap $1$2",
        Regex("^L(\\d+) 单选：该圈没有速度数据$") to "L$1: no speed data",
        Regex("^L(\\d+) 单选：绿=加速，红=减速，黄点=最高速 (\\d+) km/h$") to
            "L$1: green=accel, red=brake, yellow=max $2 km/h",
        Regex("^删除 (\\d+) 条记录？此操作不可恢复。$") to "Delete $1 record(s)? This cannot be undone.",
        Regex("^已清空 (\\d+) 条 Race Log$") to "Cleared $1 Race Log(s)",
        Regex("^删除「(.*)」？赛道几何会被清空，已保存场次不会删除。$") to
            "Delete “$1”? Track geometry will be cleared; saved sessions are kept.",
        Regex("^正在从设备删除「(.*)」…$") to "Deleting “$1” from device…",
        Regex("^卫星图已同步：(.*)$") to "Satellite synced: $1",
        Regex("^已生成 (.*)$") to "Created $1",
        Regex("^设备地址已保存：(.*)$") to "Device address saved: $1",
        Regex("^已保存：(.*)$") to "Saved: $1",
    )

    fun tr(input: String): String {
        if (!isEnglish || input.isEmpty()) return input
        exact[input]?.let { return it }
        for ((re, replacement) in patterns) {
            if (re.matches(input)) return re.replace(input, replacement)
        }
        var out = input
        for ((cn, en) in phrases) if (out.contains(cn)) out = out.replace(cn, en)
        return out
    }

    fun localizeTree(root: View?) {
        if (!isEnglish || root == null) return
        when (root) {
            is EditText -> {
                val hint = root.hint?.toString()
                if (!hint.isNullOrEmpty()) {
                    val t = tr(hint)
                    if (t != hint) root.hint = t
                }
            }
            is TextView -> {
                val text = root.text?.toString() ?: ""
                if (text.isNotEmpty() && text.any { it in '\u4e00'..'\u9fff' }) {
                    val t = tr(text)
                    if (t != text) root.text = t
                }
            }
        }
        if (root is ViewGroup) {
            for (i in 0 until root.childCount) localizeTree(root.getChildAt(i))
        }
    }

    fun localizeDialog(dialog: Dialog?) {
        if (!isEnglish || dialog == null) return
        runCatching { localizeTree(dialog.window?.decorView) }
    }
}
