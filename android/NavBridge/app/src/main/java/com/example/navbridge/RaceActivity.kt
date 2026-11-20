package com.example.navbridge

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.graphics.Typeface
import android.text.InputFilter
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.CheckBox
import android.widget.HorizontalScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class RaceActivity : AppCompatActivity() {
    private lateinit var tvStatus: TextView
    private lateinit var etUrl: EditText
    private lateinit var llList: LinearLayout

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var direct: WifiDirectConnector
    private var generation = 0

    private var tracks: List<RaceTrack> = emptyList()
    private var sessions: List<RaceSession> = emptyList()
    private var logs: List<RaceLogEntry> = emptyList()
    private var filterTrack: Int? = null
    private var logFilterName: String? = null
    private var showingSessions = false
    private var showingLogs = false
    private var directPending = false
    private var awaitingWriteSettings = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_race)

        tvStatus = findViewById(R.id.tvRaceStatus)
        etUrl = findViewById(R.id.etRaceUrl)
        llList = findViewById(R.id.llRaceList)
        direct = WifiDirectConnector(this)
        etUrl.setText(RaceApi.baseUrl(this))

        findViewById<Button>(R.id.btnRaceNav).setOnClickListener { finish() }
        findViewById<Button>(R.id.btnRaceSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.btnRaceDirect).setOnClickListener { directWithPermission() }
        findViewById<Button>(R.id.btnRaceLoad).setOnClickListener { loadAll() }
        findViewById<Button>(R.id.btnRaceTracks).setOnClickListener {
            showingSessions = false
            showingLogs = false
            logFilterName = null
            renderTracks()
        }
        findViewById<Button>(R.id.btnRaceSessions).setOnClickListener {
            showingSessions = true
            showingLogs = false
            renderSessions()
        }
        findViewById<Button>(R.id.btnRaceLogs).setOnClickListener {
            showingSessions = false
            showingLogs = true
            logFilterName = null
            renderLogs()
        }

        loadAll()
    }

    private fun directWithPermission() {
        if (!Settings.System.canWrite(this)) {
            awaitingWriteSettings = true
            status("直连需要“修改系统设置”权限，请在系统页允许后返回")
            val intent = Intent(
                Settings.ACTION_MANAGE_WRITE_SETTINGS,
                Uri.parse("package:$packageName"),
            )
            try {
                startActivity(intent)
            } catch (_: Exception) {
                startActivity(Intent(Settings.ACTION_SETTINGS))
            }
            return
        }
        val needed = ArrayList<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED
        ) needed += Manifest.permission.ACCESS_FINE_LOCATION
        if (android.os.Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.NEARBY_WIFI_DEVICES)
            != PackageManager.PERMISSION_GRANTED
        ) needed += Manifest.permission.NEARBY_WIFI_DEVICES
        if (needed.isEmpty()) {
            toggleDirect()
            return
        }
        directPending = true
        status("直连需要定位/附近 WiFi 权限")
        ActivityCompat.requestPermissions(this, needed.toTypedArray(), 101)
    }

    override fun onResume() {
        super.onResume()
        if (awaitingWriteSettings && Settings.System.canWrite(this)) {
            awaitingWriteSettings = false
            directWithPermission()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 101) return
        val granted = grantResults.isNotEmpty() &&
            grantResults.all { it == PackageManager.PERMISSION_GRANTED }
        if (granted && directPending) {
            directPending = false
            directWithPermission()
        } else {
            directPending = false
            fail("缺少定位/附近 WiFi 权限，无法直连")
        }
    }

    private fun toggleDirect() {
        val btn = findViewById<Button>(R.id.btnRaceDirect)
        if (direct.isConnected) {
            direct.disconnect()
            btn.text = "直连 ESP WiFi"
            status("已断开 ESP 直连")
            return
        }
        status("正在请求直连 RaceBrick-XXXX…")
        direct.connect { ok, message ->
            status(message)
            if (ok) {
                etUrl.setText(WifiDirectConnector.AP_URL)
                RaceApi.saveBaseUrl(this, WifiDirectConnector.AP_URL)
                btn.text = "断开 ESP 直连"
                loadAll()
            } else {
                btn.text = "直连 ESP WiFi"
            }
        }
    }

    private fun base(): String = RaceApi.saveBaseUrl(this, etUrl.text.toString())

    private fun loadAll() {
        direct.bindCurrentEsp()
        val gen = ++generation
        val b = base()
        status("正在读取赛道和场次…")
        worker.execute {
            runCatching {
                val t = RaceApi.tracks(b)
                val s = RaceApi.sessions(b)
                val l = runCatching { RaceApi.logs(b) }.getOrElse { emptyList() }
                Triple(t, s, l)
            }.onSuccess { (t, s, l) ->
                main.post {
                    if (gen != generation) return@post
                    tracks = t
                    sessions = s
                    logs = l
                    status("赛道 ${t.size} 条，场次 ${s.size} 场，Race Log ${l.size} 条")
                    when {
                        showingLogs -> renderLogs()
                        showingSessions -> renderSessions()
                        else -> renderTracks()
                    }
                }
            }.onFailure { e ->
                main.post {
                    if (gen != generation) return@post
                    fail("连接失败：${e.message}")
                    renderTracks()
                }
            }
        }
    }

    private fun renderTracks() {
        llList.removeAllViews()
        if (tracks.isEmpty()) {
            empty("没有赛道。设备首次启动会写入一条模拟赛道；也可以先在设备端保存赛道。")
            return
        }
        for (t in tracks) {
            val card = card()
            card.addView(title(t.name.ifBlank { "赛道 #${t.id}" }))
            card.addView(
                meta(
                    "${formatLength(t.lengthM)}  ·  宽 ${String.format(Locale.US, "%.1f", t.widthM)}m  ·  " +
                        "起点 ${String.format(Locale.US, "%.1f", t.startF * 100)}%  ·  " +
                        (if (t.reverse) "CCW" else "CW") + "  ·  " +
                        if (t.geo) "有 GPS 基准" else "无 GPS 基准"
                )
            )
            card.addView(
                row(
                    button("编辑") { editTrack(t) },
                    button("查看场次") {
                        filterTrack = t.id
                        showingSessions = true
                        showingLogs = false
                        renderSessions()
                    },
                    button("Race Log") {
                        logFilterName = t.name
                        showingSessions = false
                        showingLogs = true
                        renderLogs()
                    },
                    button("删除", danger = true) { confirmDeleteTrack(t) },
                )
            )
            llList.addView(card)
        }
    }

    private fun renderSessions() {
        llList.removeAllViews()
        val filter = filterTrack
        val visible = if (filter == null) sessions else sessions.filter { it.trackSlot == filter }
        if (filter != null) {
            val header = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val name = tracks.firstOrNull { it.id == filter }?.name ?: "赛道 #$filter"
            header.addView(
                TextView(this).apply {
                    text = "筛选：$name"
                    setTextColor(0xFFF2B95C.toInt())
                    textSize = 13f
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }
            )
            header.addView(
                button("显示全部") {
                    filterTrack = null
                    renderSessions()
                }
            )
            llList.addView(header)
        }
        if (visible.isEmpty()) {
            empty(
                if (filter == null) "没有比赛场次。设备端进入 RACE 并在 GPS 有定位后开始比赛，结束后会自动保存。"
                else "这条赛道还没有场次。"
            )
            return
        }
        for (s in visible.sortedByDescending { it.startEpoch }) {
            val card = card()
            card.addView(title(s.trackName.ifBlank { "赛道 #${s.trackSlot}" }))
            card.addView(
                meta(
                    "${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  " +
                        "${s.laps} 圈  ·  ${s.samples} 点"
                )
            )
            if (s.deletedLapMask != 0) {
                card.addView(meta("已删除圈：${deletedLaps(s)}", warning = true))
            }
            card.addView(
                row(
                    button("详情") { showSessionDetail(s) },
                    button("导出 RaceChrono") { exportSession(s) },
                    button("删除", danger = true) { confirmDeleteSession(s) },
                )
            )
            llList.addView(card)
        }
    }

    private fun renderLogs() {
        llList.removeAllViews()
        val filter = logFilterName
        val visible = if (filter == null) logs else logs.filter { it.trackName == filter }
        if (filter != null) {
            llList.addView(TextView(this).apply {
                text = "筛选：$filter"
                setTextColor(0xFFF2B95C.toInt())
                textSize = 13f
                setPadding(dp(4), 0, dp(4), dp(8))
            })
        }
        if (visible.isEmpty()) {
            empty("没有 Race Log 记录。RaceBrick 上完成比赛并选择 KEEP RACE LOG 后会出现在这里。")
            return
        }
        for (l in visible) {
            val card = card()
            card.addView(title(l.trackName.ifBlank { "RaceBrick 记录" }))
            card.addView(
                meta(
                    "${l.date}  ·  ${formatDuration((l.durationS * 1000).toLong())}  ·  " +
                        "${l.laps} 圈  ·  " +
                        if (l.hasBest) "最佳 ${formatLap(l.bestS)}（L${l.bestIndex}）" else "无最佳圈"
                )
            )
            card.addView(
                row(
                    button("圈速详情") { showLogDetail(l) },
                )
            )
            llList.addView(card)
        }
    }

    private fun showLogDetail(l: RaceLogEntry) {
        val b = base()
        status("正在读取 Race Log 轨迹…")
        worker.execute {
            runCatching { RaceApi.logDetail(b, l.index) }
                .onSuccess { d -> main.post { if (!isFinishing) buildLogDetailDialog(d).show() } }
                .onFailure { e -> main.post { fail("读取 Race Log 失败：${e.message}") } }
        }
    }

    private fun buildLogDetailDialog(d: RaceLogDetail): AlertDialog {
        val l = d.entry
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), 0)
        }
        content.addView(meta("${l.date}  ·  ${l.laps} 圈  ·  Vehicle ${l.vehicle}"))
        if (d.trace.isNotEmpty()) {
            content.addView(TrackMapView(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(220),
                ).apply { topMargin = dp(6) }
                setLayers(listOf(TrackMapView.Layer(d.trace, 0xFFFFD700.toInt(), "BEST")))
            })
            content.addView(meta("Race Log 保存的是最佳圈轨迹（L${l.bestIndex}）。"))
        }
        if (l.lapTimes.isEmpty()) {
            content.addView(meta("该 Race Log 没有保存圈速。"))
        } else {
            for ((i, t) in l.lapTimes.withIndex()) {
                val lap = i + 1
                val best = l.hasBest && lap == l.bestIndex
                content.addView(TextView(this).apply {
                    text = "第 $lap 圈   ${formatLap(t)}${if (best) "   BEST" else ""}"
                    setTextColor(if (best) 0xFFFFD700.toInt() else 0xFFF0F6F4.toInt())
                    textSize = 14f
                    setTypeface(typeface, if (best) Typeface.BOLD else Typeface.NORMAL)
                    setPadding(0, dp(6), 0, dp(6))
                })
            }
        }
        return AlertDialog.Builder(this)
            .setTitle("Race Log 详情")
            .setView(ScrollView(this).apply { addView(content) })
            .setPositiveButton("关闭", null)
            .create()
    }

    private fun showSessionDetail(summary: RaceSession) {
        status("正在读取场次详情…")
        val b = base()
        worker.execute {
            runCatching {
                val s = RaceApi.session(b, summary.slot)
                val laps = runCatching { RaceApi.sessionLaps(b, summary.slot) }.getOrElse { emptyList() }
                val csv = runCatching { RaceApi.sessionCsv(b, summary.slot) }.getOrNull()
                Triple(s, laps, if (csv != null) parseSessionTraces(csv) else emptyMap())
            }.onSuccess { (s, laps, traces) ->
                main.post { if (!isFinishing) buildDetailDialog(s, laps, traces).show() }
            }.onFailure { e -> main.post { fail("读取详情失败：${e.message}") } }
        }
    }

    private fun parseSessionTraces(data: ByteArray): Map<Int, List<android.graphics.PointF>> {
        val text = String(data, Charsets.UTF_8)
        val grouped = LinkedHashMap<Int, MutableList<android.graphics.PointF>>()
        var lat0 = Double.NaN
        var lon0 = 0.0
        for (line in text.lineSequence()) {
            val p = line.split(',')
            if (p.size < 7) continue
            val lap = p[2].trim().toIntOrNull() ?: continue
            val lat = p[4].trim().toDoubleOrNull() ?: continue
            val lon = p[5].trim().toDoubleOrNull() ?: continue
            if (lat == 0.0 && lon == 0.0) continue
            if (lat0.isNaN()) { lat0 = lat; lon0 = lon }
            val x = (lon - lon0) * 111320.0 * kotlin.math.cos(Math.toRadians(lat0))
            val y = (lat - lat0) * 111320.0
            grouped.getOrPut(lap) { ArrayList() } += android.graphics.PointF(x.toFloat(), y.toFloat())
        }
        val out = LinkedHashMap<Int, List<android.graphics.PointF>>()
        for ((lap, pts) in grouped) {
            if (pts.size < 2) continue
            val step = (pts.size / 360).coerceAtLeast(1)
            val reduced = ArrayList<android.graphics.PointF>(360)
            var i = 0
            while (i < pts.size) { reduced += pts[i]; i += step }
            if (reduced.last() !== pts.last()) reduced += pts.last()
            out[lap] = reduced
        }
        return out
    }

    private fun shapes(
        selected: Set<Int>,
        traces: Map<Int, List<android.graphics.PointF>>,
        colors: IntArray,
        best: Int?,
    ): List<TrackMapView.Layer> {
        val layers = ArrayList<TrackMapView.Layer>()
        for ((idx, lap) in traces.keys.sorted().withIndex()) {
            if (lap !in selected) continue
            layers += TrackMapView.Layer(
                traces[lap].orEmpty(),
                if (lap == best) 0xFFFFD700.toInt() else colors[idx % colors.size],
                "L$lap",
            )
        }
        return layers
    }

    private fun buildDetailDialog(
        s: RaceSession,
        laps: List<LapDetail>,
        traces: Map<Int, List<android.graphics.PointF>>,
    ): AlertDialog {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        content.addView(meta("${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  ${s.samples} 点"))
        val best = laps.filter { !it.deleted && it.time > 0 }.minByOrNull { it.time }?.lap
        if (traces.isNotEmpty()) {
            val map = TrackMapView(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(220),
                )
            }
            val colors = intArrayOf(
                0xFF66E0D2.toInt(), 0xFFFFB74D.toInt(), 0xFF8B5CF6.toInt(),
                0xFF5ED6A3.toInt(), 0xFFFF6B9A.toInt(), 0xFF42A5F5.toInt(),
                0xFFFFD700.toInt(), 0xFFFF7043.toInt(),
            )
            val selected = HashSet<Int>()
            fun refreshMap() {
                val layers = shapes(selected, traces, colors, best)
                map.setLayers(layers)
            }
            selected.addAll(traces.keys)
            refreshMap()
            content.addView(map)
            val checks = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, dp(4))
            }
            for (lap in traces.keys.sorted()) {
                checks.addView(CheckBox(this).apply {
                    text = "L$lap"
                    textSize = 12f
                    isChecked = true
                    setTextColor(if (lap == best) 0xFFFFD700.toInt() else 0xFFD7E7E4.toInt())
                    setOnCheckedChangeListener { _, checked ->
                        if (checked) selected.add(lap) else selected.remove(lap)
                        refreshMap()
                    }
                })
            }
            content.addView(HorizontalScrollView(this).apply { addView(checks) })
            content.addView(meta("可勾选单圈或多圈叠加轨迹。"))
        }
        if (laps.isEmpty() && s.lapTimes.isEmpty()) {
            content.addView(meta("没有有效圈速。"))
        } else if (laps.isNotEmpty()) {
            for (l in laps) {
                val row = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(0, dp(6), 0, dp(6))
                }
                val isBest = l.lap == best
                row.addView(
                    TextView(this).apply {
                        text = "第 ${l.lap} 圈   ${formatLap(l.time)}" +
                            (if (isBest) "   BEST" else "") +
                            (if (l.deleted) "   (已删除)" else "")
                        setTextColor(
                            when {
                                l.deleted -> 0xFF8A9A9A.toInt()
                                isBest -> 0xFFFFD700.toInt()
                                else -> 0xFFF0F6F4.toInt()
                            }
                        )
                        textSize = 14f
                        setTypeface(typeface, if (isBest) Typeface.BOLD else Typeface.NORMAL)
                    }
                )
                row.addView(
                    TextView(this).apply {
                        text = String.format(
                            Locale.US,
                            "距离 %.0fm  ·  均速 %.1f km/h  ·  最高 %.1f km/h  ·  %d 点",
                            l.distanceM, l.avgKmh, l.maxKmh, l.samples,
                        )
                        setTextColor(0xFFA9C4BE.toInt())
                        textSize = 12f
                    }
                )
                if (!l.deleted) {
                    val del = button("删除这一圈") { confirmDeleteLap(s, l.lap) }
                    del.layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                    ).apply { topMargin = dp(4) }
                    row.addView(del)
                }
                content.addView(row)
            }
        } else {
            for ((index, t) in s.lapTimes.withIndex()) {
                val lap = index + 1
                val deleted = (s.deletedLapMask and (1 shl (lap - 1))) != 0
                content.addView(meta("第 $lap 圈   ${formatLap(t)}${if (deleted) "  (已删除)" else ""}"))
            }
        }
        val scroll = ScrollView(this).apply { addView(content) }
        return AlertDialog.Builder(this)
            .setTitle("场次详情")
            .setView(scroll)
            .setPositiveButton("关闭", null)
            .create()
    }

    private fun editTrack(t: RaceTrack) {
        val b = base()
        status("正在读取赛道轨迹…")
        worker.execute {
            runCatching { RaceApi.track(b, t.id) }
                .onSuccess { d -> main.post { if (!isFinishing) showTrackEditor(t, d) } }
                .onFailure { e -> main.post { fail("读取轨迹失败：${e.message}") } }
        }
    }

    private fun showTrackEditor(t: RaceTrack, detail: TrackDetail) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        fun addLabel(text: String) {
            box.addView(TextView(this).apply {
                this.text = text
                setTextColor(0xFFA9C4BE.toInt())
                textSize = 12f
                setPadding(0, dp(8), 0, dp(2))
            })
        }
        addLabel("赛道名称（最多 15 个字符）")
        val etName = EditText(this).apply {
            setText(t.name)
            filters = arrayOf(InputFilter.LengthFilter(15))
            setSelection(text.length)
        }
        box.addView(etName)
        addLabel("赛道轨迹 / 起始线预览")
        val map = TrackMapView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(210),
            ).apply { topMargin = dp(6) }
            setLayers(detail.points)
            startF = t.startF.toFloat()
            widthM = t.widthM.toFloat()
            trackLengthM = detail.lengthM.toFloat()
            reverse = t.reverse
        }
        box.addView(map)
        addLabel("起始线位置")
        val tvStart = TextView(this).apply {
            text = String.format(Locale.US, "%.1f%%", t.startF * 100.0)
            setTextColor(0xFF66E0D2.toInt())
            textSize = 18f
            setTypeface(typeface, Typeface.BOLD)
        }
        box.addView(tvStart)
        val sbStart = SeekBar(this).apply {
            max = 1000
            progress = (t.startF * 1000.0).toInt().coerceIn(0, 1000)
        }
        sbStart.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                tvStart.text = String.format(Locale.US, "%.1f%%", progress / 10.0)
                map.startF = progress / 1000f
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
        box.addView(sbStart)

        addLabel("赛道宽度 / 起始线长度")
        val tvWidth = TextView(this).apply {
            text = String.format(Locale.US, "%.1f m", t.widthM)
            setTextColor(0xFF66E0D2.toInt())
            textSize = 18f
            setTypeface(typeface, Typeface.BOLD)
        }
        box.addView(tvWidth)
        val sbWidth = SeekBar(this).apply {
            max = 490
            progress = ((t.widthM - 1.0) * 10.0).toInt().coerceIn(0, 490)
        }
        sbWidth.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val w = 1.0 + progress / 10.0
                tvWidth.text = String.format(Locale.US, "%.1f m", w)
                map.widthM = w.toFloat()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
        box.addView(sbWidth)

        val swReverse = Switch(this).apply {
            text = "反向起点方向（CCW）"
            isChecked = t.reverse
            setOnCheckedChangeListener { _, checked -> map.reverse = checked }
        }
        box.addView(swReverse)
        box.addView(TextView(this).apply {
            text = "保存后会写入 ESP，并立即用于下一场计圈。"
            setTextColor(0xFF6D817D.toInt())
            textSize = 11f
            setPadding(0, dp(8), 0, 0)
        })
        val scroll = ScrollView(this).apply { addView(box) }
        val dialog = AlertDialog.Builder(this)
            .setTitle("编辑赛道")
            .setView(scroll)
            .setNegativeButton("取消", null)
            .setPositiveButton("保存", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val name = etName.text.toString().trim()
                val startPct = sbStart.progress / 10.0
                val width = 1.0 + sbWidth.progress / 10.0
                if (name.isEmpty()) {
                    toast("请填写名称")
                    return@setOnClickListener
                }
                val b = base()
                status("正在保存赛道设置…")
                worker.execute {
                    runCatching {
                        RaceApi.updateTrack(
                            b, t.id, name, startPct / 100.0,
                            swReverse.isChecked, width,
                        )
                        val saved = RaceApi.tracks(b).firstOrNull { it.id == t.id }
                        if (saved == null || saved.name != name) {
                            throw IllegalStateException("设备未确认新名称")
                        }
                    }.onSuccess {
                        main.post {
                            dialog.dismiss()
                            toast("赛道设置已保存并同步")
                            loadAll()
                        }
                    }.onFailure { e -> main.post { fail("保存失败：${e.message}") } }
                }
            }
        }
        dialog.show()
    }

    private fun confirmDeleteTrack(t: RaceTrack) {
        confirm("删除赛道", "删除「${t.name}」？赛道几何会被清空，已保存场次不会删除。") {
            val gen = ++generation
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteTrack(b, t.id) }
                    .onSuccess {
                        main.post {
                            if (gen != generation) return@post
                            toast("已删除赛道")
                            filterTrack = null
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun confirmDeleteSession(s: RaceSession) {
        confirm("删除场次", "删除这整场原始数据？此操作不可恢复。") {
            val gen = ++generation
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteSession(b, s.slot) }
                    .onSuccess {
                        main.post {
                            if (gen != generation) return@post
                            toast("已删除场次")
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun confirmDeleteLap(s: RaceSession, lap: Int) {
        confirm("删除单圈", "只删除第 $lap 圈的圈速/轨迹标记，原始场次保留。") {
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteLap(b, s.slot, lap) }
                    .onSuccess {
                        main.post {
                            toast("已删除第 $lap 圈")
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun exportSession(s: RaceSession) {
        status("正在下载 CSV…")
        val b = base()
        worker.execute {
            runCatching {
                val bytes = RaceApi.sessionCsv(b, s.slot)
                val dir = File(cacheDir, "race_exports").apply { mkdirs() }
                val file = File(dir, "racebrick-session-${s.id}.csv")
                file.writeBytes(bytes)
                file
            }.onSuccess { file ->
                main.post {
                    status("已生成 ${file.name}")
                    val uri = FileProvider.getUriForFile(
                        this,
                        "$packageName.files",
                        file,
                    )
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = "text/csv"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        putExtra(Intent.EXTRA_SUBJECT, "RaceBrick session ${s.id}")
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    startActivity(Intent.createChooser(send, "导出到 RaceChrono / 文件"))
                }
            }.onFailure { e -> main.post { fail("导出失败：${e.message}") } }
        }
    }

    private fun deletedLaps(s: RaceSession): String =
        (1..s.laps).filter { (s.deletedLapMask and (1 shl (it - 1))) != 0 }
            .joinToString("、")

    private fun card(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundResource(R.drawable.bg_card)
        setPadding(dp(12), dp(12), dp(12), dp(12))
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(10) }
    }

    private fun title(text: String): TextView = TextView(this).apply {
        this.text = text
        setTextColor(0xFFF0F6F4.toInt())
        textSize = 16f
        setTypeface(typeface, Typeface.BOLD)
    }

    private fun meta(text: String, warning: Boolean = false): TextView = TextView(this).apply {
        this.text = text
        setTextColor(if (warning) 0xFFE0A040.toInt() else 0xFFA9C4BE.toInt())
        textSize = 12f
        setPadding(0, dp(4), 0, 0)
    }

    private fun row(vararg children: Button): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        setPadding(0, dp(8), 0, 0)
        for (child in children) {
            addView(child)
        }
    }

    private fun button(text: String, danger: Boolean = false, action: () -> Unit): Button =
        Button(this).apply {
            this.text = text
            textSize = 12f
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setBackgroundResource(if (danger) R.drawable.btn_danger else R.drawable.btn_secondary)
            backgroundTintList = null
            setTextColor(if (danger) 0xFFFFDADA.toInt() else 0xFFD7E7E4.toInt())
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                .apply { marginStart = dp(4) }
            setOnClickListener { action() }
        }

    private fun empty(text: String) {
        llList.addView(TextView(this).apply {
            this.text = text
            setTextColor(0xFF6D817D.toInt())
            textSize = 13f
            setPadding(dp(4), dp(12), dp(4), dp(12))
        })
    }

    private fun status(text: String) {
        tvStatus.text = text
        tvStatus.setTextColor(0xFFA9C4BE.toInt())
    }

    private fun fail(text: String) {
        tvStatus.text = text
        tvStatus.setTextColor(0xFFFF9B9B.toInt())
    }

    private fun toast(text: String) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show()
    }

    private fun confirm(title: String, message: String, action: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setNegativeButton("取消", null)
            .setPositiveButton("确认") { _, _ -> action() }
            .show()
    }

    private fun formatTime(epoch: Long): String {
        if (epoch <= 1_600_000_000L) return "时间未同步"
        return SimpleDateFormat("MM-dd HH:mm:ss", Locale.getDefault()).format(Date(epoch * 1000L))
    }

    private fun formatDuration(ms: Long): String {
        val total = (ms / 1000L).coerceAtLeast(0L)
        return String.format(Locale.US, "%d:%02d", total / 60L, total % 60L)
    }

    private fun formatLap(seconds: Double): String {
        if (seconds <= 0.0) return "--"
        val total = seconds
        val m = (total / 60.0).toInt()
        val s = total - m * 60
        return String.format(Locale.US, "%d:%06.3f", m, s)
    }

    private fun formatLength(m: Double): String =
        if (m >= 1000.0) String.format(Locale.US, "%.2f km", m / 1000.0)
        else String.format(Locale.US, "%.0f m", m)

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    override fun onDestroy() {
        generation++
        direct.disconnect()
        worker.shutdownNow()
        super.onDestroy()
    }
}
