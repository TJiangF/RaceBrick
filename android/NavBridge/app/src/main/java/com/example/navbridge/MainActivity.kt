package com.example.navbridge

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.location.Location
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.SystemClock
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.widget.doAfterTextChanged
import com.amap.api.maps.model.LatLng
import com.amap.api.services.help.Tip
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

class MainActivity : LocalizedActivity() {

    private lateinit var mainScroll: View
    private lateinit var connectGate: View
    private lateinit var floatingStatus: TextView
    private lateinit var btnGateConnect: Button
    private lateinit var tvGateStatus: TextView
    private var sessionActive = false

    private lateinit var tvStatus: TextView
    private lateinit var tvLog: TextView
    private lateinit var etKeyword: EditText
    private lateinit var llTips: LinearLayout
    private lateinit var tvDest: TextView
    private lateinit var ivPreview: ImageView
    private lateinit var btnPreviewMode: Button
    private lateinit var btnStyle: Button
    private lateinit var btnTravel: Button
    private lateinit var btnOrientation: Button
    private lateinit var btnZoomOut: Button
    private lateinit var btnZoomIn: Button
    private lateinit var tvZoom: TextView

    private var zoom = 1f
    private var zoomIndex = 1
    private val zoomLevels = floatArrayOf(0.75f, 1.0f, 1.25f, 1.5f, 2.0f)

    private val frameWorker = Executors.newSingleThreadExecutor()
    private var encodingFrame = false
    private val sendFrames = true
    private var frameId = 0
    private var frameGen = 0L
    private var lastFrameBytes: ByteArray? = null
    private var lastFrameId = 0
    private var lastFrameGen = 0L
    private var lastSentGen = -1L
    private var lastEncodeAt = 0L
    private var lastFrameLogAt = 0L
    private var lastMediaState: Boolean? = null
    private var lastMediaMeta: String? = null
    private var mediaIdentity: String = ""
    private var stableTitle: String = ""
    private var lastRawTitle: String = ""
    private var lastMediaPositionMs: Long = -1L
    private var lastAlbumKey: String? = null
    private var lastArtIdentity: String = ""
    private var lastArtSentIdentity: String = ""
    private var lastArtSrcSig: String = ""
    private var lastArtSkipSig: String = ""
    private var artTxSeq: Int = 0
    private var artHoldUntil: Long = 0L
    private var artAckSeen: Boolean = true
    private var artAckToken: Int = 0
    private var artRetryCount: Int = 0
    private var artWatchdog: Runnable? = null
    private var lastNotifLyric: String = ""
    private var coverRefreshToken: Int = 0
    private var coverRefreshAttempts: Int = 0
    private var coverRefreshPendingToken: Int = -1
    private val coverRefreshTimeout = Runnable {
        val token = coverRefreshPendingToken
        if (token < 0) return@Runnable
        if (coverRefreshAttempts < 5) {
            log("封面刷新超时，重试")
            startCoverRefreshAttempt()
        } else {
            coverRefreshPendingToken = -1
            log("封面刷新失败：设备未确认")
        }
    }
    // System/player title channel is the default lyric source. Set this to
    // true only when you deliberately want LRCLIB/NetEase network lookup.
    private val onlineLyricsEnabled = true
    private var lyricLines: List<LyricsRepository.Line> = emptyList()
    private var systemLyric: String = ""
    private var systemLyricSent: String = ""
    private var lyricKey: String = ""
    private var lyricMode: Int = 0
    private var lyricOffsetMs: Int = 0
    private var forceFirstLyric: Boolean = false
    private var preferNotificationLyric: Boolean = false
    private val lyricModeNames = arrayOf("AUTO", "LRCLIB", "NETEASE", "NOTIF", "TITLE")
    private var lyricSource: String = "NONE"
    private var lastLyricSourceSent: String = ""
    private var lyricWindowKey: String = ""
    private var lyricCleared = true
    private val lyricCache = HashMap<String, LyricsRepository.Result>()
    private var mediaPrompted = false
    private var deviceKmh = -1
    private var deviceGpsAt = 0L

    // Keep the device's media button state in sync with the phone's active session.
    private val mediaStatePump = object : Runnable {
        override fun run() {
            sendMediaState()
            sendMediaProgress()
            sendMediaVolume()
            sendMediaMetadata()
            sendLyricWindow()
            main.postDelayed(this, 1000)
        }
    }

    private var deviceGpsSeen = false
    private var headingUp = true
    private var guideDir = "S"
    private var guideDist = -1
    private var nextRoad = ""
    private var remainS = -1
    private var lights = 0
    private var lightDist = -1
    private var turnIcon: Bitmap? = null
    private var laneBg = IntArray(0)
    private var laneFr = IntArray(0)
    private var cameras: List<NavEngine.CamInfo> = emptyList()
    private var rawTraffic: List<NavEngine.TrafficSeg> = emptyList()
    private var speedLimit = 0

    // Racebrick style: Tianditu vec_w base in WGS-84, AMap route converted to WGS.
    private val tianditu = TiandituTiles()
    private var tiandituEffect: Bitmap? = null
    private var tiandituFrame: GeoFrame? = null
    private var tiandituBlockX0 = 0
    private var tiandituBlockY0 = 0
    private var tiandituGen = 0
    private var tiandituAppliedGen = 0
    private var tiandituIdx: ByteArray? = null
    private var lastRouteCoords: List<LatLng>? = null
    private var lastRoutePtsWgs: DoubleArray? = null
    private var lastRouteWgsFrame: GeoFrame? = null

    private var sdkBase: Bitmap? = null
    private var effectBase: Bitmap? = null
    private var currentPreview: Bitmap? = null
    private var lastRoutePts: DoubleArray? = null
    private var lastX0 = 0
    private var lastY0 = 0
    private var showSdkPreview = false
    private var mapStyle = MapStyles.DEFAULT
    private var travelMode = TravelMode.DEFAULT
    private var lastPreviewAt = 0L

    private lateinit var search: SearchEngine
    private val searchHandler = Handler(Looper.getMainLooper())
    private var searchRunnable: Runnable? = null
    private var suppressSearch = false
    private var selLat = Double.NaN
    private var selLng = Double.NaN
    private var selName = ""

    private lateinit var ble: BleClient
    private var wifiSetup: WifiSetupDialog? = null
    private lateinit var capture: MapCapture
    private lateinit var nav: NavEngine

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val syncWatchHandler = Handler(Looper.getMainLooper())
    private val syncWatchWorker = Executors.newSingleThreadExecutor()
    private val cloudLaunchWorker = Executors.newSingleThreadExecutor()
    private var launchCloudChecked = false
    private var cloudProgress: AlertDialog? = null
    private var cloudProgressBar: ProgressBar? = null
    private var cloudProgressText: TextView? = null
    private var syncWatchOn = false
    private var syncWatchBusy = false
    private var syncWatchMisses = 0
    private var syncWatchPromptUid: String? = null
    private val syncWatchTick = object : Runnable {
        override fun run() {
            if (!syncWatchOn) return
            checkHotspotSync()
            syncWatchHandler.postDelayed(this, 12_000)
        }
    }
    private val tdtWorker = Executors.newSingleThreadExecutor()
    private val routeWorker = Executors.newSingleThreadExecutor()
    private val lyricWorker = Executors.newSingleThreadExecutor()

    // Keep the BLE pipe busy with the newest frame: stale frames are dropped.
    private val framePump = object : Runnable {
        override fun run() {
            if (navigating && sendFrames && ble.verified && ble.bulkIdle()) {
                val bytes = lastFrameBytes
                if (bytes != null && lastFrameGen != lastSentGen) {
                    if (ble.sendBulk(BleClient.TYPE_FRAME, lastFrameId, bytes)) {
                        lastSentGen = lastFrameGen
                    }
                }
            }
            main.postDelayed(this, 100)
        }
    }
    private val logLines = ArrayDeque<String>()

    private var frame: GeoFrame? = null
    private var blockCx = 0.0
    private var blockCy = 0.0
    private var haveBlock = false

    private var fetching = false
    private var nextCaptureAt = 0L
    private var rasterId = 0
    private var routeId = 0
    private var lastRaster: ByteArray? = null
    private var lastRoute: ByteArray? = null

    private var lastLat = Double.NaN
    private var lastLng = Double.NaN
    private var lastBearing = 0f
    private var lastKmh = 0
    private var navigating = false
    private var remainM = -1
    private var routeTotalM = 0.0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        lyricOffsetMs = getSharedPreferences("navbridge", MODE_PRIVATE)
            .getInt("lyric_offset2", 0).coerceIn(-2000, 2000)

        mainScroll = findViewById(R.id.mainScroll)
        connectGate = findViewById(R.id.connectGate)
        floatingStatus = findViewById(R.id.floatingStatus)
        btnGateConnect = findViewById(R.id.btnGateConnect)
        tvGateStatus = findViewById(R.id.tvGateStatus)
        tvStatus = findViewById(R.id.tvStatus)
        tvLog = findViewById(R.id.tvLog)
        etKeyword = findViewById(R.id.etKeyword)
        llTips = findViewById(R.id.llTips)
        tvDest = findViewById(R.id.tvDest)
        ivPreview = findViewById(R.id.ivPreview)
        btnPreviewMode = findViewById(R.id.btnPreviewMode)
        btnStyle = findViewById(R.id.btnStyle)
        btnTravel = findViewById(R.id.btnTravel)
        btnOrientation = findViewById(R.id.btnOrientation)
        btnZoomOut = findViewById(R.id.btnZoomOut)
        btnZoomIn = findViewById(R.id.btnZoomIn)
        tvZoom = findViewById(R.id.tvZoom)

        log("等待操作")

