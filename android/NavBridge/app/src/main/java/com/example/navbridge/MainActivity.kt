package com.example.navbridge

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.location.Location
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
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

class MainActivity : AppCompatActivity() {

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
    private var mediaPrompted = false
    private var deviceKmh = -1
    private var deviceGpsAt = 0L

    // Keep the device's media button state in sync with the phone's active session.
    private val mediaStatePump = object : Runnable {
        override fun run() {
            sendMediaState()
            main.postDelayed(this, 2000)
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
    private lateinit var capture: MapCapture
    private lateinit var nav: NavEngine

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val tdtWorker = Executors.newSingleThreadExecutor()
    private val routeWorker = Executors.newSingleThreadExecutor()

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
            onStatus = { msg -> runOnUiThread { status(msg); log(msg) } },
            onReady = {
                runOnUiThread {
                    log("RaceBrick 通过验证，开始同步")
                    lastSentGen = -1L
                    lastMediaState = null
                    sendMediaState()
                    sendPosition()
                    sendLedProgress()
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

        findViewById<Button>(R.id.btnConnect).setOnClickListener { ble.connect() }
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
        findViewById<Button>(R.id.btnSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
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
        if (Build.VERSION.SDK_INT >= 33) ask(Manifest.permission.POST_NOTIFICATIONS)
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

    private fun status(msg: String) { tvStatus.text = msg }

    private fun log(msg: String) {
        val t = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        logLines.addLast("$t  $msg")
        while (logLines.size > 60) logLines.removeFirst()
        tvLog.text = logLines.joinToString("\n")
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    override fun onResume() {
        super.onResume()
        if (MediaControl.hasAccess(this)) mediaPrompted = false
        lastMediaState = null
        sendMediaState()
    }

    private fun sendMediaState() {
        if (!::ble.isInitialized || !ble.verified) return
        val playing = MediaControl.isPlaying(this) ?: return
        if (playing == lastMediaState) return
        lastMediaState = playing
        ble.send("M,STATE,${if (playing) 1 else 0}")
    }

    private fun handleMediaCommand(line: String) {
        val cmd = line.removePrefix("M,").trim()
        val ok = when (cmd) {
            "TOGGLE" -> MediaControl.toggle(this)
            "NEXT" -> MediaControl.next(this)
            "PREV" -> MediaControl.prev(this)
            else -> return
        }
        log(if (ok) "媒体控制：$cmd" else "媒体控制失败：$cmd")
        status(if (ok) "音乐：$cmd" else "音乐控制失败，请开启通知使用权")
        if (!MediaControl.hasAccess(this) && !mediaPrompted) {
            mediaPrompted = true
            log("请开启通知使用权，ESP 才能控制其他音乐 App")
            MediaControl.openSettings(this)
        }
        main.postDelayed({ sendMediaState() }, 450)
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
        runCatching { ble.disconnect() }
        runCatching { nav.stop() }
        worker.shutdownNow()
        tdtWorker.shutdownNow()
        routeWorker.shutdownNow()
        frameWorker.shutdownNow()
        super.onDestroy()
    }
}
