package com.example.navbridge

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.InputFilter
import android.text.InputType
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
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.GZIPInputStream

class RaceActivity : LocalizedActivity() {
    companion object {
        const val EXTRA_AUTO_SYNC_UID = "auto_sync_uid"
        const val EXTRA_OFFLINE = "offline_cache"
        const val EXTRA_FORCE_LOCAL = "force_local"
    }

    private lateinit var tvStatus: TextView
    private lateinit var etUrl: EditText
    private lateinit var llList: LinearLayout
    private lateinit var pbLoad: ProgressBar
    private var busyDialog: AlertDialog? = null
    private var busyProgress: ProgressBar? = null
    private var busyText: TextView? = null

    private val main = Handler(Looper.getMainLooper())
    private val localDb by lazy { SyncLocalDb(this) }
    private var localMode = false
    private val worker = Executors.newSingleThreadExecutor()
    private val satelliteWorker = Executors.newSingleThreadExecutor()
    private val satelliteUploadWorker = Executors.newSingleThreadExecutor()
    private val cloudWorker = Executors.newSingleThreadExecutor()
    private val cloudApi = CloudApi()
    private lateinit var direct: WifiDirectConnector
    private var generation = 0

    private var tracks: List<RaceTrack> = emptyList()
    private var sessions: List<RaceSession> = emptyList()
    private var logs: List<RaceLogEntry> = emptyList()
    private val traceCache = HashMap<Int, Map<Int, TraceData>>()
    private val sessionLapCache = HashMap<Int, List<LapDetail>>()
    private val logTraceCache = HashMap<Int, List<android.graphics.PointF>>()
    private val trackDetailCache = HashMap<Int, TrackDetail>()
    private val satelliteCache = HashMap<Int, TrackSatellite>()
    private var filterTrack: Int? = null
    private var logFilterName: String? = null
    private var showingSessions = false
    private var showingLogs = false
    private var directPending = false
    private var awaitingWriteSettings = false
    private var offlineMode = false
    private var discoveredDevice: DiscoveredDevice? = null
    private var hotspotWatching = false
    private var hotspotChecking = false
    private var hotspotMisses = 0
    private var lastPromptedUid: String? = null
    private var lastPromptedIp: String? = null
    private var cloudSyncRunning = false
    private var satelliteUploadRunning = false
    private var autoSyncUid: String? = null
    private var forceLocalSync = false
    private val hotspotHandler = Handler(Looper.getMainLooper())
    private val hotspotTick = object : Runnable {
        override fun run() {
            if (!hotspotWatching) return
            checkHotspot()
            hotspotHandler.postDelayed(this, 10_000)
        }
    }

    private data class LoadedData(
        val tracks: List<RaceTrack>,
        val sessions: List<RaceSession>,
        val logs: List<RaceLogEntry>,
        val traces: Map<Int, Map<Int, TraceData>>,
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
        autoSyncUid = intent.getStringExtra(EXTRA_AUTO_SYNC_UID)
        forceLocalSync = intent.getBooleanExtra(EXTRA_FORCE_LOCAL, false)
        offlineMode = intent.getBooleanExtra(EXTRA_OFFLINE, false)
        etUrl.setText(RaceApi.baseUrl(this))

        findViewById<Button>(R.id.btnRaceNav).setOnClickListener { finish() }
        findViewById<Button>(R.id.btnRaceSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.btnRaceDirect).setOnClickListener { directWithPermission() }
        findViewById<Button>(R.id.btnRaceCloud).setOnClickListener {
            startActivity(Intent(this, SyncActivity::class.java)
                .putExtra(SyncActivity.EXTRA_BASE, discoveredDevice?.baseUrl ?: RaceApi.baseUrl(this))
                .putExtra(SyncActivity.EXTRA_UID, discoveredDevice?.uid ?: LocalCache.uid(this)))
        }
        findViewById<Button>(R.id.btnRaceSat).setOnClickListener { uploadAllSatellites(manual = true) }
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

        if (offlineMode) {
            offlineModeUi()
            loadCachedSnapshot()
        } else if (localDb.summary().let { it.tracks > 0 || it.sessions > 0 }) {
            loadLocalData()
        } else {
            status("点击“连接并刷新”读取设备数据")
        }
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
        if (offlineMode) return
        hotspotWatching = true
        hotspotHandler.removeCallbacks(hotspotTick)
        hotspotHandler.postDelayed(hotspotTick, 1200)
    }