        requestPermissions()
        capture = MapCapture(this)

        ble = BleClient(
            this,
            onStatus = { msg -> runOnUiThread {
                status(msg); log(msg)
                if (!sessionActive) tvGateStatus.text = msg
            } },
            onSessionChanged = { active ->
                runOnUiThread { setSessionActive(active) }
            },
            onReady = {
                runOnUiThread {
                    setSessionActive(true)
                    MediaKeepAliveService.start(this)
                    log("RaceBrick 通过验证，开始同步")
                    lastSentGen = -1L
                    lastMediaState = null
                    lastMediaMeta = null
                    mediaIdentity = ""
                    stableTitle = ""
                    lastRawTitle = ""
                    lastMediaPositionMs = -1L
                    lastAlbumKey = null
                    lastArtIdentity = ""
                    lastArtSentIdentity = ""
                    lastArtSrcSig = ""
                    lastArtSkipSig = ""
                    cancelArtWatchdog()
                    lyricKey = ""
                    lyricSource = "NONE"
                    lastLyricSourceSent = ""
                    lyricLines = emptyList()
                    systemLyric = ""
                    systemLyricSent = ""
                    lyricWindowKey = ""
                    lyricCleared = true
                    ble.send("M,LYRMODE,$lyricMode")
                    ble.send("M,LYROFF,$lyricOffsetMs")
                    sendMediaState()
                    sendMediaProgress()
                    sendMediaVolume()
                    sendMediaMetadata()
                    sendLyricWindow()
                    sendPosition()
                    sendLedProgress()
                    // If the player had not published its MediaSession yet at
                    // the instant BLE verified, retry once after it settles.
                    main.postDelayed({
                        if (ble.verified && stableTitle.isBlank()) {
                            mediaIdentity = ""
                            sendMediaMetadata()
                            sendLyricWindow()
                        }
                    }, 1200)
                    main.postDelayed({
                        if (ble.verified) forceCoverRefresh()
                    }, 900)
                    if (!sendFrames) {
                        lastRaster?.let { sendRasterBytes(it) }
                        lastRoute?.let { sendRouteBytes(it) }
                    }
                    if (!lastLat.isNaN()) maybeCapture()
                }
            },
            onLine = { line ->
                runOnUiThread {
                    when {
                        line.startsWith("M,") -> handleMediaCommand(line)
                        line.startsWith("#G,") -> handleDeviceGps(line)
                        line.startsWith("#Z,") -> handleDeviceZoom(line)
                        line.startsWith("#W,") -> wifiSetup?.onLine(line)
                        line.startsWith("#R,COVER,") -> handleCoverRefreshAck(line)
                        line == "#R,ART,OK" -> handleArtAck()
                        else -> log("← $line")
                    }
                }
            },
        )

        nav = NavEngine(this, object : NavEngine.Callback {
            override fun onStatus(msg: String) { status(msg); log(msg) }
            override fun onFix(lat: Double, lng: Double) { ensureFrame(lat, lng) }
            override fun onLocation(lat: Double, lng: Double, bearing: Float, kmh: Int) {
                handleLocation(lat, lng, bearing, kmh)
            }
            override fun onGuide(dir: String, dist: Int, nextRoad: String, icon: Bitmap?) {
                guideDir = dir
                guideDist = dist
                this@MainActivity.nextRoad = nextRoad
                val old = turnIcon
                turnIcon = icon
                if (old != null && old !== icon && !old.isRecycled) old.recycle()
                if (ble.verified && !sendFrames) ble.send("$dir,$dist")
                status("下一步 $dir，还有 $dist 米")
                updatePreview()
            }
            override fun onRoute(pathKey: String, coords: List<LatLng>) { sendRoute(coords) }
            override fun onProgress(remainM: Int, remainS: Int, lights: Int) {
                this@MainActivity.remainM = remainM
                this@MainActivity.remainS = remainS
                this@MainActivity.lights = lights
                val eta = java.util.Calendar.getInstance().apply { add(java.util.Calendar.SECOND, remainS) }
                val hhmm = eta.get(java.util.Calendar.HOUR_OF_DAY) * 100 + eta.get(java.util.Calendar.MINUTE)
                if (ble.verified && !sendFrames) ble.send("#e,$remainM,$hhmm,$lights")
                sendLedProgress(remainM)
                status("剩余 ${formatDistance(remainM)}，约 ${remainS / 60} 分钟")
                updatePreview()
            }
            override fun onLightDistance(dist: Int) {
                lightDist = dist
                if (ble.verified && !sendFrames) ble.send(if (dist >= 0) "#g,$dist" else "#g")
            }
            override fun onLanes(background: IntArray, recommended: IntArray) {
                laneBg = background
                laneFr = recommended
                updatePreview()
            }
            override fun onCameras(cameras: List<NavEngine.CamInfo>, speedLimit: Int) {
                this@MainActivity.cameras = cameras
                this@MainActivity.speedLimit = speedLimit
                updatePreview()
            }
            override fun onTraffic(segments: List<NavEngine.TrafficSeg>) {
                rawTraffic = segments
                updatePreview()
            }
            override fun onArrive() {
                navigating = false
                sendLedProgress(0)
                NavKeepAliveService.stop(this@MainActivity)
                if (ble.verified && !sendFrames) ble.send("A,0")
                status("已到达目的地")
                log("已到达")
            }
        })

        search = SearchEngine(
            this,
            onTips = { tips -> runOnUiThread { showTips(tips) } },
            onStatus = { msg -> runOnUiThread { status(msg); log(msg) } },
        )

        etKeyword.doAfterTextChanged { editable ->
            if (suppressSearch) return@doAfterTextChanged
            val q = editable?.toString()?.trim().orEmpty()
            searchRunnable?.let { searchHandler.removeCallbacks(it) }
            if (q != selName) {
                selLat = Double.NaN
                selLng = Double.NaN
                tvDest.text = "尚未选择目的地"
            }
            if (q.length < 2) {
                llTips.removeAllViews()
                return@doAfterTextChanged
            }
            val runnable = Runnable { search.suggest(q) }
            searchRunnable = runnable
            searchHandler.postDelayed(runnable, 350)
        }

        btnGateConnect.setOnClickListener {
            MediaKeepAliveService.start(this)
            tvGateStatus.text = "正在搜索 RaceBrick…"
            ble.connect()
        }
        floatingStatus.setOnClickListener { showDeviceMenu() }
        findViewById<Button>(R.id.btnStart).setOnClickListener { startNavigation(false) }
        findViewById<Button>(R.id.btnSimulate).setOnClickListener { startNavigation(true) }
        findViewById<Button>(R.id.btnStop).setOnClickListener { stopNavigation() }
        btnPreviewMode.setOnClickListener {
            showSdkPreview = !showSdkPreview
            haveBlock = false
            fetching = false
            nextCaptureAt = 0
            main.postDelayed({ maybeCapture() }, 300)
            updatePreview()
        }
        btnStyle.setOnClickListener { cycleStyle() }
        btnTravel.setOnClickListener { cycleTravelMode() }
        btnOrientation.setOnClickListener {
            headingUp = !headingUp
            updatePreview()
        }
        btnZoomOut.setOnClickListener {
            zoomIndex = (zoomIndex - 1 + zoomLevels.size) % zoomLevels.size
            applyZoom(zoomLevels[zoomIndex])
        }
        btnZoomIn.setOnClickListener {
            zoomIndex = (zoomIndex + 1) % zoomLevels.size
            applyZoom(zoomLevels[zoomIndex])
        }
        findViewById<Button>(R.id.btnWifi).setOnClickListener {
            if (!ble.connected) {
                toast("请先连接 RaceBrick 蓝牙")
                return@setOnClickListener
            }
            if (wifiSetup == null) wifiSetup = WifiSetupDialog(this, ble)
            wifiSetup?.show()
        }
        findViewById<Button>(R.id.btnSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.btnRace).setOnClickListener {
            startActivity(Intent(this, RaceActivity::class.java))
        }
        findViewById<Button>(R.id.btnOffline).setOnClickListener {
            startActivity(Intent(this, RaceActivity::class.java)
                .putExtra(RaceActivity.EXTRA_OFFLINE, true))
        }
        mapStyle = MapStyles.byId(
            getSharedPreferences("navbridge", MODE_PRIVATE).getString("map_style", MapStyles.DEFAULT.id)
        )
        travelMode = TravelMode.byId(
            getSharedPreferences("navbridge", MODE_PRIVATE).getString("travel_mode", TravelMode.DEFAULT.id)
        )
        tvZoom.text = String.format(Locale.US, "%.2f×", zoom)
        btnPreviewMode.text = if (showSdkPreview) "预览：SDK 原图" else "预览：Racebrick 风格"
        btnStyle.text = "风格：${mapStyle.label}"
        btnTravel.text = "工具：${travelMode.label}"
        maybePromptMediaAccess()

