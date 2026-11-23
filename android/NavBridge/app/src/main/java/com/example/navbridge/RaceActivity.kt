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
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
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
    private lateinit var pbLoad: ProgressBar

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val satelliteWorker = Executors.newSingleThreadExecutor()
    private lateinit var direct: WifiDirectConnector
    private var generation = 0

    private var tracks: List<RaceTrack> = emptyList()
    private var sessions: List<RaceSession> = emptyList()
    private var logs: List<RaceLogEntry> = emptyList()
    private val traceCache = HashMap<Int, Map<Int, List<android.graphics.PointF>>>()
    private val trackDetailCache = HashMap<Int, TrackDetail>()
    private val satelliteCache = HashMap<Int, TrackSatellite>()
    private var filterTrack: Int? = null
    private var logFilterName: String? = null
    private var showingSessions = false
    private var showingLogs = false
    private var directPending = false
    private var awaitingWriteSettings = false

    private data class LoadedData(
        val tracks: List<RaceTrack>,
        val sessions: List<RaceSession>,
        val logs: List<RaceLogEntry>,
        val traces: Map<Int, Map<Int, List<android.graphics.PointF>>>,
        val details: Map<Int, TrackDetail>,
        val satellites: Map<Int, TrackSatellite>,
        val satelliteError: String? = null,
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_race)

        tvStatus = findViewById(R.id.tvRaceStatus)
        etUrl = findViewById(R.id.etRaceUrl)
        llList = findViewById(R.id.llRaceList)
        pbLoad = findViewById(R.id.pbRaceLoad)
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

    private fun satelliteForSession(s: RaceSession): TrackSatellite? {
        satelliteCache[s.trackSlot]?.let { return it }
        val t = tracks.firstOrNull { it.name == s.trackName } ?: return null
        return satelliteCache[t.id]
    }

    private fun loadAll() {
        direct.bindCurrentEsp()
        val gen = ++generation
        val b = base()
        val directBound = direct.isConnected
        val internetNetwork = direct.internetNetwork
        pbLoad.visibility = View.VISIBLE
        pbLoad.progress = 0
        status("正在读取赛道和场次…")
        worker.execute {
            runCatching {
                val t = RaceApi.tracks(b)
                publishLoad(gen, 8, "已读取 ${t.size} 条赛道")
                val s = RaceApi.sessions(b)
                publishLoad(gen, 15, "已读取 ${s.size} 条 Race Log")
                val l = runCatching { RaceApi.logs(b) }.getOrElse { emptyList() }
                val tc = HashMap<Int, Map<Int, List<android.graphics.PointF>>>()
                for ((i, session) in s.withIndex()) {
                    runCatching { RaceApi.sessionTraces(b, session.slot) }
                        .onSuccess { if (it.isNotEmpty()) tc[session.slot] = it }
                    publishLoad(gen, 15 + 30 * (i + 1) / s.size.coerceAtLeast(1),
                        "正在读取圈轨迹 ${i + 1}/${s.size}")
                }
                val details = HashMap<Int, TrackDetail>()
                for ((i, track) in t.withIndex()) {
                    val detail = runCatching { RaceApi.track(b, track.id) }.getOrNull() ?: continue
                    details[track.id] = detail
                    publishLoad(gen, 45 + 20 * (i + 1) / t.size.coerceAtLeast(1),
                        "正在读取赛道几何 ${i + 1}/${t.size}")
                }
                LoadedData(t, s, l, tc, details, emptyMap(), null)
            }.onSuccess { loaded ->
                main.post {
                    if (gen != generation) return@post
                    tracks = loaded.tracks
                    sessions = loaded.sessions
                    logs = loaded.logs
                    traceCache.clear()
                    traceCache.putAll(loaded.traces)
                    trackDetailCache.clear()
                    trackDetailCache.putAll(loaded.details)
                    satelliteCache.clear()
                    pbLoad.progress = 65
                    val geoTracks = loaded.details.values.count { it.geoRef != null }
                    status(
                        "赛道 ${loaded.tracks.size} 条，Race Log ${loaded.sessions.size} 条，" +
                            "正在加载卫星图 0/$geoTracks"
                    )
                    when {
                        showingLogs -> renderLogs()
                        showingSessions -> renderSessions()
                        else -> renderTracks()
                    }
                    loadSatellites(gen, loaded.tracks, loaded.details, directBound, internetNetwork)
                }
            }.onFailure { e ->
                main.post {
                    if (gen != generation) return@post
                    pbLoad.visibility = View.GONE
                    fail("连接失败：${e.message}")
                    renderTracks()
                }
            }
        }
    }

    private fun publishLoad(gen: Int, progress: Int, text: String) {
        main.post {
            if (gen != generation) return@post
            pbLoad.visibility = View.VISIBLE
            pbLoad.progress = progress.coerceIn(0, 100)
            status(text)
        }
    }

    private fun loadSatellites(
        gen: Int,
        loadedTracks: List<RaceTrack>,
        details: Map<Int, TrackDetail>,
        directBound: Boolean,
        internetNetwork: android.net.Network?,
    ) {
        val geo = loadedTracks.filter { details[it.id]?.geoRef != null }
        if (geo.isEmpty()) {
            finishSatelliteLoad(gen, 0, geo.size, "无 GPS 基准赛道")
            return
        }
        if (directBound && internetNetwork == null) {
            finishSatelliteLoad(gen, 0, geo.size, "直连 ESP 时没有可用的互联网网络")
            return
        }
        satelliteWorker.execute {
            var loaded = 0
            var error: String? = null
            for ((i, track) in geo.withIndex()) {
                if (gen != generation) return@execute
                val detail = details[track.id] ?: continue
                val sat = runCatching {
                    TrackSatelliteApi.load(detail, internetNetwork)
                }.onFailure {
                    if (error == null) error = it.message ?: "卫星图请求失败"
                }.getOrNull()
                if (sat == null) {
                    if (error == null) error = "卫星图解码失败"
                    break
                }
                loaded++
                main.post {
                    if (gen == generation) satelliteCache[track.id] = sat
                }
                main.post {
                    if (gen != generation) return@post
                    pbLoad.visibility = View.VISIBLE
                    pbLoad.progress = (65 + 35 * loaded / geo.size).coerceIn(65, 99)
                    status("正在加载卫星图 $loaded/${geo.size}")
                }
            }
            main.post {
                if (gen != generation) return@post
                when {
                    showingLogs -> renderLogs()
                    showingSessions -> renderSessions()
                    else -> renderTracks()
                }
                finishSatelliteLoad(gen, loaded, geo.size, error)
            }
        }
    }

    private fun finishSatelliteLoad(gen: Int, loaded: Int, total: Int, error: String?) {
        if (gen != generation) return
        pbLoad.progress = 100
        val suffix = when {
            error != null -> "（$error）"
            total == 0 -> "（无 GPS 基准赛道）"
            else -> ""
        }
        status("赛道 ${tracks.size} 条，Race Log ${sessions.size} 条，卫星图 $loaded/$total 张$suffix")
        main.postDelayed({ if (gen == generation) pbLoad.visibility = View.GONE }, 1200)
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
            val detail = trackDetailCache[t.id]
            if (detail != null) {
                val preview = TrackMapView(this).apply {
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, dp(120),
                    ).apply { topMargin = dp(8) }
                    setLayers(detail.points)
                    satelliteCache[t.id]?.let { setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds)) }
                    setOnClickListener { showTrackPreview(t, detail) }
                }
                card.addView(preview)
            }
            if (t.geo && satelliteCache[t.id] == null) {
                val detail = trackDetailCache[t.id]
                card.addView(meta(
                    when {
                        detail == null -> "赛道详情读取失败，点“编辑”重试"
                        detail.geoRef == null -> "该赛道缺少 GPS 基准，无法生成卫星图"
                        else -> "卫星图未加载，点地图或编辑重试"
                    },
                    warning = true,
                ))
            }
            card.addView(
                row(
                    button("编辑") { editTrack(t) },
                    button("Race Log") {
                        filterTrack = t.id
                        showingSessions = true
                        showingLogs = false
                        renderSessions()
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
        val trackName = tracks.firstOrNull { it.id == filter }?.name
        val visible = if (filter == null) sessions else sessions.filter {
            it.trackSlot == filter || (trackName != null && it.trackName == trackName)
        }
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
            header.addView(
                button("清空 Log", danger = true) {
                    deleteTrackLogs(filter, visible)
                }
            )
            llList.addView(header)
        }
        if (visible.isEmpty()) {
            empty(
                if (filter == null) "没有 Race Log。设备端进入 RACE 并在 GPS 有定位后开始比赛，结束后会自动保存完整记录。"
                else "这条赛道还没有 Race Log。"
            )
            return
        }
        for (s in visible.sortedByDescending { it.startEpoch }) {
            val card = card()
            val displayName = tracks.firstOrNull { it.id == s.trackSlot }?.name
                ?: s.trackName.ifBlank { "赛道 #${s.trackSlot}" }
            card.addView(title(displayName))
            card.addView(
                meta(
                    "${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  " +
                        "${s.laps} 圈  ·  ${s.samples} 点"
                )
            )
            val preview = traceCache[s.slot]
            if (preview != null && preview.isNotEmpty()) {
                card.addView(TrackMapView(this).apply {
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, dp(130),
                    ).apply { topMargin = dp(8) }
                    val cols = intArrayOf(
                        0xFF66E0D2.toInt(), 0xFFFFB74D.toInt(), 0xFF8B5CF6.toInt(),
                        0xFF5ED6A3.toInt(), 0xFFFF6B9A.toInt(), 0xFF42A5F5.toInt(),
                    )
                    setLayers(preview.entries.sortedBy { it.key }.mapIndexed { i, e ->
                        TrackMapView.Layer(e.value, cols[i % cols.size], "L${e.key}")
                    })
                    satelliteForSession(s)?.let {
                        setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
                    }
                })
            }
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
        val matching = if (filter == null) logs else logs.filter { it.trackName == filter }
        val visible = if (matching.isEmpty()) logs else matching
        if (filter != null) {
            llList.addView(TextView(this).apply {
                text = if (matching.isEmpty()) "未找到「$filter」同名 Race Log，显示全部" else "筛选：$filter"
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
                    button("关联到赛道") { associateLog(l) },
                )
            )
            llList.addView(card)
        }
    }

    private fun associateLog(l: RaceLogEntry) {
        if (tracks.isEmpty()) {
            toast("没有可关联的赛道")
            return
        }
        val names = tracks.map { it.name }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("把 Race Log 关联到赛道")
            .setItems(names) { _, which ->
                val target = tracks[which].name
                if (target == l.trackName) return@setItems
                val b = base()
                worker.execute {
                    runCatching { RaceApi.renameLogs(b, l.trackName, target) }
                        .onSuccess {
                            main.post {
                                toast("Race Log 已关联到 $target")
                                loadAll()
                            }
                        }
                        .onFailure { e -> main.post { fail("关联失败：${e.message}") } }
                }
            }
            .setNegativeButton("取消", null)
            .show()
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
                tracks.firstOrNull { it.name == l.trackName }?.let { tr ->
                    satelliteCache[tr.id]?.let { setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds)) }
                }
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
        status("正在读取 Race Log 详情…")
        val b = base()
        worker.execute {
            runCatching {
                val s = RaceApi.session(b, summary.slot)
                val laps = runCatching { RaceApi.sessionLaps(b, summary.slot) }.getOrElse { emptyList() }
                val traces = traceCache[summary.slot]
                    ?: runCatching { RaceApi.sessionTraces(b, summary.slot) }.getOrElse { emptyMap() }
                Triple(s, laps, traces)
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
        val maxDist = laps.filter { !it.deleted }.maxOfOrNull { it.distanceM } ?: 0.0
        val complete = laps.filter { !it.deleted && it.time > 0 && (maxDist <= 0.0 || it.distanceM >= maxDist * 0.7) }
        val best = complete.minByOrNull { it.time }?.lap
        if (traces.isNotEmpty()) {
            val map = TrackMapView(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(220),
                )
                satelliteForSession(s)?.let {
                    setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
                }
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
            .setTitle("Race Log 详情")
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
            satelliteCache[t.id]?.let {
                setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
            }
        }
        box.addView(map)
        val satButton = button("同步卫星图到设备") {
            syncTrackSatellite(t, satelliteCache[t.id])
        }
        satButton.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(6) }
        box.addView(satButton)
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

    private fun showTrackPreview(t: RaceTrack, detail: TrackDetail?) {
        if (detail == null) {
            toast("赛道轨迹尚未加载")
            return
        }
        val satellite = satelliteCache[t.id]
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), 0)
        }
        box.addView(meta(
            "点击预览可查看卫星图叠加；同步后设备端同一赛道也会显示。"
        ))
        box.addView(TrackMapView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(320),
            ).apply { topMargin = dp(8) }
            setLayers(detail.points)
            startF = t.startF.toFloat()
            widthM = t.widthM.toFloat()
            trackLengthM = detail.lengthM.toFloat()
            reverse = t.reverse
            satellite?.let { setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds)) }
        })
        if (satellite == null) {
            box.addView(meta("该赛道没有 GPS 基准，或卫星图加载失败。", warning = true))
        } else {
            box.addView(meta(String.format(Locale.US, "卫星图覆盖 %.0f m，透明度 50%%", satellite.spanM)))
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("赛道卫星预览")
            .setView(ScrollView(this).apply { addView(box) })
            .setPositiveButton("关闭", null)
            .setNeutralButton("同步到设备", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).isEnabled = satellite != null
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                syncTrackSatellite(t, satellite)
            }
        }
        dialog.show()
    }

    private fun syncTrackSatellite(t: RaceTrack, satellite: TrackSatellite?) {
        if (satellite == null) {
            toast("没有可同步的卫星图")
            return
        }
        val b = base()
        status("正在同步卫星图到设备…")
        worker.execute {
            runCatching {
                RaceApi.uploadTrackImage(
                    b, t.id, satellite.pal16,
                    satellite.bounds.left, satellite.bounds.top,
                    satellite.bounds.right, satellite.bounds.bottom,
                )
            }.onSuccess {
                main.post {
                    status("卫星图已同步到设备")
                    toast("卫星图已同步：${t.name}")
                }
            }.onFailure { e ->
                main.post { fail("同步失败：${e.message}") }
            }
        }
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

    private fun deleteTrackLogs(trackSlot: Int, list: List<RaceSession>) {
        if (list.isEmpty()) {
            toast("没有可删除的记录")
            return
        }
        confirm("清空该赛道 Race Log", "删除 ${list.size} 条记录？此操作不可恢复。") {
            val b = base()
            worker.execute {
                val failed = ArrayList<String>()
                for (s in list) {
                    runCatching { RaceApi.deleteSession(b, s.slot) }
                        .onFailure { failed += "${s.id}: ${it.message}" }
                }
                main.post {
                    if (failed.isEmpty()) {
                        toast("已清空 ${list.size} 条 Race Log")
                        loadAll()
                    } else {
                        fail("部分删除失败：${failed.first()}")
                        loadAll()
                    }
                }
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
        satelliteWorker.shutdownNow()
        super.onDestroy()
    }
}