    override fun onPause() {
        hotspotWatching = false
        hotspotHandler.removeCallbacks(hotspotTick)
        super.onPause()
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

    private fun trackForSession(s: RaceSession): RaceTrack? =
        tracks.firstOrNull { it.id == s.trackSlot }
            ?: tracks.firstOrNull { it.name == s.trackName }

    private fun styleStartLine(map: TrackMapView, t: RaceTrack, detail: TrackDetail? = null) {
        map.startF = t.startF.toFloat()
        map.widthM = (detail?.startWidthM ?: t.startWidthM).toFloat()
        map.trackLengthM = (detail?.lengthM ?: t.lengthM).toFloat()
        map.startOffsetM = (detail?.startOffsetM ?: t.startOffsetM).toFloat()
        map.reverse = t.reverse
    }

    private fun loadLocalData() {
        localMode = true
        status("正在读取 Local App 数据…")
        worker.execute {
            runCatching {
                val trackEntities = localDb.tracks()
                val sessionEntities = localDb.sessions()
                val trackList = ArrayList<RaceTrack>()
                val details = HashMap<Int, TrackDetail>()
                for (e in trackEntities) {
                    val p = e.payload
                    val id = e.localKey.toIntOrNull() ?: continue
                    val sections = ArrayList<Double>()
                    p.optJSONArray("sections")?.let { a -> for (i in 0 until a.length()) sections += a.optDouble(i, 0.0) }
                    val points = localPoints(p)
                    val detail = localDetail(p, points) ?: continue
                    trackList += RaceTrack(
                        id = id,
                        name = p.optString("name", e.name),
                        geo = p.optBoolean("geo", detail.geoRef != null),
                        lengthM = p.optDouble("length_m", detail.lengthM),
                        sections = sections.size,
                        widthM = p.optDouble("width", 5.0),
                        startWidthM = p.optDouble("start_width", 5.0),
                        startF = p.optDouble("start_f", 0.0),
                        startOffsetM = p.optDouble("start_offset", 0.0),
                        reverse = p.optBoolean("reverse", false),
                        uuid = p.optString("uuid", e.uuid),
                        revision = p.optInt("version", 1),
                        revisionId = p.optString("revision_id", ""),
                    )
                    details[id] = detail
                }
                val sessionList = ArrayList<RaceSession>()
                val traces = HashMap<Int, Map<Int, TraceData>>()
                for (e in sessionEntities) {
                    val p = e.payload
                    val slot = e.localKey.toIntOrNull() ?: continue
                    val trackUuid = p.optString("track_uuid", "")
                    val track = trackList.firstOrNull { trackUuid.isNotBlank() && it.uuid == trackUuid }
                        ?: trackList.firstOrNull { it.id == p.optInt("track", -1) }
                    val lapTimes = ArrayList<Double>()
                    p.optJSONArray("lap_times")?.let { a -> for (i in 0 until a.length()) lapTimes += a.optDouble(i, 0.0) }
                    val trackPayload = trackEntities.firstOrNull {
                        it.uuid == trackUuid || (track != null && it.localKey == track.id.toString())
                    }?.payload
                    val lapTraces = localSessionTraces(e, trackPayload)
                    if (lapTraces.isNotEmpty()) traces[slot] = lapTraces
                    sessionList += RaceSession(
                        slot = slot,
                        id = p.optLong("id", 0L),
                        trackSlot = track?.id ?: p.optInt("track", -1),
                        trackName = p.optString("track_name", track?.name ?: ""),
                        startEpoch = p.optLong("start", 0L),
                        durationMs = p.optLong("duration_ms", 0L),
                        samples = p.optLong("samples", 0L),
                        laps = p.optInt("laps", lapTimes.size),
                        deletedLapMask = p.optInt("deleted_laps", 0),
                        lapTimes = lapTimes,
                        uuid = p.optString("session_uuid", e.uuid),
                        trackUuid = trackUuid,
                        trackRevisionId = p.optString("track_revision_id", ""),
                        trackVersion = p.optInt("track_version", 0),
                        endEpoch = p.optLong("end", 0L),
                        sampleSize = p.optInt("sample_size", 16),
                        lapIds = run {
                            val out = ArrayList<String>()
                            p.optJSONArray("lap_ids")?.let { a -> for (i in 0 until a.length()) out += a.optString(i, "") }
                            out
                        },
                    )
                }
                Triple(trackList, details, sessionList to traces)
            }.onSuccess { (t, d, s) ->
                main.post {
                    tracks = t
                    trackDetailCache.clear()
                    trackDetailCache.putAll(d)
                    sessions = s.first
                    traceCache.clear()
                    traceCache.putAll(s.second)
                    sessionLapCache.clear()
                    logs = emptyList()
                    filterTrack = null
                    localMode = true
                    pbLoad.visibility = View.GONE
                    status("Local App：${tracks.size} Tracks / ${sessions.size} Race Logs")
                    renderTracks()
                    val gen = ++generation
                    loadSatellites(gen, tracks, trackDetailCache, false, null)
                }
            }.onFailure { e ->
                main.post {
                    localMode = false
                    fail("Local 数据读取失败：${e.message}")
                }
            }
        }
    }

    private fun localPoints(p: JSONObject): List<android.graphics.PointF> {
        val arr = p.optJSONArray("points") ?: return emptyList()
        val out = ArrayList<android.graphics.PointF>()
        var i = 0
        if (arr.length() > 0 && arr.opt(0) is JSONArray) {
            for (j in 0 until arr.length()) {
                val q = arr.optJSONArray(j) ?: continue
                if (q.length() >= 2) out += android.graphics.PointF(q.optDouble(0).toFloat(), q.optDouble(1).toFloat())
            }
        } else {
            while (i + 1 < arr.length()) {
                out += android.graphics.PointF(arr.optDouble(i).toFloat(), arr.optDouble(i + 1).toFloat())
                i += 2
            }
        }
        return out
    }

    private fun localDetail(p: JSONObject, points: List<android.graphics.PointF>): TrackDetail? {
        if (points.size < 2) return null
        val ns = p.optDouble("nscale", 0.0)
        val geo = if (p.optBoolean("geo", false) && ns > 1e-9) TrackGeoRef(
            p.optDouble("glat0", 0.0), p.optDouble("glon0", 0.0), ns,
            p.optDouble("nox", 0.0), p.optDouble("noy", 0.0),
        ) else null
        val sections = ArrayList<Double>()
        p.optJSONArray("sections")?.let { a -> for (i in 0 until a.length()) sections += a.optDouble(i, 0.0) }
        return TrackDetail(
            name = p.optString("name", ""),
            lengthM = p.optDouble("length_m", 0.0),
            widthM = p.optDouble("width", 5.0),
            startWidthM = p.optDouble("start_width", p.optDouble("width", 5.0)),
            startF = p.optDouble("start_f", 0.0),
            startOffsetM = p.optDouble("start_offset", 0.0),
            reverse = p.optBoolean("reverse", false),
            points = listOf(TrackMapView.Layer(points, 0xFF66E0D2.toInt())),
            geoRef = geo,
            sections = sections,
        )
    }

    private fun localSessionTraces(e: SyncLocalDb.LocalEntity, track: JSONObject?): Map<Int, TraceData> {
        val file = e.blobPath?.let { File(it) } ?: return emptyMap()
        if (!file.isFile) return emptyMap()
        val csv = runCatching {
            GZIPInputStream(file.inputStream()).bufferedReader().use { it.readText() }
        }.getOrNull() ?: return emptyMap()
        val groups = LinkedHashMap<Int, MutableList<android.graphics.PointF>>()
        val speeds = HashMap<Int, MutableList<Double>>()
        val times = HashMap<Int, MutableList<Long>>()
        var lat0 = Double.NaN
        var lon0 = 0.0
        for (line in csv.lineSequence()) {
            val p = line.split(',')
            if (p.size < 7) continue
            val lap = p[2].trim().toIntOrNull() ?: continue
            if (lap <= 0) continue
            val elapsed = p[3].trim().toDoubleOrNull() ?: continue
            val lat = p[4].trim().toDoubleOrNull() ?: continue
            val lon = p[5].trim().toDoubleOrNull() ?: continue
            val kmh = p[6].trim().toDoubleOrNull() ?: 0.0
            if (lat == 0.0 && lon == 0.0) continue
            if (lat0.isNaN()) { lat0 = lat; lon0 = lon }
            val q = localGeoNorm(track, lat, lon, lat0, lon0)
            groups.getOrPut(lap) { ArrayList() } += q
            speeds.getOrPut(lap) { ArrayList() } += kmh
            times.getOrPut(lap) { ArrayList() } += (elapsed * 1000.0).toLong()
        }
        val out = LinkedHashMap<Int, TraceData>()
        for ((lap, pts) in groups) {
            if (pts.size < 2) continue
            val step = (pts.size / 360).coerceAtLeast(1)
            val rp = ArrayList<android.graphics.PointF>()
            val rs = ArrayList<Double>()
            val rt = ArrayList<Long>()
            var i = 0
            while (i < pts.size) {
                rp += pts[i]
                speeds[lap]?.getOrNull(i)?.let { rs += it }
                times[lap]?.getOrNull(i)?.let { rt += it }
                i += step
            }
            if (rp.size >= 2) out[lap] = TraceData(rp, rs, rt)
        }
        return out
    }

    private fun localGeoNorm(track: JSONObject?, lat: Double, lon: Double, lat0: Double, lon0: Double): android.graphics.PointF {
        if (track != null && track.optBoolean("geo", false)) {
            val ns = track.optDouble("nscale", 0.0)
            if (ns > 1e-9) {
                val tl0 = track.optDouble("glat0", 0.0)
                val to0 = track.optDouble("glon0", 0.0)
                val mx = (lon - to0) * 111320.0 * kotlin.math.cos(Math.toRadians((lat + tl0) / 2.0))
                val my = (lat - tl0) * 111320.0
                return android.graphics.PointF(
                    (mx * ns + track.optDouble("nox", 0.0)).toFloat(),
                    (my * ns + track.optDouble("noy", 0.0)).toFloat(),
                )
            }
        }
        val x = (lon - lon0) * 111320.0 * kotlin.math.cos(Math.toRadians(lat0))
        val y = (lat - lat0) * 111320.0
        return android.graphics.PointF(x.toFloat(), y.toFloat())
    }

    private fun loadAll() {
        localMode = false
        direct.bindCurrentEsp()
        val gen = ++generation
        val b = base()
        val directBound = direct.isConnected
        val internetNetwork = direct.internetNetwork
        showBusy("正在同步设备数据", "正在读取赛道、Race Log 和原始场次，请稍候…", progress = true)
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
                val tc = HashMap<Int, Map<Int, TraceData>>()
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
                    updateBusy(65, "正在加载卫星图…")
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
                    hideBusy()
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
            val p = progress.coerceIn(0, 100)
            pbLoad.visibility = View.VISIBLE
            pbLoad.progress = p
            updateBusy(p, text)
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
                    val p = (65 + 35 * loaded / geo.size).coerceIn(65, 99)
                    pbLoad.visibility = View.VISIBLE
                    pbLoad.progress = p
                    updateBusy(p, "正在加载卫星图 $loaded/${geo.size}")
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
        hideBusy()
        if (!offlineMode && discoveredDevice != null) autoUploadSatellites()
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
                    "${formatLength(t.lengthM)}  ·  起始线 ${String.format(Locale.US, "%.1f", t.startWidthM)}m  ·  " +
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
                    styleStartLine(this, t, detail)
                    satelliteCache[t.id]?.let { setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds)) }
                    setOnClickListener { showTrackPreview(t, detail) }
                }
                card.addView(preview)
            }
            if (!offlineMode && t.geo && satelliteCache[t.id] == null) {
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
                if (offlineMode) {
                    row(
                        button("查看地图") { showTrackPreview(t, trackDetailCache[t.id]) },
                        button("Race Log") {
                            filterTrack = t.id
                            showingSessions = true
                            showingLogs = false
                            renderSessions()
                        },
                    )
                } else {
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
                }
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
            if (!offlineMode) header.addView(
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
                        TrackMapView.Layer(e.value.points, cols[i % cols.size], "L${e.key}")
                    })
                    trackForSession(s)?.let { styleStartLine(this, it) }
                    satelliteForSession(s)?.let {
                        setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
                    }
                })
            }
            if (s.deletedLapMask != 0) {
                card.addView(meta("已删除圈：${deletedLaps(s)}", warning = true))
            }
            card.addView(
                if (offlineMode) {
                    row(button("详情") { showSessionDetail(s) })
                } else {
                    row(
                        button("详情") { showSessionDetail(s) },
                        button("导出 RaceChrono") { exportSession(s) },
                        button("删除", danger = true) { confirmDeleteSession(s) },
                    )
                }
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
                if (offlineMode) {
                    row(button("圈速详情") { showLogDetail(l) })
                } else {
                    row(
                        button("圈速详情") { showLogDetail(l) },
                        button("关联到赛道") { associateLog(l) },
                    )
                }
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
        val d = AlertDialog.Builder(this)
            .setTitle(Lang.tr("把 Race Log 关联到赛道"))
            .setItems(names) { _, which ->
                val target = tracks[which].name
                if (target == l.trackName) return@setItems
                val b = base()
                worker.execute {
                    runCatching { RaceApi.renameLogs(b, l.trackName, target) }
                        .onSuccess {
                            main.post {
                                markCacheDirty()
                                toast("Race Log 已关联到 $target")
                                loadAll()
                            }
                        }
                        .onFailure { e -> main.post { fail("关联失败：${e.message}") } }
                }
            }
            .setNegativeButton(Lang.tr("取消"), null)
            .create()
        d.show()
        Lang.localizeDialog(d)
    }

    private fun showLogDetail(l: RaceLogEntry) {
        if (offlineMode) {
            val dialog = buildLogDetailDialog(RaceLogDetail(l, logTraceCache[l.index].orEmpty()))
            dialog.show()
            Lang.localizeDialog(dialog)
            return
        }
        val b = base()
        status("正在读取 Race Log 轨迹…")
        worker.execute {
            runCatching { RaceApi.logDetail(b, l.index) }
                .onSuccess { d -> main.post {
                    if (!isFinishing) {
                        val dialog = buildLogDetailDialog(d)
                        dialog.show()
                        Lang.localizeDialog(dialog)
                    }
                } }
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
                    styleStartLine(this, tr)
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
        if (offlineMode || localMode) {
            val laps = sessionLapCache[summary.slot].orEmpty()
            val traces = traceCache[summary.slot].orEmpty()
            val dialog = buildDetailDialog(summary, laps, traces)
            dialog.show()
            Lang.localizeDialog(dialog)
            return
        }
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
                main.post {
                    if (!isFinishing) {
                        val dialog = buildDetailDialog(s, laps, traces)
                        dialog.show()
                        Lang.localizeDialog(dialog)
                    }
                }
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
        traces: Map<Int, TraceData>,
        colors: IntArray,
        best: Int?,
    ): List<TrackMapView.Layer> {
        val layers = ArrayList<TrackMapView.Layer>()
        for ((idx, lap) in traces.keys.sorted().withIndex()) {
            if (lap !in selected) continue
            val data = traces[lap] ?: continue
            layers += TrackMapView.Layer(
                data.points,
                if (lap == best) 0xFFFFD700.toInt() else colors[idx % colors.size],
                "L$lap",
            )
        }
        return layers
    }

    private fun accelerationSegments(data: TraceData): IntArray? {
        val points = data.points
        val speeds = data.speeds
        val times = data.times
        if (points.size < 2 || speeds.size < 2 || times.size < 2) return null
        val neutral = 0xFF62727A.toInt()
        val out = IntArray(points.size - 1) { neutral }
        val n = minOf(points.size, speeds.size, times.size)
        for (i in 1 until n) {
            val dt = (times[i] - times[i - 1]) / 1000.0
            if (dt <= 0.0) continue
            val a = (speeds[i] - speeds[i - 1]) / dt
            out[i - 1] = when {
                a > 2.0 -> 0xFF00E676.toInt()
                a > 0.7 -> 0xFF2E7D32.toInt()
                a < -2.0 -> 0xFFFF1744.toInt()
                a < -0.7 -> 0xFFB71C1C.toInt()
                else -> neutral
            }
        }
        return out
    }

    private fun maxSpeedMarker(data: TraceData): TrackMapView.Marker? {
        if (data.points.isEmpty() || data.speeds.isEmpty()) return null
        val n = minOf(data.points.size, data.speeds.size)
        if (n <= 0) return null
        var idx = 0
        for (i in 1 until n) if (data.speeds[i] > data.speeds[idx]) idx = i
        return TrackMapView.Marker(
            data.points[idx], 0xFFFFB300.toInt(),
            String.format(Locale.US, "MAX %.0f km/h", data.speeds[idx]),
        )
    }

    private fun buildDetailDialog(
        s: RaceSession,
        laps: List<LapDetail>,
        traces: Map<Int, TraceData>,
    ): AlertDialog {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        content.addView(meta("${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  ${s.samples} 点"))
        val usable = laps.filter { !it.deleted && it.time > 0 }
        val positiveDist = usable.map { it.distanceM }.filter { it > 0.0 }.sorted()
        val typicalDist = if (positiveDist.isNotEmpty()) positiveDist[(positiveDist.size - 1) / 2] else 0.0
        val complete = usable.filter {
            typicalDist <= 0.0 || (it.distanceM >= typicalDist * 0.70 && it.distanceM <= typicalDist * 1.30)
        }
        val candidates = (if (complete.isNotEmpty()) complete else usable)
            .filter { traces.isEmpty() || traces.containsKey(it.lap) }
        val best = (candidates.ifEmpty { complete.ifEmpty { usable } }).minByOrNull { it.time }?.lap
        if (traces.isNotEmpty()) {
            val map = TrackMapView(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(220),
                )
                trackForSession(s)?.let { styleStartLine(this, it) }
                satelliteForSession(s)?.let {
                    setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
                }
            }
            val selected = HashSet<Int>()
            val checkViews = LinkedHashMap<Int, CheckBox>()
            val legend = TextView(this).apply {
                setTextColor(0xFFA9C4BE.toInt())
                textSize = 12f
                setPadding(0, dp(6), 0, 0)
            }
            val highContrast = intArrayOf(
                0xFFFF3B30.toInt(), 0xFF2979FF.toInt(), 0xFFFFD60A.toInt(),
                0xFF34C759.toInt(), 0xFFFF2D95.toInt(), 0xFF32D4FF.toInt(),
                0xFFFF9F0A.toInt(), 0xFFAF52DE.toInt(),
            )
            fun colorMapFor(sel: Set<Int>): Map<Int, Int> {
                val order = sel.sorted()
                val pal = when (order.size) {
                    2 -> intArrayOf(0xFFFF3B30.toInt(), 0xFF2979FF.toInt())
                    3 -> intArrayOf(0xFFFF3B30.toInt(), 0xFFFFD60A.toInt(), 0xFF2979FF.toInt())
                    else -> highContrast
                }
                return order.mapIndexed { i, lap -> lap to pal[i % pal.size] }.toMap()
            }
            fun textOn(color: Int): Int {
                val r = Color.red(color) / 255.0
                val g = Color.green(color) / 255.0
                val b = Color.blue(color) / 255.0
                return if (0.299 * r + 0.587 * g + 0.114 * b > 0.62) 0xFF101010.toInt()
                else 0xFFFFFFFF.toInt()
            }
            fun updateChecks(colors: Map<Int, Int>) {
                for ((lap, cb) in checkViews) {
                    val on = selected.contains(lap)
                    val c = colors[lap] ?: 0xFF62727A.toInt()
                    val fill = if (on) (c and 0x00FFFFFF) or 0x66000000 else 0x332F3A40
                    cb.background = GradientDrawable().apply {
                        cornerRadius = dp(5).toFloat()
                        setColor(fill)
                        if (lap == best) setStroke(dp(2), 0xFF8B5CF6.toInt())
                    }
                    cb.setTextColor(if (on) textOn(c) else 0xFF8A9A9A.toInt())
                    cb.setTypeface(Typeface.DEFAULT, if (lap == best) Typeface.BOLD else Typeface.NORMAL)
                    cb.buttonTintList = ColorStateList.valueOf(
                        if (on) textOn(c) else 0xFF8A9A9A.toInt()
                    )
                    cb.setPadding(dp(8), dp(3), dp(8), dp(3))
                }
            }
            fun refreshMap() {
                map.setMarker(null)
                val colors = colorMapFor(selected)
                if (selected.size == 1) {
                    val lap = selected.first()
                    val data = traces[lap]
                    if (data != null) {
                        val seg = accelerationSegments(data)
                        val base = if (lap == best) 0xFFFFD700.toInt() else 0xFFE6F5F1.toInt()
                        map.setLayers(listOf(TrackMapView.Layer(data.points, base, "L$lap", seg)))
                        map.setMarker(maxSpeedMarker(data))
                        legend.text = if (data.speeds.isNotEmpty()) {
                            "L$lap 单选：绿=加速，红=减速，黄点=最高速 ${String.format(Locale.US, "%.0f", data.speeds.maxOrNull() ?: 0.0)} km/h"
                        } else {
                            "L$lap 单选：该圈没有速度数据"
                        }
                        updateChecks(colors)
                        return
                    }
                }
                map.setLayers(selected.sorted().mapNotNull { lap ->
                    val data = traces[lap] ?: return@mapNotNull null
                    TrackMapView.Layer(data.points, colors[lap] ?: 0xFF66E0D2.toInt(), "L$lap")
                })
                legend.text = "可勾选单圈或多圈叠加；单选时显示加减速颜色和最高速位置。"
                updateChecks(colors)
            }
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
                    checkViews[lap] = this
                    setOnCheckedChangeListener { _, checked ->
                        if (checked) selected.add(lap) else selected.remove(lap)
                        refreshMap()
                    }
                })
            }
            content.addView(HorizontalScrollView(this).apply { addView(checks) })
            content.addView(legend)
            content.addView(meta("绿=加速  红=减速  黄点=最高速"))
            selected.addAll(traces.keys)
            refreshMap()
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
            widthM = detail.startWidthM.toFloat()
            trackLengthM = detail.lengthM.toFloat()
            startOffsetM = detail.startOffsetM.toFloat()
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

        addLabel("起始线宽度")
        val tvWidth = TextView(this).apply {
            text = String.format(Locale.US, "%.1f m", detail.startWidthM)
            setTextColor(0xFF66E0D2.toInt())
            textSize = 18f
            setTypeface(typeface, Typeface.BOLD)
        }
        box.addView(tvWidth)
        val sbWidth = SeekBar(this).apply {
            max = 490
            progress = ((detail.startWidthM - 1.0) * 10.0).toInt().coerceIn(0, 490)
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

        addLabel("起点左右偏移（负=左，正=右）")
        val tvOffset = TextView(this).apply {
            text = String.format(Locale.US, "%+.1f m", detail.startOffsetM)
            setTextColor(0xFF66E0D2.toInt())
            textSize = 18f
            setTypeface(typeface, Typeface.BOLD)
        }
        box.addView(tvOffset)
        val sbOffset = SeekBar(this).apply {
            max = 400
            progress = ((detail.startOffsetM + 20.0) * 10.0).toInt().coerceIn(0, 400)
        }
        sbOffset.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val v = progress / 10.0 - 20.0
                tvOffset.text = String.format(Locale.US, "%+.1f m", v)
                map.startOffsetM = v.toFloat()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
        box.addView(sbOffset)

        val swReverse = Switch(this).apply {
            text = "反向起点方向（CCW）"
            isChecked = t.reverse
            setOnCheckedChangeListener { _, checked -> map.reverse = checked }
        }
        box.addView(swReverse)
        box.addView(TextView(this).apply {
            text = "点击保存只写入并确认这一条赛道，不重新拉取全部数据。"
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
                val startWidthM = 1.0 + sbWidth.progress / 10.0
                val offsetM = sbOffset.progress / 10.0 - 20.0
                if (name.isEmpty()) {
                    toast("请填写名称")
                    return@setOnClickListener
                }
                val b = base()
                status("正在保存赛道设置…")
                worker.execute {
                    runCatching {
                        val result = RaceApi.updateTrack(
                            b, t.id, name, startPct / 100.0,
                            swReverse.isChecked, startWidthM, offsetM,
                        )
                        if (t.uuid.isNotBlank() && result.uuid.isNotBlank() &&
                            result.uuid != t.uuid) {
                            throw IllegalStateException("设备返回了不同的 Track UUID")
                        }
                        val sat = satelliteCache[t.id]
                        val newId = if (result.id >= 0) result.id else {
                            RaceApi.tracks(b).firstOrNull {
                                (t.uuid.isNotBlank() && it.uuid == t.uuid) ||
                                    (t.uuid.isBlank() && it.name == name)
                            }?.id ?: throw IllegalStateException("设备未确认保存结果")
                        }
                        val localUuid = t.uuid.ifBlank {
                            tracks.firstOrNull { it.id == t.id }?.uuid.orEmpty()
                        }
                        if (localUuid.isNotBlank()) {
                            val newVersion = if (result.revision > 0) result.revision else t.revision
                            runCatching {
                                localDb.applyTrackCommit(
                                    localUuid, newId, name, startPct / 100.0,
                                    startWidthM, offsetM, swReverse.isChecked,
                                    newVersion, result.revisionId,
                                )
                            }
                        }
                        if (sat != null && newId != t.id) {
                            satelliteUploadWorker.execute {
                                runCatching {
                                    RaceApi.uploadTrackImage(
                                        b, newId, sat.pal16,
                                        sat.bounds.left, sat.bounds.top,
                                        sat.bounds.right, sat.bounds.bottom,
                                    )
                                }
                            }
                        }
                        result
                    }.onSuccess { result ->
                        main.post {
                            dialog.dismiss()
                            markCacheDirty()
                            val newId = if (result.id >= 0) result.id else t.id
                            val updated = t.copy(
                                id = newId,
                                name = name,
                                startF = startPct / 100.0,
                                startWidthM = startWidthM,
                                startOffsetM = offsetM,
                                reverse = swReverse.isChecked,
                                uuid = result.uuid.ifBlank { t.uuid },
                                revision = if (result.revision > 0) result.revision else t.revision,
                                revisionId = result.revisionId.ifBlank { t.revisionId },
                            )
                            val idx = tracks.indexOfFirst { it.uuid == t.uuid }
                            tracks = if (idx >= 0) {
                                tracks.toMutableList().also { it[idx] = updated }
                            } else {
                                tracks.map { if (it.id == t.id) updated else it }
                            }
                            trackDetailCache.remove(t.id)
                            trackDetailCache[newId] = detail.copy(
                                name = name,
                                startF = startPct / 100.0,
                                startWidthM = startWidthM,
                                startOffsetM = offsetM,
                                reverse = swReverse.isChecked,
                            )
                            val sat = satelliteCache.remove(t.id)
                            if (sat != null) satelliteCache[newId] = sat
                            if (filterTrack == t.id) filterTrack = newId
                            status("赛道已同步：${name} · 版本 ${updated.revision}（只确认这一条）")
                            toast("单条赛道已保存，无需全量刷新")
                            when {
                                showingLogs -> renderLogs()
                                showingSessions -> renderSessions()
                                else -> renderTracks()
                            }
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
            styleStartLine(this, t, detail)
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
            .apply { if (!offlineMode) setNeutralButton("同步到设备", null) }
            .create()
        if (!offlineMode) dialog.setOnShowListener {
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
                val uid = discoveredDevice?.uid ?: LocalCache.uid(this) ?: "manual"
                SatelliteUploadCache.markUploaded(this, uid, t.id, satellite)
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
            showBusy("正在删除赛道", "正在从设备删除「${t.name}」…")
            worker.execute {
                runCatching { RaceApi.deleteTrack(b, t.id) }
                    .onSuccess {
                        main.post {
                            if (gen != generation) return@post
                            hideBusy()
                            tracks = tracks.filterNot { it.id == t.id }
                            trackDetailCache.remove(t.id)
                            satelliteCache.remove(t.id)
                            localDb.markTrackDeleted(t.uuid)
                            if (filterTrack == t.id) filterTrack = null
                            markCacheDirty()
                            toast("已删除赛道")
                            when {
                                showingLogs -> renderLogs()
                                showingSessions -> renderSessions()
                                else -> renderTracks()
                            }
                        }
                    }
                    .onFailure { e ->
                        main.post {
                            if (gen != generation) return@post
                            hideBusy()
                            fail("删除失败：${e.message}")
                        }
                    }
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
                    val slots = list.map { it.slot }.toSet()
                    for (s in list) localDb.markSessionDeleted(s.uuid)
                    sessions = sessions.filterNot { it.slot in slots }
                    for (slot in slots) {
                        traceCache.remove(slot)
                        sessionLapCache.remove(slot)
                    }
                    markCacheDirty()
                    if (failed.isEmpty()) {
                        status("已删除 ${list.size} 条 Race Log（仅同步删除动作）")
                        toast("已清空 ${list.size} 条 Race Log")
                    } else {
                        fail("部分删除失败：${failed.first()}")
                    }
                    renderSessions()
                }
            }
        }
    }

    private fun confirmDeleteSession(s: RaceSession) {
        confirm("删除 Race Log", "从设备删除这条 Race Log？只同步删除动作，不修改原始数据。") {
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteSession(b, s.slot) }
                    .onSuccess {
                        main.post {
                            localDb.markSessionDeleted(s.uuid)
                            sessions = sessions.filterNot { it.slot == s.slot }
                            traceCache.remove(s.slot)
                            sessionLapCache.remove(s.slot)
                            markCacheDirty()
                            status("Race Log 已删除：${s.trackName}（仅同步删除动作）")
                            toast("已删除 Race Log")
                            renderSessions()
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
        tvStatus.text = Lang.tr(text)
        tvStatus.setTextColor(0xFFA9C4BE.toInt())
    }

    private fun fail(text: String) {
        tvStatus.text = Lang.tr(text)
        tvStatus.setTextColor(0xFFFF9B9B.toInt())
    }

    private fun toast(text: String) {
        Toast.makeText(this, Lang.tr(text), Toast.LENGTH_SHORT).show()
    }

    private fun showBusy(title: String, message: String, progress: Boolean = false) {
        if (isFinishing) return
        hideBusy()
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(16), dp(20), dp(20))
        }
        busyText = TextView(this).apply {
            text = Lang.tr(message)
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 13f
        }
        box.addView(busyText)
        busyProgress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            isIndeterminate = !progress
            max = 100
            this.progress = 0
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(12) }
        }
        box.addView(busyProgress)
        busyDialog = AlertDialog.Builder(this)
            .setTitle(Lang.tr(title))
            .setView(box)
            .setCancelable(false)
            .create()
            .also {
                it.setCanceledOnTouchOutside(false)
                it.show()
                Lang.localizeDialog(it)
            }
    }

    private fun updateBusy(progress: Int, message: String? = null) {
        busyProgress?.let {
            it.isIndeterminate = false
            it.progress = progress.coerceIn(0, 100)
        }
        if (message != null) busyText?.text = Lang.tr(message)
    }

    private fun hideBusy() {
        busyDialog?.dismiss()
        busyDialog = null
        busyProgress = null
        busyText = null
    }

    private fun confirm(title: String, message: String, action: () -> Unit) {
        val d = AlertDialog.Builder(this)
            .setTitle(Lang.tr(title))
            .setMessage(Lang.tr(message))
            .setNegativeButton(Lang.tr("取消"), null)
            .setPositiveButton(Lang.tr("确认")) { _, _ -> action() }
            .create()
        d.show()
        Lang.localizeDialog(d)
    }

    private fun offlineModeUi() {
        findViewById<Button>(R.id.btnRaceDirect).visibility = View.GONE
        findViewById<Button>(R.id.btnRaceLoad).visibility = View.GONE
        findViewById<Button>(R.id.btnRaceCloud).visibility = View.GONE
        findViewById<Button>(R.id.btnRaceSat).visibility = View.GONE
        etUrl.visibility = View.GONE
        pbLoad.visibility = View.GONE
    }

    private fun loadCachedSnapshot() {
        val snap = LocalCache.loadSnapshot(this)
        if (snap == null) {
            status("没有本地缓存；联网完成一次云端同步后即可离线查看")
            renderTracks()
            return
        }
        tracks = parseCacheTracks(snap)
        sessions = parseCacheSessions(snap)
        logs = parseCacheLogs(snap)
        sessionLapCache.clear()
        val sIn = snap.optJSONArray("sessions") ?: JSONArray()
        for (i in 0 until sIn.length()) {
            val o = sIn.getJSONObject(i)
            val slot = o.optInt("local_slot", -1)
            val summary = o.optJSONObject("summary") ?: continue
            val la = summary.optJSONArray("laps") ?: continue
            val laps = ArrayList<LapDetail>()
            val traces = LinkedHashMap<Int, TraceData>()
            for (j in 0 until la.length()) {
                val l = la.getJSONObject(j)
                val lap = l.optInt("lap")
                laps += LapDetail(
                    lap = lap, time = l.optDouble("time", 0.0),
                    samples = l.optLong("samples", 0), distanceM = l.optDouble("distance_m", 0.0),
                    avgKmh = l.optDouble("avg_kmh", 0.0), maxKmh = l.optDouble("max_kmh", 0.0),
                    deleted = false,
                )
                val pts = ArrayList<android.graphics.PointF>()
                val sp = ArrayList<Double>()
                val tm = ArrayList<Long>()
                val pa = l.optJSONArray("points")
                if (pa != null) for (k in 0 until pa.length()) {
                    val q = pa.optJSONArray(k) ?: continue
                    if (q.length() < 2) continue
                    pts += android.graphics.PointF(q.optDouble(0).toFloat(), q.optDouble(1).toFloat())
                    if (q.length() > 2) sp += q.optDouble(2, 0.0)
                    if (q.length() > 3) tm += (q.optDouble(3, 0.0) * 1000).toLong()
                }
                if (pts.size >= 2) traces[lap] = TraceData(pts, sp, tm)
            }
            if (slot >= 0) sessionLapCache[slot] = laps
            if (traces.isNotEmpty()) traceCache[slot] = traces
        }
        renderTracks()
        val name = LocalCache.uid(this) ?: "本地"
        status("离线缓存 · $name · ${tracks.size} 赛道 / ${sessions.size} Race Log（只读）")
    }

    private fun parseCacheTracks(snap: JSONObject): List<RaceTrack> {
        val out = ArrayList<RaceTrack>()
        val arr = snap.optJSONArray("tracks") ?: return out
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val p = o.optJSONObject("payload") ?: JSONObject()
            val id = o.optInt("local_id", -1)
            val nscale = p.optDouble("nscale", 0.0)
            out += RaceTrack(
                id = id,
                name = p.optString("name", ""),
                geo = p.optBoolean("geo", false),
                lengthM = p.optDouble("length_m", 0.0),
                sections = p.optJSONArray("sections")?.length() ?: 0,
                widthM = p.optDouble("width", 5.0),
                startWidthM = p.optDouble("start_width", p.optDouble("width", 5.0)),
                startF = p.optDouble("start_f", 0.0),
                startOffsetM = p.optDouble("start_offset", 0.0),
                reverse = p.optBoolean("reverse", false),
            )
            val pts = ArrayList<android.graphics.PointF>()
            val pa = p.optJSONArray("points")
            if (pa != null) for (j in 0 until pa.length()) {
                val q = pa.optJSONArray(j) ?: continue
                if (q.length() >= 2) pts += android.graphics.PointF(q.optDouble(0).toFloat(), q.optDouble(1).toFloat())
            }
            val geoRef = if (p.optBoolean("geo", false) && nscale > 1e-9) TrackGeoRef(
                p.optDouble("glat0", 0.0), p.optDouble("glon0", 0.0), nscale,
                p.optDouble("nox", 0.0), p.optDouble("noy", 0.0),
            ) else null
            val sections = ArrayList<Double>()
            p.optJSONArray("sections")?.let { a -> for (j in 0 until a.length()) sections += a.optDouble(j, 0.0) }
            trackDetailCache[id] = TrackDetail(
                name = p.optString("name", ""), lengthM = p.optDouble("length_m", 0.0),
                widthM = p.optDouble("width", 5.0),
                startWidthM = p.optDouble("start_width", p.optDouble("width", 5.0)),
                startF = p.optDouble("start_f", 0.0), startOffsetM = p.optDouble("start_offset", 0.0),
                reverse = p.optBoolean("reverse", false),
                points = listOf(TrackMapView.Layer(pts, 0xFF66E0D2.toInt())),
                geoRef = geoRef, sections = sections,
            )
        }
        return out
    }

    private fun parseCacheSessions(snap: JSONObject): List<RaceSession> {
        val out = ArrayList<RaceSession>()
        val arr = snap.optJSONArray("sessions") ?: return out
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val p = o.optJSONObject("payload") ?: JSONObject()
            val times = ArrayList<Double>()
            p.optJSONArray("lap_times")?.let { a -> for (j in 0 until a.length()) times += a.optDouble(j, 0.0) }
            out += RaceSession(
                slot = o.optInt("local_slot", -1), id = p.optLong("id", 0L),
                trackSlot = p.optInt("track", o.optInt("track_local_id", -1)),
                trackName = p.optString("track_name", o.optString("track_name", "")),
                startEpoch = p.optLong("start", o.optLong("start_epoch", 0L)),
                durationMs = p.optLong("duration_ms", 0L),
                samples = p.optLong("samples", 0L), laps = p.optInt("laps", 0),
                deletedLapMask = p.optInt("deleted_laps", 0), lapTimes = times,
            )
        }
        return out
    }

    private fun parseCacheLogs(snap: JSONObject): List<RaceLogEntry> {
        val out = ArrayList<RaceLogEntry>()
        val arr = snap.optJSONArray("logs") ?: return out
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val p = o.optJSONObject("payload") ?: JSONObject()
            val times = ArrayList<Double>()
            p.optJSONArray("lap_times")?.let { a -> for (j in 0 until a.length()) times += a.optDouble(j, 0.0) }
            val idx = o.optInt("local_index", -1)
            out += RaceLogEntry(
                index = idx, trackName = p.optString("track_name", o.optString("track_name", "")),
                date = p.optString("date", ""), mode = p.optInt("mode", 0),
                durationS = p.optDouble("duration", 0.0), laps = p.optInt("laps", 0),
                bestS = p.optDouble("best", 0.0), bestIndex = p.optInt("best_index", 0),
                hasBest = p.optInt("has_best", 0) != 0, vehicle = p.optInt("vehicle", 0),
                lapTimes = times,
            )
            val trace = ArrayList<android.graphics.PointF>()
            p.optJSONArray("trace")?.let { a ->
                var j = 0
                while (j + 1 < a.length()) {
                    trace += android.graphics.PointF(a.optDouble(j).toFloat(), a.optDouble(j + 1).toFloat())
                    j += 2
                }
            }
            if (idx >= 0) logTraceCache[idx] = trace
        }
        return out
    }

    private fun checkHotspot() {
        if (!hotspotWatching || hotspotChecking || direct.isConnected || isFinishing) return
        hotspotChecking = true
        val saved = RaceApi.baseUrl(this)
        cloudWorker.execute {
            val found = HotspotDiscovery.find(saved, 3200)
            main.post {
                hotspotChecking = false
                if (isFinishing || !hotspotWatching) return@post
                if (found == null) {
                    hotspotMisses++
                    if (hotspotMisses >= 3) {
                        discoveredDevice = null
                        lastPromptedUid = null
                        lastPromptedIp = null
                    }
                    return@post
                }
                hotspotMisses = 0
                val changed = discoveredDevice?.uid != found.uid || discoveredDevice?.ip != found.ip
                discoveredDevice = found
                RaceApi.saveBaseUrl(this, found.baseUrl)
                etUrl.setText(found.baseUrl)
                if (changed) {
                    status("发现 RaceBrick ${found.uid} · ${found.ip}")
                    if (!cloudSyncRunning && tracks.isEmpty() && !isDestroyed &&
                        autoSyncUid != found.uid) loadAll()
                }
                if (tracks.isNotEmpty() && satelliteCache.isNotEmpty() && !offlineMode) {
                    autoUploadSatellites()
                }
                autoSyncUid = null
                forceLocalSync = false
            }
        }
    }

    private fun maybePromptCloud(device: DiscoveredDevice) {
        if (lastPromptedUid == device.uid && lastPromptedIp == device.ip) return
        lastPromptedUid = device.uid
        lastPromptedIp = device.ip
        val token = CloudStore.token(this)
        if (token == null) {
            val d = AlertDialog.Builder(this)
                .setTitle("发现 RaceBrick")
                .setMessage("设备已连上手机热点。登录云端账号后，可在本机与设备之间同步 Track 和 Race Log。")
                .setPositiveButton("登录云端") { _, _ -> showCloudAccount() }
                .setNegativeButton("稍后", null)
                .create()
            d.show()
            Lang.localizeDialog(d)
            return
        }
        val at = CloudStore.lastSyncAt(this, device.uid)
        val bytes = CloudStore.lastSyncBytes(this, device.uid)
        val whenText = if (at > 0) fmtCloudTime(at) else "尚未同步"
        val message = "设备：${device.uid}\n上次同步：$whenText\n上次大小：${fmtCloudBytes(bytes)}\n\n云端保留最近 5 次完整同步记录。现在同步 Track / Race Log？"
        val d = AlertDialog.Builder(this)
            .setTitle("与 RaceBrick 同步 Race 信息？")
            .setMessage(message)
            .setPositiveButton("同步") { _, _ -> syncCloud(device) }
            .setNeutralButton("云端账户") { _, _ -> showCloudAccount() }
            .setNegativeButton("稍后", null)
            .create()
        d.show()
        Lang.localizeDialog(d)
    }

    private fun showCloudAccount() {
        if (CloudStore.token(this) == null) showCloudLogin() else showCloudStatus()
    }

    private fun showCloudLogin() {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        val etUser = EditText(this).apply {
            hint = "RaceBrick Cloud 用户名"
            setSingleLine(true)
            setText(CloudStore.username(this@RaceActivity))
        }
        val etPass = EditText(this).apply {
            hint = "密码"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setSingleLine(true)
        }
        box.addView(etUser)
        box.addView(etPass)
        box.addView(TextView(this).apply {
            text = "登录后可把设备数据传到云端，并在网页端查看和修改。"
            setTextColor(0xFF6D817D.toInt())
            textSize = 11f
            setPadding(0, dp(8), 0, 0)
        })
        val d = AlertDialog.Builder(this)
            .setTitle("云端账户")
            .setView(box)
            .setPositiveButton("登录", null)
            .setNeutralButton("注册", null)
            .setNegativeButton("取消", null)
            .create()
        d.setOnShowListener {
            d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                cloudAuth(false, etUser.text.toString().trim(), etPass.text.toString(), d)
            }
            d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                cloudAuth(true, etUser.text.toString().trim(), etPass.text.toString(), d)
            }
        }
        d.show()
        Lang.localizeDialog(d)
    }

    private fun cloudAuth(register: Boolean, username: String, password: String, dialog: AlertDialog) {
        if (username.isEmpty() || password.isEmpty()) {
            toast("请输入用户名和密码")
            return
        }
        status(if (register) "正在注册云端账户…" else "正在登录云端…")
        cloudWorker.execute {
            runCatching {
                if (register) cloudApi.register(username, password)
                else cloudApi.login(username, password)
            }.onSuccess { account ->
                main.post {
                    CloudStore.save(this, account.token, account.username)
                    toast(if (register) "账户已创建" else "云端登录成功")
                    dialog.dismiss()
                    discoveredDevice?.let { device ->
                        if (!cloudSyncRunning) {
                            syncCloud(device, forceLocalSync)
                        }
                    } ?: status("云端已登录；等待设备连接手机热点")
                }
            }.onFailure { e ->
                main.post { fail("云端登录失败：${e.message}") }
            }
        }
    }

    private fun showCloudStatus() {
        val token = CloudStore.token(this) ?: return
        val user = CloudStore.username(this)
        val device = discoveredDevice
        val at = device?.let { CloudStore.lastSyncAt(this, it.uid) } ?: 0L
        val bytes = device?.let { CloudStore.lastSyncBytes(this, it.uid) } ?: 0L
        val msg = "账户：$user\n设备：${device?.uid ?: "未发现设备"}\n上次同步：${if (at > 0) fmtCloudTime(at) else "尚未同步"}\n上次大小：${fmtCloudBytes(bytes)}"
        val d = AlertDialog.Builder(this)
            .setTitle("RaceBrick Cloud")
            .setMessage(msg)
            .setPositiveButton("立即同步", null)
            .setNeutralButton("5次记录", null)
            .setNegativeButton("退出登录", null)
            .create()
        d.setOnShowListener {
            d.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled = device != null
            d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                device?.let { syncCloud(it); d.dismiss() }
            }
            d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                device?.let { showCloudHistory(it.uid) }
            }
            d.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener {
                CloudStore.clear(this)
                toast("已退出云端账户")
                d.dismiss()
            }
        }
        d.show()
        Lang.localizeDialog(d)
    }

    private fun showCloudHistory(uid: String) {
        val token = CloudStore.token(this) ?: return
        status("正在读取云端同步记录…")
        cloudWorker.execute {
            runCatching { cloudApi.history(uid, token) }
                .onSuccess { history ->
                    main.post {
                        val lines = ArrayList<String>()
                        for (i in 0 until history.length()) {
                            val h = history.getJSONObject(i)
                            lines += "${fmtCloudTime(h.optLong("created_at", 0L))}  ${fmtCloudBytes(h.optLong("upload_bytes", 0L))}\n" +
                                "${h.optInt("track_count", 0)} tracks · ${h.optInt("session_count", 0)} sessions · ${h.optInt("log_count", 0)} logs"
                        }
                        val text = if (lines.isEmpty()) "云端还没有同步记录。" else lines.joinToString("\n\n")
                        val d = AlertDialog.Builder(this)
                            .setTitle("最近 5 次云端记录")
                            .setMessage(text)
                            .setPositiveButton("关闭", null)
                            .create()
                        d.show()
                        Lang.localizeDialog(d)
                    }
                }
                .onFailure { e -> main.post { fail("读取云端记录失败：${e.message}") } }
        }
    }

    private fun syncCloud(device: DiscoveredDevice, forceLocal: Boolean = false) {
        if (cloudSyncRunning) return
        val token = CloudStore.token(this)
        if (token == null) {
            showCloudAccount()
            return
        }
        cloudSyncRunning = true
        discoveredDevice = device
        RaceApi.saveBaseUrl(this, device.baseUrl)
        etUrl.setText(device.baseUrl)
        showBusy("正在与云端同步", "正在检查云端版本…", progress = true)
        status("正在与云端同步 ${device.uid}…")
        cloudWorker.execute {
            runCatching {
                CloudSyncManager.sync(this, device.baseUrl, device.uid, token,
                    pullCloud = !forceLocal) { msg, pct ->
                    main.post {
                        if (!isFinishing) {
                            updateBusy(pct, msg)
                            status(msg)
                        }
                    }
                }
            }.onSuccess { r ->
                main.post {
                    cloudSyncRunning = false
                    if (isFinishing) return@post
                    hideBusy()
                    status("云端同步完成：${r.tracks} 赛道 / ${r.logs} Race Log / ${r.sessions} Session，上传 ${fmtCloudBytes(r.uploadBytes.toLong())}")
                    toast("云端同步完成")
                    loadAll()
                }
            }.onFailure { e ->
                main.post {
                    cloudSyncRunning = false
                    if (isFinishing) return@post
                    hideBusy()
                    fail("云端同步失败：${e.message}")
                }
            }
        }
    }

    private fun autoUploadSatellites() {
        uploadAllSatellites(manual = false)
    }

    private fun uploadAllSatellites(manual: Boolean) {
        if (satelliteUploadRunning) return
        val jobs = ArrayList<Pair<RaceTrack, TrackSatellite>>()
        val uid = discoveredDevice?.uid ?: LocalCache.uid(this) ?: "manual"
        for (t in tracks) {
            val sat = satelliteCache[t.id] ?: continue
            if (manual || SatelliteUploadCache.needsUpload(this, uid, t.id, sat)) {
                jobs += t to sat
            }
        }
        if (jobs.isEmpty()) {
            if (manual) toast("卫星图都已是最新")
            return
        }
        if (manual && discoveredDevice == null && RaceApi.baseUrl(this).isBlank()) {
            toast("请先连接设备热点")
            return
        }
        val b = discoveredDevice?.baseUrl ?: base()
        satelliteUploadRunning = true
        if (manual) status("正在上传 ${jobs.size} 张卫星图…")
        satelliteUploadWorker.execute {
            var done = 0
            var failed = 0
            for ((t, sat) in jobs) {
                runCatching {
                    RaceApi.uploadTrackImage(
                        b, t.id, sat.pal16,
                        sat.bounds.left, sat.bounds.top,
                        sat.bounds.right, sat.bounds.bottom,
                    )
                }.onSuccess {
                    SatelliteUploadCache.markUploaded(this, uid, t.id, sat)
                    done++
                    main.post {
                        if (manual) status("正在上传卫星图 $done/${jobs.size}…")
                    }
                }.onFailure {
                    failed++
                }
            }
            main.post {
                satelliteUploadRunning = false
                if (manual) {
                    if (failed == 0) {
                        toast("卫星图已全部同步到设备")
                        status("卫星图同步完成：$done 张")
                    } else {
                        fail("卫星图同步：成功 $done，失败 $failed")
                    }
                } else if (done > 0) {
                    status("卫星图已自动同步到设备：$done 张")
                }
            }
        }
    }

    private fun markCacheDirty() {
        val uid = discoveredDevice?.uid ?: LocalCache.uid(this) ?: return
        LocalCache.markDirty(this, uid)
    }

    private fun fmtCloudTime(epoch: Long): String {
        if (epoch <= 0L) return "尚未同步"
        return SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(epoch * 1000L))
    }

    private fun fmtCloudBytes(bytes: Long): String {
        if (bytes >= 1024L * 1024L) return String.format(Locale.US, "%.2f MB", bytes / 1024.0 / 1024.0)
        if (bytes >= 1024L) return String.format(Locale.US, "%.1f KB", bytes / 1024.0)
        return "$bytes B"
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
        hideBusy()
        direct.disconnect()
        hotspotHandler.removeCallbacks(hotspotTick)
        worker.shutdownNow()
        satelliteWorker.shutdownNow()
        satelliteUploadWorker.shutdownNow()
        cloudWorker.shutdownNow()
        super.onDestroy()
    }
}