        main.post(framePump)
        main.post(mediaStatePump)
        handleDebugIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleDebugIntent(intent)
    }

    private fun handleDebugIntent(intent: Intent?) {
        if (!BuildConfig.DEBUG || intent == null) return
        intent.getStringExtra("media_cmd")?.let { cmd ->
            main.postDelayed({ handleMediaCommand("M,${cmd.uppercase()}") }, 1500)
        }
        intent.getStringExtra("gps_line")?.let { line ->
            main.postDelayed({ handleDeviceGps(line) }, 800)
        }
        intent.getStringExtra("dest")?.let { spec ->
            val p = spec.split(",")
            val lat = p.getOrNull(0)?.toDoubleOrNull()
            val lng = p.getOrNull(1)?.toDoubleOrNull()
            if (lat != null && lng != null) {
                main.postDelayed({ setDestination(lat, lng, "调试目的地") }, 1200)
            }
        }
        if (intent.getBooleanExtra("race", false)) {
            main.postDelayed({
                showSdkPreview = false
                updatePreview()
            }, 1600)
        }
        if (intent.getBooleanExtra("start", false)) {
            main.postDelayed({ startNavigation(true) }, 2200)
        }
    }

    private fun setSessionActive(active: Boolean) {
        sessionActive = active
        mainScroll.visibility = if (active) View.VISIBLE else View.GONE
        connectGate.visibility = if (active) View.GONE else View.VISIBLE
        floatingStatus.visibility = if (active) View.VISIBLE else View.GONE
        if (!active) {
            cancelArtWatchdog()
            tvGateStatus.text = "未连接 RaceBrick"
        }
    }

    private fun showDeviceMenu() {
        if (!sessionActive) return
        AlertDialog.Builder(this)
            .setTitle("RaceBrick 设备")
            .setItems(arrayOf("断开连接", "重新连接")) { _, which ->
                when (which) {
                    0 -> {
                        log("主动断开 RaceBrick")
                        ble.disconnect()
                        setSessionActive(false)
                        tvGateStatus.text = "设备已断开"
                    }
                    1 -> {
                        log("重新连接 RaceBrick…")
                        ble.disconnect()
                        setSessionActive(false)
                        tvGateStatus.text = "正在重新连接…"
                        main.postDelayed({
                            MediaKeepAliveService.start(this)
                            ble.connect()
                        }, 700)
                    }
                }
            }
            .setNegativeButton("关闭", null)
            .show()
    }

    private fun forceCoverRefresh() {
        if (!ble.verified) return
        coverRefreshAttempts = 0
        startCoverRefreshAttempt()
    }

    private fun startCoverRefreshAttempt() {
        if (!ble.verified) return
        coverRefreshAttempts++
        val token = ++coverRefreshToken
        coverRefreshPendingToken = token
        log("封面刷新请求 #$coverRefreshAttempts token=$token")
        ble.send("#CR,$token")
        // Force the normal metadata path to resend the current bitmap.
        lastArtSentIdentity = ""
        artHoldUntil = 0L
        sendMediaMetadata()
        main.removeCallbacks(coverRefreshTimeout)
        main.postDelayed(coverRefreshTimeout, 2000)
    }

    private fun cancelArtWatchdog() {
        artWatchdog?.let { main.removeCallbacks(it) }
        artWatchdog = null
        artAckSeen = true
        artAckToken++
        artRetryCount = 0
    }

    private fun armArtWatchdog() {
        artAckSeen = false
        artAckToken++
        val token = artAckToken
        artWatchdog?.let { main.removeCallbacks(it) }
        val r = Runnable {
            if (artAckSeen || token != artAckToken || !ble.verified) return@Runnable
            if (artRetryCount >= 3) {
                log("封面确认超时：停止重试")
                return@Runnable
            }
            artRetryCount++
            log("封面5秒未确认，重发 #$artRetryCount")
            lastArtSentIdentity = ""
            artHoldUntil = 0L
            sendMediaMetadata()
        }
        artWatchdog = r
        main.postDelayed(r, 5000)
    }

    private fun handleArtAck() {
        artAckSeen = true
        artRetryCount = 0
        artWatchdog?.let { main.removeCallbacks(it) }
        artWatchdog = null
        coverRefreshPendingToken = -1
        main.removeCallbacks(coverRefreshTimeout)
    }

    private fun handleCoverRefreshAck(line: String) {
        val p = line.split(',')
        if (p.size < 4 || p[0] != "#R" || p[1] != "COVER") return
        val token = p[2].toIntOrNull() ?: return
        if (token != coverRefreshPendingToken) return
        coverRefreshPendingToken = -1
        main.removeCallbacks(coverRefreshTimeout)
        log("封面刷新完成：设备已确认 token=$token")
    }

    private fun requestPermissions() {
        val need = mutableListOf<String>()
        fun ask(p: String) {
            if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED) need.add(p)
        }
        ask(Manifest.permission.ACCESS_FINE_LOCATION)
        ask(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 29) ask(Manifest.permission.ACCESS_BACKGROUND_LOCATION)
        if (Build.VERSION.SDK_INT >= 31) {
            ask(Manifest.permission.BLUETOOTH_SCAN)
            ask(Manifest.permission.BLUETOOTH_CONNECT)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            ask(Manifest.permission.POST_NOTIFICATIONS)
            ask(Manifest.permission.NEARBY_WIFI_DEVICES)
        }
        if (need.isNotEmpty()) ActivityCompat.requestPermissions(this, need.toTypedArray(), 1)
    }

    private fun startNavigation(simulate: Boolean = false) {
        if (selLat.isNaN() || selLng.isNaN()) {
            toast("先在搜索框里选择一个目的地")
            return
        }
        if (Keys.amap(this).isEmpty()) {
            toast("请先在设置里填写高德 Key")
            startActivity(Intent(this, SettingsActivity::class.java))
            return
        }
        val targetLat = selLat
        val targetLng = selLng
        val targetName = selName
        frame = null
        haveBlock = false
        lastRaster = null
        lastRoute = null
        lastRoutePts = null
        lastRouteCoords = null
        lastRoutePtsWgs = null
        lastRouteWgsFrame = null
        tiandituGen++
        tiandituAppliedGen = tiandituGen
        tiandituIdx = null
        tiandituEffect?.recycle()
        tiandituEffect = null
        tiandituFrame = null
        lastFrameBytes = null
        lastFrameId = 0
        lastFrameGen = 0L
        lastSentGen = -1L
        frameGen = 0L
        frameId = 0
        deviceKmh = -1
        deviceGpsAt = 0L
        blockCx = 0.0
        blockCy = 0.0
        navigating = true
        guideDir = "S"
        guideDist = -1
        nextRoad = ""
        remainS = -1
        routeTotalM = 0.0
        clearLedProgress()
        lights = 0
        lightDist = -1
        laneBg = IntArray(0)
        laneFr = IntArray(0)
        cameras = emptyList()
        rawTraffic = emptyList()
        speedLimit = 0
        turnIcon?.let { if (!it.isRecycled) it.recycle() }
        turnIcon = null
        runCatching { sdkBase?.recycle(); effectBase?.recycle(); currentPreview?.recycle() }
        sdkBase = null
        effectBase = null
        currentPreview = null
        ivPreview.setImageBitmap(null)
        if (!ble.connected) log("未连接 RaceBrick：先手机端模拟，连接后自动补发")
        log((if (simulate) "模拟导航（${travelMode.label}）→ " else "真实导航（${travelMode.label}）→ ") +
            "$targetName ($targetLat,$targetLng)")
        nav.start(targetLat, targetLng, simulate, travelMode = travelMode)
        NavKeepAliveService.start(this)
    }

    private fun stopNavigation() {
        navigating = false
        clearLedProgress()
        nav.stop()
        haveBlock = false
        NavKeepAliveService.stop(this)
        if (ble.verified && !sendFrames) ble.send("C")
        log("已停止导航")
    }

    private fun ensureFrame(lat: Double, lng: Double): GeoFrame {
        var f = frame
        if (f == null) {
            f = GeoFrame.at(lat, lng)
            frame = f
            log("坐标系 #${f.id}")
        }
        return f
    }

    private fun handleLocation(lat: Double, lng: Double, bearing: Float, kmh: Int) {
        val f = ensureFrame(lat, lng)
        lastLat = lat
        lastLng = lng
        lastKmh = kmh
        val hdg = if (kmh >= 4) { lastBearing = bearing; bearing } else lastBearing
        sendPosition(f, lat, lng, hdg, kmh)
        status("${effectiveSpeedKmh()} km/h  ·  剩余 ${formatDistance(remainM)}")
        val now = System.currentTimeMillis()
        if (now - lastPreviewAt >= 500) {
            lastPreviewAt = now
            updatePreview()
        }
        // 没连圆屏也要取图：手机端模拟预览需要，同时缓存底图等连上后补发
        if (navigating) {
            val x = f.x(lng)
            val y = f.y(lat)
            maybeCapture(f, x, y, hdg, kmh)
        }
    }

    private fun sendPosition() {
        if (lastLat.isNaN()) return
        val f = frame ?: return
        sendPosition(f, lastLat, lastLng, lastBearing, lastKmh)
    }

    private fun sendPosition(f: GeoFrame, lat: Double, lng: Double, hdg: Float, kmh: Int) {
        if (!ble.verified || sendFrames) return
        val x = f.x(lng)
        val y = f.y(lat)
        val spd = effectiveSpeedKmh()
        ble.send("#p,${f.id},${(x * 10).roundToInt()},${(y * 10).roundToInt()},${hdg.roundToInt()},$spd")
        ble.send("#v,$spd")
    }

    /** Device NEO speed wins while fresh (the AMap callback often reports 0). */
    private fun effectiveSpeedKmh(): Int {
        val now = System.currentTimeMillis()
        return if (deviceKmh >= 0 && now - deviceGpsAt < 3000) deviceKmh else lastKmh
    }

    private fun maybeCapture() {
        if (lastLat.isNaN()) return
        val f = frame ?: return
        maybeCapture(f, f.x(lastLng), f.y(lastLat), lastBearing, lastKmh)
    }

    private fun maybeCapture(f: GeoFrame, x: Double, y: Double, hdg: Float, kmh: Int) {
        val now = System.currentTimeMillis()
        if (fetching || now < nextCaptureAt) return
        // 7500m 块 + 450m 重取阈值：支持 0.75×~2× 视图，车头朝上旋转仍不放大
        if (haveBlock && hypot(x - blockCx, y - blockCy) < 450.0) return

        fetching = true
        val ahead = 0.0
        val a = Math.toRadians(hdg.toDouble())
        val cx = x + sin(a) * ahead
        val cy = y + cos(a) * ahead
        val half = PreviewRenderer.BLOCK / 2.0
        val block = PreviewRenderer.BLOCK.toInt()
        val x0 = (cx - half).roundToInt()
        val y0 = (cy + half).roundToInt()

        val nw = LatLng(f.lat(y0.toDouble()), f.lng(x0.toDouble()))
        val ne = LatLng(f.lat(y0.toDouble()), f.lng((x0 + block).toDouble()))
        val sw = LatLng(f.lat((y0 - block).toDouble()), f.lng(x0.toDouble()))

        if (!showSdkPreview) {
            // Racebrick style: same block in WGS-84, fetched from Tianditu vec_w.
            val gcjLat = f.lat(y)
            val gcjLng = f.lng(x)
            val wgs = CoordConv.gcjToWgs(gcjLat, gcjLng)
            val wf = GeoFrame.at(wgs[0], wgs[1])
            val wcx = wf.x(wgs[1])
            val wcy = wf.y(wgs[0])
            val wx0 = (wcx - PreviewRenderer.BLOCK / 2.0).roundToInt()
            val wy0 = (wcy + PreviewRenderer.BLOCK / 2.0).roundToInt()
            val tdtGen = ++tiandituGen
            tianditu.renderBase(wgs[0], wgs[1], PreviewRenderer.BLOCK, PreviewRenderer.TDT_BASE) { bmp ->
                if (bmp == null) {
                    haveBlock = false
                    nextCaptureAt = System.currentTimeMillis() + 3000
                    log("天地图底图失败，3 秒后重试")
                    return@renderBase
                }
                tdtWorker.execute {
                    try {
                        val idx = RasterCodec.toIndices(bmp, PreviewRenderer.TDT_BASE)
                        val effect = PreviewRenderer.effectBase(
                            idx, PreviewRenderer.TDT_BASE, PreviewRenderer.TDT_BASE, mapStyle,
                        )
                        main.post {
                            if (tdtGen < tiandituAppliedGen) {
                                effect.recycle()
                                return@post
                            }
                            tiandituAppliedGen = tdtGen
                            tiandituIdx = idx
                            tiandituEffect?.recycle()
                            tiandituEffect = effect
                            tiandituFrame = wf
                            tiandituBlockX0 = wx0
                            tiandituBlockY0 = wy0
                            lastRoutePtsWgs = null
                            lastRouteWgsFrame = null
                            updatePreview()
                            log("天地图底图已更新")
                        }
                    } catch (e: Throwable) {
                        log("天地图底图转换失败：${e.message}")
                    } finally {
                        bmp.recycle()
                    }
                }
            }
        }

        if (!showSdkPreview) {
            // Racebrick 风格只需要天地图底图；跳过 AMap 截图，省 CPU/GL。
            lastX0 = x0
            lastY0 = y0
            blockCx = cx
            blockCy = cy
            haveBlock = true
            fetching = false
            updatePreview()
            return
        }

        status("正在取底图…")
        capture.capture(nw, ne, sw) { shot ->
            if (shot == null) {
                fetching = false
                nextCaptureAt = System.currentTimeMillis() + 5000
                log("取底图失败，5 秒后重试")
                return@capture
            }
            worker.execute {
                try {
                    // 旧 MotoNav 固件用的方形底图只在兼容分包模式生成；
                    // RaceBrick 整帧模式不再做这份 1200×1200 RLE，省 CPU/内存。
                    var raster: ByteArray? = null
                    if (!sendFrames) {
                        val mm = (PreviewRenderer.BLOCK / MapCapture.SIZE * 1000).roundToInt()
                        val idxFull = RasterCodec.toIndices(shot, MapCapture.SIZE)
                        raster = RasterCodec.encodeRaster(
                            f.id, x0, y0, MapCapture.SIZE, MapCapture.SIZE, mm, idxFull,
                        )
                        // 固件单块上限 500 KB；太大就降到 600×600（范围不变，像素翻倍）
                        if (raster.size > 480 * 1024) {
                            val small = ByteArray(600 * 600)
                            for (y in 0 until 600) {
                                for (x in 0 until 600) {
                                    small[y * 600 + x] = idxFull[(y * 2) * MapCapture.SIZE + x * 2]
                                }
                            }
                            raster = RasterCodec.encodeRaster(f.id, x0, y0, 600, 600, mm * 2, small)
                            log("底图较大，已降到 600×600")
                        }
                    }
                    // 整帧预览用的 1000×1000 底图（旋转补角靠它四周的余量）
                    val idxBase = RasterCodec.toIndices(shot, PreviewRenderer.BASE)
                    val sdk = PreviewRenderer.sdkBase(shot)
                    val effect = PreviewRenderer.effectBase(idxBase, PreviewRenderer.BASE, PreviewRenderer.BASE, mapStyle)
                    main.post {
                        raster?.let {
                            lastRaster = it
                            sendRasterBytes(it)
                            log("底图 ${it.size / 1024} KB 已生成")
                        }
                        runCatching { sdkBase?.recycle(); effectBase?.recycle() }
                        sdkBase = sdk
                        effectBase = effect
                        lastX0 = x0
                        lastY0 = y0
                        blockCx = cx
                        blockCy = cy
                        haveBlock = true
                        fetching = false
                        updatePreview()
                    }
                } catch (e: Throwable) {
                    main.post {
                        fetching = false
                        nextCaptureAt = System.currentTimeMillis() + 5000
                        log("底图编码失败：${e.message}")
                    }
                }
            }
        }
    }

    private fun sendRasterBytes(bytes: ByteArray) {
        if (sendFrames) return
        rasterId = (rasterId + 1) and 0xFF
        ble.sendBulk(BleClient.TYPE_RASTER, rasterId, bytes)
    }

    private fun sendRoute(coords: List<LatLng>) {
        val f = frame ?: return
        if (coords.size < 2) return
        lastRouteCoords = coords
        lastRoutePtsWgs = null
        lastRouteWgsFrame = null
        val pts = DoubleArray(coords.size * 2)
        for (i in coords.indices) {
            pts[2 * i] = f.x(coords[i].longitude)
            pts[2 * i + 1] = f.y(coords[i].latitude)
        }
        lastRoutePts = pts
        routeTotalM = routeLengthMeters(coords)
        sendLedProgress(if (remainM >= 0) remainM else routeTotalM.roundToInt())
        routeWorker.execute {
            try {
                val bytes = RasterCodec.encodeRoute(f.id, pts)
                main.post {
                    lastRoute = bytes
                    sendRouteBytes(bytes)
                    updatePreview()
                    log("路线 ${coords.size} 点 / ${bytes.size / 1024} KB 已生成")
                }
            } catch (e: Throwable) {
                main.post { log("路线编码失败：${e.message}") }
            }
        }
    }

    private fun sendRouteBytes(bytes: ByteArray) {
        if (sendFrames) return
        routeId = (routeId + 1) and 0xFF
        ble.sendBulk(BleClient.TYPE_ROUTE, routeId, bytes)
    }

    // ---------------- 目的地联想 ----------------

    private fun showTips(tips: List<Tip>) {
        llTips.removeAllViews()
        val visible = tips.filter { !it.name.isNullOrBlank() }.take(8)
        for (tip in visible) {
            val name = tip.name ?: ""
            val sub = listOfNotNull(tip.district, tip.address)
                .filter { it.isNotBlank() }
                .joinToString(" ")
            val row = TextView(this).apply {
                text = if (sub.isBlank()) name else "$name\n$sub"
                textSize = 14f
                setTextColor(0xFFDDDDDD.toInt())
                setPadding(28, 22, 28, 22)
                setBackgroundColor(0xFF191919.toInt())
                setOnClickListener { chooseTip(tip) }
            }
            llTips.addView(
                row,
                LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { bottomMargin = 2 },
            )
        }
    }

    private fun chooseTip(tip: Tip) {
        val name = tip.name ?: ""
        val point = tip.point
        if (point != null) {
            setDestination(point.latitude, point.longitude, name)
            return
        }
        // 少数建议（公交线、路段）没有坐标，用地名 + 区县做地理编码
        status("正在解析“$name”的坐标…")
        search.geocode(name, tip.district ?: "") { lat, lng, resolvedName ->
            runOnUiThread { setDestination(lat, lng, resolvedName) }
        }
    }

    private fun setDestination(lat: Double, lng: Double, name: String) {
        selLat = lat
        selLng = lng
        selName = name

        suppressSearch = true
        if (etKeyword.text.toString() != name) {
            etKeyword.setText(name)
            etKeyword.setSelection(etKeyword.text.length)
        }
        suppressSearch = false

        searchRunnable?.let { searchHandler.removeCallbacks(it) }
        llTips.removeAllViews()
        tvDest.text = "目的地：$name\n$lat, $lng"
        hideKeyboard()
        status("已选择：$name")
        log("目的地：$name ($lat,$lng)")
    }

    private fun hideKeyboard() {
        runCatching {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.hideSoftInputFromWindow(etKeyword.windowToken, 0)
        }
    }

    /**
     * 视图大小接口：0.75×~2×。
     * 手机按钮和设备 BLE 指令 #Z 都走这里。
     */
    private fun applyZoom(z: Float) {
        zoom = z.coerceIn(0.75f, 2.0f)
        tvZoom.text = String.format(Locale.US, "%.2f×", zoom)
        updatePreview()
        log(String.format(Locale.US, "视图大小：%.2f×", zoom))
    }

    /** 设备 → 手机： #Z,75 或 #Z,0.75（百分比/倍率都支持） */
    private fun handleDeviceZoom(line: String) {
        val raw = line.substring(3).trim().toFloatOrNull() ?: return
        val z = if (raw > 10f) raw / 100f else raw
        applyZoom(z)
        log(String.format(Locale.US, "设备设置视图：%.2f×", z))
    }

    /** ESP32 → 手机： #G,lat,lng,bearing,speed,accuracy,alt,time */
    private fun handleDeviceGps(line: String) {
        if (!this::nav.isInitialized) return
        val p = line.split(',')
        if (p.size < 8) return
        val lat = p[1].toDoubleOrNull() ?: return
        val lng = p[2].toDoubleOrNull() ?: return
        val bearing = p[3].toFloatOrNull() ?: 0f
        val speed = p[4].toFloatOrNull() ?: 0f
        val accuracy = p[5].toFloatOrNull() ?: 5f
        val altitude = p[6].toDoubleOrNull() ?: 0.0
        val time = p[7].toLongOrNull() ?: 0L
        deviceKmh = (speed * 3.6f).roundToInt().coerceAtLeast(0)
        deviceGpsAt = System.currentTimeMillis()
        nav.feedExternalGps(lat, lng, bearing, speed, accuracy, altitude, time)
        if (!deviceGpsSeen) {
            deviceGpsSeen = true
            log("收到设备 GPS：$lat,$lng（可切换定位来源）")
        }
        updatePreview()
    }

    private fun updatePreview() {
        val useTdt = !showSdkPreview && tiandituEffect != null && tiandituFrame != null
        val base = when {
            showSdkPreview -> sdkBase
            useTdt -> tiandituEffect
            else -> effectBase
        }
        if (base == null) return
        val f = frame
        val wf = if (useTdt) tiandituFrame else null
        val routeX0 = if (useTdt) tiandituBlockX0 else lastX0
        val routeY0 = if (useTdt) tiandituBlockY0 else lastY0
        val riderX: Double
        val riderY: Double
        if (useTdt && wf != null && !lastLat.isNaN()) {
            val w = CoordConv.gcjToWgs(lastLat, lastLng)
            riderX = wf.x(w[1])
            riderY = wf.y(w[0])
        } else if (f != null && !lastLat.isNaN()) {
            riderX = f.x(lastLng)
            riderY = f.y(lastLat)
        } else {
            riderX = routeX0 + PreviewRenderer.BLOCK / 2.0
            riderY = routeY0 - PreviewRenderer.BLOCK / 2.0
        }
        val hud = PreviewRenderer.NavHud(
            dir = guideDir,
            dist = guideDist,
            nextRoad = nextRoad,
            remainM = remainM,
            remainS = remainS,
            lights = lights,
            lightDist = lightDist,
            speed = effectiveSpeedKmh(),
        )

        val traffic = if (useTdt && wf != null) {
            rawTraffic.map { s ->
                val pts = DoubleArray(s.pts.size * 2)
                for (i in s.pts.indices) {
                    val w = CoordConv.gcjToWgs(s.pts[i].latitude, s.pts[i].longitude)
                    pts[2 * i] = wf.x(w[1])
                    pts[2 * i + 1] = wf.y(w[0])
                }
                PreviewRenderer.TrafficSeg(s.status, pts)
            }
        } else if (f != null) {
            rawTraffic.map { s ->
                val pts = DoubleArray(s.pts.size * 2)
                for (i in s.pts.indices) {
                    pts[2 * i] = f.x(s.pts[i].longitude)
                    pts[2 * i + 1] = f.y(s.pts[i].latitude)
                }
                PreviewRenderer.TrafficSeg(s.status, pts)
            }
        } else {
            emptyList()
        }
        val cams = if (useTdt && wf != null) {
            cameras.mapNotNull {
                if (it.lat == 0.0 && it.lng == 0.0) null
                else {
                    val w = CoordConv.gcjToWgs(it.lat, it.lng)
                    PreviewRenderer.CamMarker(wf.x(w[1]), wf.y(w[0]), it.type, it.speed)
                }
            }
        } else if (f != null) {
            cameras.mapNotNull {
                if (it.lat == 0.0 && it.lng == 0.0) null
                else PreviewRenderer.CamMarker(f.x(it.lng), f.y(it.lat), it.type, it.speed)
            }
        } else {
            emptyList()
        }

        val bmp = PreviewRenderer.overlay(
            base,
            if (useTdt && wf != null) wgsRoutePts(wf) else lastRoutePts,
            routeX0,
            routeY0,
            riderX,
            riderY,
            lastBearing,
            headingUp,
            hud,
            zoom,
            traffic,
            cams,
            laneBg,
            laneFr,
            speedLimit,
            mapStyle = this@MainActivity.mapStyle,
            sdkOriginal = showSdkPreview,
        )
        ivPreview.setImageBitmap(bmp)
        val old = currentPreview
        currentPreview = bmp
        if (old !== bmp && old != null && !old.isRecycled) old.recycle()
        btnPreviewMode.text = if (showSdkPreview) "预览：SDK 原图" else "预览：Racebrick 风格"
        btnOrientation.text = if (headingUp) "视角：车头朝上" else "视角：朝北"

        // 手机整帧：320×240 → 16 色 → RLE。约 1 秒一帧，编码放在后台线程。
        val now = System.currentTimeMillis()
        if (!encodingFrame && now - lastEncodeAt >= 650) {
            lastEncodeAt = now
            val id = (++frameId) and 0xFF
            val gen = ++frameGen
            val copy = bmp.copy(Bitmap.Config.ARGB_8888, false)
            if (copy == null) {
                encodingFrame = false
            } else {
                encodingFrame = true
                frameWorker.execute {
                    try {
                        val style = if (showSdkPreview) MapStyles.SDK_FRAME else mapStyle
                        val idx = FrameCodec.quantize(copy, style)
                        val bytes = FrameCodec.encode(id, idx, style)
                        main.post {
                            lastFrameBytes = bytes
                            lastFrameId = id
                            lastFrameGen = gen
                            encodingFrame = false
                            if (System.currentTimeMillis() - lastFrameLogAt > 3000) {
                                lastFrameLogAt = System.currentTimeMillis()
                                log("整帧 ${bytes.size / 1024} KB（${FrameCodec.W}×${FrameCodec.H}）")
                            }
                        }
                    } catch (e: Throwable) {
                        main.post {
                            encodingFrame = false
                            log("整帧编码失败：${e.message}")
                        }
                    } finally {
                        copy.recycle()
                    }
                }
            }
        }
    }


    private fun wgsRoutePts(wf: GeoFrame): DoubleArray? {
        val coords = lastRouteCoords ?: return null
        val cached = lastRoutePtsWgs
        if (cached != null && lastRouteWgsFrame === wf) return cached
        val out = DoubleArray(coords.size * 2)
        for (i in coords.indices) {
            val w = CoordConv.gcjToWgs(coords[i].latitude, coords[i].longitude)
            out[2 * i] = wf.x(w[1])
            out[2 * i + 1] = wf.y(w[0])
        }
        lastRoutePtsWgs = out
        lastRouteWgsFrame = wf
        return out
    }



    private fun cycleTravelMode() {
        travelMode = TravelMode.next(travelMode)
        getSharedPreferences("navbridge", MODE_PRIVATE).edit()
            .putString("travel_mode", travelMode.id).apply()
        btnTravel.text = "工具：${travelMode.label}"
        log("出行工具：${travelMode.label}（下次导航生效）")
    }

    private fun cycleStyle() {
        mapStyle = MapStyles.next(mapStyle)
        getSharedPreferences("navbridge", MODE_PRIVATE).edit()
            .putString("map_style", mapStyle.id).apply()
        btnStyle.text = "风格：${mapStyle.label}"
        log("地图风格：${mapStyle.label}")
        applyStyle()
    }

    private fun applyStyle() {
        val idx = tiandituIdx
        if (idx != null) {
            val st = mapStyle
            tdtWorker.execute {
                try {
                    val effect = PreviewRenderer.effectBase(
                        idx, PreviewRenderer.TDT_BASE, PreviewRenderer.TDT_BASE, st,
                    )
                    main.post {
                        if (st !== mapStyle) {
                            effect.recycle()
                            return@post
                        }
                        tiandituEffect?.recycle()
                        tiandituEffect = effect
                        updatePreview()
                    }
                } catch (e: Throwable) {
                    log("切换风格失败：${e.message}")
                }
            }
        } else {
            haveBlock = false
            fetching = false
            nextCaptureAt = 0
            maybeCapture()
        }
        updatePreview()
    }


    private fun routeLengthMeters(coords: List<LatLng>): Double {
        if (coords.size < 2) return 0.0
        var total = 0.0
        val out = FloatArray(1)
        for (i in 1 until coords.size) {
            Location.distanceBetween(
                coords[i - 1].latitude, coords[i - 1].longitude,
                coords[i].latitude, coords[i].longitude, out,
            )
            total += out[0]
        }
        return total
    }

    private fun sendLedProgress(remain: Int = remainM) {
        if (!this::ble.isInitialized || !ble.verified) return
        val total = routeTotalM.roundToInt()
        if (total <= 0 || remain < 0) return
        ble.send("#P,${remain.coerceIn(0, total)},$total")
    }

    private fun clearLedProgress() {
        if (this::ble.isInitialized && ble.verified) ble.send("#P,-1,0")
    }

    private fun formatDistance(m: Int): String {
        if (m < 0) return "--"
        return if (m >= 1000) String.format(Locale.US, "%.1f km", m / 1000.0) else "$m m"
    }

    private fun status(msg: String) { tvStatus.text = Lang.tr(msg) }

    private fun log(msg: String) {
        val t = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        logLines.addLast("$t  $msg")
        while (logLines.size > 60) logLines.removeFirst()
        tvLog.text = logLines.joinToString("\n")
    }

    private fun toast(msg: String) = Toast.makeText(this, Lang.tr(msg), Toast.LENGTH_SHORT).show()

    override fun onResume() {
        super.onResume()
        if (MediaControl.hasAccess(this)) mediaPrompted = false
        lastMediaState = null
        sendMediaState()
        // Cloud backup is manual in the new local-first model.
        syncWatchOn = false
        syncWatchHandler.removeCallbacks(syncWatchTick)
    }

    override fun onPause() {
        syncWatchOn = false
        syncWatchHandler.removeCallbacks(syncWatchTick)
        super.onPause()
    }

    private fun maybeCheckCloudOnLaunch() {
        if (launchCloudChecked) return
        val token = CloudStore.token(this) ?: return
        val uid = LocalCache.uid(this) ?: return
        if (!LocalCache.hasCache(this)) return
        launchCloudChecked = true
        showCloudProgress("云端同步", "正在比较本地与云端记录版本…", true)
        updateCloudProgress(20, null)
        cloudLaunchWorker.execute {
            runCatching { CloudApi().state(uid, token) }
                .onSuccess { st ->
                    main.post {
                        if (isFinishing) return@post
                        if (st.dataVersion == LocalCache.dataVersion(this) && !LocalCache.dirty(this)) {
                            updateCloudProgress(100, "版本一致，无需同步")
                            status("云端已同步 · 版本 ${st.dataVersion}")
                            main.postDelayed({ hideCloudProgress() }, 450)
                        } else {
                            hideCloudProgress()
                            showCloudConflict(uid, token, st.dataVersion)
                        }
                    }
                }
                .onFailure { e ->
                    main.post {
                        hideCloudProgress()
                        status("云端检查失败：${e.message}")
                    }
                }
        }
    }

    private fun showCloudConflict(uid: String, token: String, cloudVersion: Int) {
        val localVersion = LocalCache.localVersion(this)
        val dirty = LocalCache.dirty(this)
        val text = "本地版本：$localVersion${if (dirty) "（有本地修改）" else ""}\n" +
            "云端版本：$cloudVersion\n\n" +
            "请选择要加载的数据。"
        val d = AlertDialog.Builder(this)
            .setTitle("本地与云端记录不一致")
            .setMessage(text)
            .setPositiveButton("加载本地", null)
            .setNeutralButton("加载云端", null)
            .setNegativeButton("稍后", null)
            .create()
        d.setOnShowListener {
            d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                d.dismiss()
                if (dirty) {
                    launchLocalSyncForDevice()
                } else {
                    uploadLocalCache(uid, token)
                }
            }
            d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                d.dismiss()
                downloadCloudCache(uid, token)
            }
        }
        d.show()
        Lang.localizeDialog(d)
    }

    private fun launchLocalSyncForDevice() {
        showCloudProgress("本地优先", "正在寻找设备，准备上传本地修改…", true)
        cloudLaunchWorker.execute {
            val found = HotspotDiscovery.find(RaceApi.baseUrl(this), 3500)
            main.post {
                hideCloudProgress()
                if (found == null) {
                    toast("本地有未上传修改，请先连接设备热点")
                } else {
                    startActivity(
                        Intent(this, RaceActivity::class.java)
                            .putExtra(RaceActivity.EXTRA_AUTO_SYNC_UID, found.uid)
                            .putExtra(RaceActivity.EXTRA_FORCE_LOCAL, true)
                    )
                }
            }
        }
    }

    private fun uploadLocalCache(uid: String, token: String) {
        showCloudProgress("上传本地记录", "正在用本地版本覆盖云端…", true)
        updateCloudProgress(15, null)
        cloudLaunchWorker.execute {
            runCatching {
                updateCloudProgressOnMain(45, "正在上传本地快照…")
                CloudSyncManager.uploadCachedLocal(this, uid, token)
            }.onSuccess { version ->
                main.post {
                    updateCloudProgress(100, "本地已上传")
                    status("本地版本已上传 · 版本 $version")
                    main.postDelayed({ hideCloudProgress() }, 450)
                }
            }.onFailure { e ->
                main.post {
                    hideCloudProgress()
                    status("本地上传失败：${e.message}")
                }
            }
        }
    }

    private fun downloadCloudCache(uid: String, token: String) {
        showCloudProgress("加载云端记录", "正在下载云端快照…", true)
        updateCloudProgress(15, null)
        cloudLaunchWorker.execute {
            runCatching {
                updateCloudProgressOnMain(55, "正在下载云端快照…")
               val version = CloudSyncManager.loadCloudToCache(this, uid, token)
                updateCloudProgressOnMain(80, "正在检查设备…")
                val found = HotspotDiscovery.find(RaceApi.baseUrl(this), 2500)
                if (found != null) {
                    updateCloudProgressOnMain(90, "正在写回设备 Track…")
                    CloudSyncManager.applyCloudTracks(this, found.baseUrl, uid, token)
                }
                version
            }.onSuccess { version ->
                main.post {
                    updateCloudProgress(100, "云端记录已加载")
                    status("云端记录已加载 · 版本 $version")
                    main.postDelayed({ hideCloudProgress() }, 450)
                }
            }.onFailure { e ->
                main.post {
                    hideCloudProgress()
                    status("加载云端失败：${e.message}")
                }
            }
        }
    }

    private fun updateCloudProgressOnMain(progress: Int, message: String?) {
        main.post { updateCloudProgress(progress, message) }
    }

    private fun showCloudProgress(title: String, message: String, determinate: Boolean) {
        hideCloudProgress()
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(16), dp(20), dp(20))
        }
        cloudProgressText = TextView(this).apply {
            text = Lang.tr(message)
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 13f
        }
        box.addView(cloudProgressText)
        cloudProgressBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            isIndeterminate = !determinate
            max = 100
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(12) }
        }
        box.addView(cloudProgressBar)
        cloudProgress = AlertDialog.Builder(this)
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

    private fun updateCloudProgress(progress: Int, message: String?) {
        cloudProgressBar?.let {
            it.isIndeterminate = false
            it.progress = progress.coerceIn(0, 100)
        }
        if (message != null) cloudProgressText?.text = Lang.tr(message)
    }

    private fun hideCloudProgress() {
        cloudProgress?.dismiss()
        cloudProgress = null
        cloudProgressBar = null
        cloudProgressText = null
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun checkHotspotSync() {
        if (!syncWatchOn || syncWatchBusy || isFinishing) return
        syncWatchBusy = true
        val saved = RaceApi.baseUrl(this)
        syncWatchWorker.execute {
            val found = HotspotDiscovery.find(saved, 3000)
            main.post {
                syncWatchBusy = false
                if (isFinishing || !syncWatchOn) return@post
                if (found == null) {
                    syncWatchMisses++
                    if (syncWatchMisses >= 3) syncWatchPromptUid = null
                    return@post
                }
                syncWatchMisses = 0
                if (syncWatchPromptUid == found.uid) return@post
                syncWatchPromptUid = found.uid
                RaceApi.saveBaseUrl(this, found.baseUrl)
                val logged = CloudStore.token(this) != null
                val d = AlertDialog.Builder(this)
                    .setTitle("RaceBrick 已连接手机热点")
                    .setMessage("设备：${found.uid}\n\n" +
                        (if (logged) "是否进入 RACE 同步 Track / Race Log？"
                         else "登录云端账号后，可进入 RACE 同步 Track / Race Log。"))
                    .setPositiveButton(if (logged) "进入同步" else "进入登录") { _, _ ->
                        startActivity(
                            Intent(this, RaceActivity::class.java)
                                .putExtra(RaceActivity.EXTRA_AUTO_SYNC_UID, found.uid)
                        )
                    }
                    .setNegativeButton("稍后", null)
                    .create()
                d.show()
                Lang.localizeDialog(d)
            }
        }
    }

    private fun sendMediaProgress() {
        if (!::ble.isInitialized || !ble.verified) return
        val p = MediaControl.progress(this) ?: return
        ble.send("M,POS,${p.positionMs},${p.durationMs}")
    }

    private fun sendMediaState() {
        if (!::ble.isInitialized || !ble.verified) return
        // Send a heartbeat even when unchanged. If the phone pauses while an
        // older device still shows PLAYING, the next second corrects it.
        val playing = MediaControl.isPlaying(this) ?: false
        lastMediaState = playing
        ble.send("M,STATE,${if (playing) 1 else 0}")
    }

    private fun normalizeMediaMeta(m: MediaControl.Meta): MediaControl.Meta {
        var title = m.title.trim()
        var artist = m.artist.trim()
        // Some players expose artist as "Title - Artist" (and sometimes repeat
        // the title), which breaks both the display and lyrics lookup.
        if (title.isNotEmpty() && artist.startsWith(title)) {
            artist = artist.substring(title.length).trim()
                .trimStart('-', '–', '—', ':', '：').trim()
            if (artist.contains(" - ")) artist = artist.substringAfterLast(" - ").trim()
        }
        if (artist.isNotEmpty() && title.endsWith(" - $artist")) {
            title = title.removeSuffix(" - $artist").trim()
        }
        return m.copy(title = title, artist = artist)
    }

    private fun sendMediaMetadata() {
        if (!::ble.isInitialized || !ble.verified) return
        val raw = MediaControl.metadata(this)
        val m = raw?.let { normalizeMediaMeta(it) }
        if (m == null) {
            if (lastMediaMeta != "") {
                lastMediaMeta = ""
                mediaIdentity = ""
                stableTitle = ""
                systemLyric = ""
                systemLyricSent = ""
                ble.send("M,TITLE,")
                ble.send("M,ARTIST,")
                ble.send("M,ALBUM,")
                loadLyrics(null)
            }
            return
        }
        val progress = MediaControl.progress(this)
        val duration = if (m.durationMs > 0) m.durationMs
                       else progress?.durationMs ?: 0L
        val pos = progress?.positionMs ?: 0L
        // Identity normally changes on a track change. If a player omits
        // artist/album/duration, a position reset is the reliable fallback.
        val positionReset = lastMediaPositionMs > 8000L && pos < 4000L
        // Do not use the title as a lyric update trigger. Some music apps put
        // the current lyric line into the title field; identity uses the
        // stable artist/album/duration tuple instead.
        // Duration disambiguates different tracks by the same artist/album.
        // The raw title can be replaced by a lyric line by some players, so it
        // is not used as the primary track identity here.
        val identity = "${m.artist}\u0001${m.album}\u0001${m.durationMs}"
        val trackChanged = identity != mediaIdentity ||
            (positionReset && m.title.isNotBlank() && m.title != stableTitle)
        if (trackChanged) {
            // A manually selected lyric source is only for the current song.
            // Every new track returns to the automatic matching rules.
            if (lyricMode != 0) {
                lyricMode = 0
                preferNotificationLyric = false
                ble.send("M,LYRMODE,0")
                log("歌词模式恢复：AUTO")
            }
            mediaIdentity = identity
            stableTitle = m.title
            lastRawTitle = m.title
            lastMediaMeta = identity
            systemLyric = ""
            systemLyricSent = ""
            forceFirstLyric = false
            lastNotifLyric = MediaNotificationArt.lyric
            preferNotificationLyric = false
            cancelArtWatchdog()
            artHoldUntil = SystemClock.elapsedRealtime() + 450L
            log("曲目变化：$stableTitle / ${m.artist} / ${m.album}")
            val title = stableTitle.ifBlank { m.artist }
            ble.send("M,TITLE,${Uri.encode(clipUtf8(title, 96))}")
            ble.send("M,ARTIST,${Uri.encode(clipUtf8(m.artist, 64))}")
            ble.send("M,ALBUM,${Uri.encode(clipUtf8(m.album, 64))}")
            loadLyrics(m.copy(title = stableTitle))
        } else if (m.title != lastRawTitle) {
            // Some players write the current lyric line into the title/display
            // title. Keep the first title as the song name and use later title
            // changes as a lyric fallback when no LRC is available.
            lastRawTitle = m.title
            if ((lyricMode == 0 || lyricMode == 4) &&
                lyricLines.isEmpty() && m.title.isNotBlank() && m.title != stableTitle) {
                systemLyric = m.title
                lyricSource = "TITL"
                sendLyricSource(true)
                log("歌词回退：标题通道")
            }
        }
        val notifLine = MediaNotificationArt.lyric.trim()
        if (notifLine.isNotEmpty() && notifLine != lastNotifLyric) {
            lastNotifLyric = notifLine
            log("通知歌词候选：$notifLine")
            val cjkTrack = containsCjk(m.title + m.artist + m.album)
            val autoNe = lyricMode == 0 && isAllChineseTitle(m.title)
            val validLine = notifLine.length > 1 &&
                !notifLine.equals(m.title, true) &&
                !notifLine.equals(m.artist, true) &&
                !notifLine.equals(m.album, true)
            // For fully-Chinese titles AUTO selects NETEASE directly. Other
            // Chinese-content tracks may use the status-bar notification line.
            if (validLine && ((lyricMode == 0 && cjkTrack && !autoNe) ||
                              ((lyricMode == 3 || (lyricMode == 0 && !autoNe)) && lyricLines.isEmpty()))) {
                systemLyric = notifLine
                lyricSource = "NOTI"
                if (lyricMode == 0 && cjkTrack) {
                    preferNotificationLyric = true
                    log("歌词优先级：中文通知歌词")
                }
                sendLyricSource(true)
            }
        }
        sendLyricSource()
        lastMediaPositionMs = pos
        val art = MediaControl.albumArt(this)
        val artSig = if (art == null) "null"
                     else "${art.width}x${art.height}#${bitmapSampleHash(art)}"
        if (artSig != lastArtSrcSig) {
            lastArtSrcSig = artSig
            log("封面源：$artSig")
        }
        val trackArtIdentity = "$mediaIdentity|$stableTitle"
        if (trackChanged) {
            // Old notification/metadata can survive a skip for a moment.
            // Clear the old cover immediately and give the player a short
            // settle window before latching the new bitmap.
            lastArtIdentity = trackArtIdentity
            ble.send("M,ART,0")
            log("封面清除：切歌")
        }
        val nowMs = SystemClock.elapsedRealtime()
        if (art != null) {
            // Some players expose the cover through two sources (metadata and
            // the MediaStyle notification) that can alternate with slightly
            // different bytes. Send at most one cover per track so the device
            // does not blink between variants. A reconnect resets this below.
            if (nowMs >= artHoldUntil && trackArtIdentity != lastArtSentIdentity) {
                if (sendAlbumArt(art)) {
                    lastAlbumKey = "${art.width}x${art.height}:$trackArtIdentity"
                    lastArtIdentity = trackArtIdentity
                    lastArtSentIdentity = trackArtIdentity
                    artTxSeq++
                    log("封面发送 #$artTxSeq：$artSig")
                } else {
                    log("封面发送失败：等待 MTU/连接，稍后重试")
                }
            } else if (nowMs >= artHoldUntil && artSig != lastArtSkipSig) {
                lastArtSkipSig = artSig
                log("封面保持：同曲已有封面，跳过 $artSig")
            }
        } else if (trackArtIdentity != lastArtIdentity) {
            // Only clear when the track changed. A transient null from the
            // MediaSession must not erase a cover that was already shown.
            lastArtIdentity = trackArtIdentity
            lastAlbumKey = "none"
            log("封面清除：当前曲目没有封面")
            ble.send("M,ART,0")
        }
    }

    private fun loadLyrics(m: MediaControl.Meta?) {
        val key = if (m == null) "" else "${m.title}|${m.artist}|${m.album}"
        if (key == lyricKey) return
        lyricKey = key
        lyricLines = emptyList()
        lyricSource = "WAIT"
        lyricWindowKey = ""
        if (!lyricCleared) { ble.send("M,LYRIC,CLR"); lyricCleared = true }
        if (m == null || m.title.isBlank()) {
            lyricSource = "NONE"
            sendLyricSource()
            return
        }
        if (!onlineLyricsEnabled) {
            lyricSource = "OFF"
            log("歌词：系统标题通道（不联网）")
            return
        }
        val cached = lyricCache[key]
        if (cached != null) {
            lyricLines = cached.lines
            lyricSource = cached.source
            if (cached.lines.isNotEmpty()) forceFirstLyric = true
            return
        }
        val effectiveMode = effectiveLyricMode(m.title)
        if (effectiveMode == 3) {
            lyricSource = "NOTI"
            lastNotifLyric = ""
            sendLyricSource(true)
            return
        }
        if (effectiveMode == 4) {
            lyricSource = "TITL"
            lastRawTitle = stableTitle
            sendLyricSource(true)
            return
        }
        val title = m.title
        val artist = m.artist
        val album = m.album
        val duration = if (m.durationMs > 0) m.durationMs
                       else MediaControl.progress(this)?.durationMs ?: 0L
        log("歌词请求：$title / $artist")
        lyricWorker.execute {
            val result = LyricsRepository.fetch(title, artist, album, duration, effectiveMode)
            runOnUiThread {
                if (lyricKey == key) {
                    lyricCache[key] = result
                    lyricLines = result.lines
                    lyricWindowKey = ""
                    if (result.lines.isNotEmpty()) {
                        forceFirstLyric = true
                        if (preferNotificationLyric && systemLyric.isNotEmpty()) {
                            // Keep the NotificationListenerService line as the
                            // priority source for Chinese tracks; network LRC
                            // remains available as fallback data.
                            lyricSource = "NOTI"
                        } else {
                            systemLyric = ""
                            systemLyricSent = ""
                            lyricSource = result.source
                        }
                    } else if (systemLyric.isEmpty()) {
                        lyricSource = "NONE"
                    }
                    sendLyricSource(true)
                    if (result.lines.isNotEmpty()) {
                        log("歌词首句：${result.lines.first().text}")
                    }
                    log(if (result.lines.isEmpty()) "歌词：LRCLIB/网易云均未找到 / ${result.source}" else "歌词：${result.lines.size} 行 / ${result.source}")
                }
            }
        }
    }

    private fun setLyricOffset(ms: Int) {
        lyricOffsetMs = ms.coerceIn(-2000, 2000)
        getSharedPreferences("navbridge", MODE_PRIVATE).edit()
            .putInt("lyric_offset2", lyricOffsetMs).apply()
        lyricWindowKey = ""
        ble.send("M,LYROFF,$lyricOffsetMs")
        log("歌词偏移：${lyricOffsetMs}ms")
        sendLyricWindow()
    }

    private fun setLyricMode(index: Int) {
        lyricMode = index.coerceIn(0, lyricModeNames.size - 1)
        lyricCache.clear()
        lyricKey = ""
        lyricLines = emptyList()
        lyricWindowKey = ""
        systemLyric = ""
        systemLyricSent = ""
        lastNotifLyric = ""
        lastRawTitle = stableTitle
        preferNotificationLyric = false
        lyricSource = "WAIT"
        if (!lyricCleared) { ble.send("M,LYRIC,CLR"); lyricCleared = true }
        ble.send("M,LYRMODE,$lyricMode")
        sendLyricSource(true)
        log("歌词模式：${lyricModeNames[lyricMode]}")
        val raw = MediaControl.metadata(this)
        val m = raw?.let { normalizeMediaMeta(it) }
        if (m == null) {
            lyricSource = "NONE"
            sendLyricSource(true)
            return
        }
        loadLyrics(m.copy(title = stableTitle.ifBlank { m.title }))
        sendLyricWindow()
    }

    private fun cycleLyricMode() {
        setLyricMode((lyricMode + 1) % lyricModeNames.size)
    }

    private fun clipUtf8(s: String, maxBytes: Int): String {
        if (s.toByteArray(Charsets.UTF_8).size <= maxBytes) return s
        val out = StringBuilder()
        var bytes = 0
        var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i)
            val n = when {
                cp < 0x80 -> 1
                cp < 0x800 -> 2
                cp < 0x10000 -> 3
                else -> 4
            }
            if (bytes + n > maxBytes) break
            out.appendCodePoint(cp)
            bytes += n
            i += Character.charCount(cp)
        }
        return out.toString()
    }

    private fun containsCjk(s: String): Boolean =
        s.any { it.code in 0x3400..0x9FFF }

    private fun isAllChineseTitle(title: String): Boolean {
        val t = title.trim()
        if (t.isEmpty()) return false
        val punct = "，。！？；：、（）【】《》“”‘’…—～·"
        return t.all { ch ->
            ch.code in 0x3400..0x9FFF || ch.isWhitespace() || punct.indexOf(ch) >= 0
        }
    }

    private fun effectiveLyricMode(title: String): Int =
        if (lyricMode == 0 && isAllChineseTitle(title)) 2 else lyricMode

    private fun sendLyricSource(force: Boolean = false) {
        if (!::ble.isInitialized || !ble.verified) return
        val code = lyricSource.ifBlank { "NONE" }
        if (force || code != lastLyricSourceSent) {
            lastLyricSourceSent = code
            ble.send("M,LYRICSRC,$code")
        }
    }

    private fun sendLyricWindow() {
        if (!::ble.isInitialized || !ble.verified) return
        sendLyricSource(true)
        if (preferNotificationLyric && systemLyric.isNotEmpty()) {
            if (systemLyric != systemLyricSent) {
                ble.send("M,LYRIC,CLR")
                ble.send("M,LYRIC,0,${Uri.encode(clipUtf8(systemLyric, 120))}")
                ble.send("M,LYRICPOS,0")
                systemLyricSent = systemLyric
                lyricCleared = false
            }
            return
        }
        if (lyricLines.isEmpty()) {
            if (systemLyric.isNotEmpty()) {
                if (systemLyric != systemLyricSent) {
                    ble.send("M,LYRIC,CLR")
                    ble.send("M,LYRIC,0,${Uri.encode(clipUtf8(systemLyric, 120))}")
                    ble.send("M,LYRICPOS,0")
                    systemLyricSent = systemLyric
                    lyricCleared = false
                }
                return
            }
            systemLyricSent = ""
            if (!lyricCleared) {
                ble.send("M,LYRIC,CLR")
                lyricCleared = true
            }
            return
        }
        val pos = MediaControl.progress(this)?.positionMs ?: 0L
        val effectivePos = if (lyricSource == "NE") (pos - lyricOffsetMs).coerceAtLeast(0) else pos
        var active = -1
        for (i in lyricLines.indices) {
            if (lyricLines[i].timeMs <= effectivePos) active = i else break
        }
        // Show the first available lyric during the intro instead of leaving
        // the screen blank until its timestamp. For NetEase the first real
        // lyric is the 5th raw LRC line because the first four are metadata.
        if (active < 0) active = 0
        // Guarantee the first available line is shown once when a fresh lyric
        // list arrives, even if network loading finished several seconds in.
        if (forceFirstLyric) {
            active = 0
            forceFirstLyric = false
            log("歌词定位：显示第一句")
        }
        var start = (active - 2).coerceAtLeast(0)
        if (start + 5 > lyricLines.size) start = (lyricLines.size - 5).coerceAtLeast(0)
        val window = ArrayList<String>(5)
        for (i in 0 until 5) {
            val idx = start + i
            window.add(if (idx in lyricLines.indices) lyricLines[idx].text else "")
        }
        val key = "$start|$active|${window.joinToString("\u0001")}"
        if (key == lyricWindowKey) return
        lyricWindowKey = key
        ble.send("M,LYRIC,CLR")
        for (i in 0 until 5) {
            ble.send("M,LYRIC,$i,${Uri.encode(clipUtf8(window[i], 120))}")
        }
        ble.send("M,LYRICPOS,${active - start}")
        lyricCleared = false
    }

    private fun bitmapSampleHash(bitmap: Bitmap): String {
        var h = 17L
        val stepX = (bitmap.width / 8).coerceAtLeast(1)
        val stepY = (bitmap.height / 8).coerceAtLeast(1)
        var y = 0
        while (y < bitmap.height) {
            var x = 0
            while (x < bitmap.width) {
                h = (h * 31 + bitmap.getPixel(x, y)) and 0xffffffffL
                x += stepX
            }
            y += stepY
        }
        return java.lang.Long.toHexString(h)
    }

    private fun sendAlbumArt(bitmap: Bitmap): Boolean {
        if (!::ble.isInitialized || !ble.verified) return false
        val w = 96
        val h = 96
        val scaled = Bitmap.createScaledBitmap(bitmap, w, h, true)
        val px = IntArray(w * h)
        scaled.getPixels(px, 0, w, 0, 0, w, h)
        val out = ByteArray(w * h * 2)
        for (i in px.indices) {
            val c = px[i]
            val r = (c shr 16) and 0xFF
            val g = (c shr 8) and 0xFF
            val b = c and 0xFF
            val v = ((r and 0xF8) shl 8) or ((g and 0xFC) shl 3) or (b shr 3)
            out[i * 2] = (v and 0xFF).toByte()
            out[i * 2 + 1] = ((v shr 8) and 0xFF).toByte()
        }
        if (scaled !== bitmap) scaled.recycle()
        var txHash = 2166136261L
        for (v in out) {
            txHash = (txHash xor (v.toLong() and 0xff)) * 16777619L
            txHash = txHash and 0xffffffffL
        }
        log("封面TX：96x96 ${out.size}B hash=${java.lang.Long.toHexString(txHash)}")
        val ok = ble.sendBulk(BleClient.TYPE_ALBUM, 1, out)
        if (ok) armArtWatchdog()
        return ok
    }

    private fun handleMediaCommand(line: String) {
        val cmd = line.removePrefix("M,").trim()
        if (cmd == "LYRICSRC") {
            cycleLyricMode()
            return
        }
        if (cmd.startsWith("LYRSET,")) {
            cmd.substringAfter(',').toIntOrNull()?.let { setLyricMode(it) }
            return
        }
        if (cmd.startsWith("LYROFFSET,")) {
            cmd.substringAfter(',').toIntOrNull()?.let { setLyricOffset(it) }
            return
        }
        val ok = when (cmd) {
            "TOGGLE" -> MediaControl.toggle(this)
            "NEXT" -> MediaControl.next(this)
            "PREV" -> MediaControl.prev(this)
            "VOLUP" -> MediaControl.volumeUp(this)
            "VOLDOWN" -> MediaControl.volumeDown(this)
            else -> return
        }
        log(if (ok) "媒体控制：$cmd" else "媒体控制失败：$cmd")
        status(if (ok) "音乐：$cmd" else "音乐控制失败，请开启通知使用权")
        if (!MediaControl.hasAccess(this) && !mediaPrompted) {
            mediaPrompted = true
            log("请开启通知使用权，ESP 才能控制其他音乐 App")
            MediaControl.openSettings(this)
        }
        main.postDelayed({
            sendMediaState()
            sendMediaProgress()
            sendMediaVolume()
            sendMediaMetadata()
            sendLyricWindow()
        }, 450)
    }

    private fun sendMediaVolume() {
        if (!ble.verified) return
        val v = MediaControl.volume(this) ?: return
        ble.send("M,VOL,${v.first},${v.second}")
    }

    private fun maybePromptMediaAccess() {
        if (MediaControl.hasAccess(this)) return
        val prefs = getSharedPreferences("navbridge", MODE_PRIVATE)
        if (prefs.getBoolean("media_prompted", false)) return
        prefs.edit().putBoolean("media_prompted", true).apply()
        main.postDelayed({
            toast("请开启 NavBridge 的通知使用权，ESP 才能控制手机音乐")
            MediaControl.openSettings(this)
        }, 1200)
    }

    override fun onDestroy() {
        NavKeepAliveService.stop(this)
        searchHandler.removeCallbacksAndMessages(null)
        main.removeCallbacks(framePump)
        main.removeCallbacks(mediaStatePump)
        runCatching { tianditu.close() }
        runCatching { tiandituEffect?.recycle() }
        tiandituEffect = null
        runCatching { turnIcon?.recycle() }
        turnIcon = null
        runCatching { sdkBase?.recycle(); effectBase?.recycle(); currentPreview?.recycle() }
        sdkBase = null
        effectBase = null
        currentPreview = null
        runCatching { capture.close() }
        wifiSetup?.dismiss()
        wifiSetup = null
        MediaKeepAliveService.stop(this)
        runCatching { ble.disconnect() }
        runCatching { nav.stop() }
        worker.shutdownNow()
        tdtWorker.shutdownNow()
        routeWorker.shutdownNow()
        lyricWorker.shutdownNow()
        frameWorker.shutdownNow()
        syncWatchHandler.removeCallbacks(syncWatchTick)
        syncWatchWorker.shutdownNow()
        cloudLaunchWorker.shutdownNow()
        hideCloudProgress()
        super.onDestroy()
    }
}
