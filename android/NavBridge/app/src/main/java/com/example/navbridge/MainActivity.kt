package com.example.navbridge

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
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
    private lateinit var tvSign: TextView
    private lateinit var tvLog: TextView
    private lateinit var etKey: EditText
    private lateinit var etKeyword: EditText
    private lateinit var llTips: LinearLayout
    private lateinit var tvDest: TextView
    private lateinit var ivPreview: ImageView
    private lateinit var btnPreviewMode: Button
    private lateinit var btnOrientation: Button
    private lateinit var btnGpsSource: Button
    private lateinit var btnSendFrames: Button
    private lateinit var btnZoom: Button
    private lateinit var btnMediaAccess: Button

    private var zoom = 1f
    private var zoomIndex = 1
    private val zoomLevels = floatArrayOf(0.75f, 1.0f, 1.25f, 1.5f, 2.0f)

    private val frameWorker = Executors.newSingleThreadExecutor()
    private var encodingFrame = false
    private var sendFrames = true
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

    private var useDeviceGps = false
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

    private var sdkBase: Bitmap? = null
    private var effectBase: Bitmap? = null
    private var currentPreview: Bitmap? = null
    private var lastRoutePts: DoubleArray? = null
    private var lastX0 = 0
    private var lastY0 = 0
    private var showSdkPreview = true
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        tvStatus = findViewById(R.id.tvStatus)
        tvSign = findViewById(R.id.tvSign)
        tvLog = findViewById(R.id.tvLog)
        etKey = findViewById(R.id.etKey)
        etKeyword = findViewById(R.id.etKeyword)
        llTips = findViewById(R.id.llTips)
        tvDest = findViewById(R.id.tvDest)
        ivPreview = findViewById(R.id.ivPreview)
        btnPreviewMode = findViewById(R.id.btnPreviewMode)
        btnOrientation = findViewById(R.id.btnOrientation)
        btnGpsSource = findViewById(R.id.btnGpsSource)
        btnSendFrames = findViewById(R.id.btnSendFrames)
        btnZoom = findViewById(R.id.btnZoom)
        btnMediaAccess = findViewById(R.id.btnMediaAccess)

        tvSign.text = "包名：$packageName\nSHA1：${Keys.signSha1(this)}"
        etKey.setText(Keys.amap(this))
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

        findViewById<Button>(R.id.btnSaveKey).setOnClickListener {
            val key = etKey.text.toString().trim()
            if (key.isEmpty()) {
                toast("Key 不能为空")
                return@setOnClickListener
            }
            if (!Keys.save(this, key)) {
                toast("Key 保存失败，请重试")
                return@setOnClickListener
            }
            Keys.apply(this)
            log("Key 已保存，重启 App 生效")
            restartApp()
        }
        findViewById<Button>(R.id.btnConnect).setOnClickListener { ble.connect() }
        findViewById<Button>(R.id.btnStart).setOnClickListener { startNavigation(false) }
        findViewById<Button>(R.id.btnSimulate).setOnClickListener { startNavigation(true) }
        findViewById<Button>(R.id.btnStop).setOnClickListener { stopNavigation() }
        btnPreviewMode.setOnClickListener {
            showSdkPreview = !showSdkPreview
            updatePreview()
        }
        btnOrientation.setOnClickListener {
            headingUp = !headingUp
            updatePreview()
        }
        btnGpsSource.setOnClickListener {
            useDeviceGps = !useDeviceGps
            nav.setExternalGps(useDeviceGps)
            btnGpsSource.text = if (useDeviceGps) "定位：设备 GPS" else "定位：手机 GPS"
            log(if (useDeviceGps) "定位来源：ESP32 设备 GPS" else "定位来源：手机 GPS")
        }
        btnSendFrames.setOnClickListener {
            sendFrames = !sendFrames
            btnSendFrames.text = if (sendFrames) "整帧发送：开" else "整帧发送：关"
            log(if (sendFrames) "整帧发送开启（type 5，320×240，RaceBrick）" else "整帧发送关闭")
            lastSentGen = -1L
            if (sendFrames) main.post(framePump)
        }
        btnZoom.setOnClickListener {
            zoomIndex = (zoomIndex + 1) % zoomLevels.size
            applyZoom(zoomLevels[zoomIndex])
        }
        btnMediaAccess.setOnClickListener {
            MediaControl.openSettings(this)
            toast("开启通知使用权后，ESP 才能控制其他音乐 App")
        }
        updateMediaAccessText()

        tvSign.text = "版本：${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})\n" + tvSign.text
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
    }

    private fun requestPermissions() {
        val need = mutableListOf<String>()
        fun ask(p: String) {
            if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED) need.add(p)
        }
        ask(Manifest.permission.ACCESS_FINE_LOCATION)
        ask(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 31) {
            ask(Manifest.permission.BLUETOOTH_SCAN)
            ask(Manifest.permission.BLUETOOTH_CONNECT)
        }
        if (Build.VERSION.SDK_INT >= 33) ask(Manifest.permission.POST_NOTIFICATIONS)
        if (need.isNotEmpty()) ActivityCompat.requestPermissions(this, need.toTypedArray(), 1)
    }

    private fun startNavigation(simulate: Boolean = false) {
        var targetLat = selLat
        var targetLng = selLng
        var targetName = selName
        var simStartLat: Double? = null
        var simStartLng: Double? = null
        if (simulate) {
            if (targetLat.isNaN() || targetLng.isNaN()) {
                // 没有选目的地也能演示：固定一段上海市区的短路线
                simStartLat = 31.19875; simStartLng = 121.43375
                targetLat = 31.21075; targetLng = 121.45275
                targetName = "演示路线"
            } else {
                // 从目的地西南约 1 km 处出发，保证室内没有 GPS 也能模拟
                simStartLat = targetLat - 0.006
                simStartLng = targetLng - 0.008
            }
        } else if (targetLat.isNaN() || targetLng.isNaN()) {
            toast("先在搜索框里选择一个目的地")
            return
        }
        if (Keys.amap(this).isEmpty()) {
            toast("先填高德 Key")
            return
        }
        frame = null
        haveBlock = false
        lastRaster = null
        lastRoute = null
        lastRoutePts = null
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
        log((if (simulate) "开始模拟导航 → " else "开始导航 → ") + "$targetName ($targetLat,$targetLng)")
        nav.start(targetLat, targetLng, simulate, simStartLat, simStartLng)
    }

    private fun stopNavigation() {
        navigating = false
        nav.stop()
        haveBlock = false
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
        // 5700m 块 + 450m 重取阈值：支持 0.75×~2× 视图，车头朝上旋转仍不放大
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
                    // 旧 MotoNav 固件用的方形底图：4650m 块，mm/px 约 3875
                    val mm = (PreviewRenderer.BLOCK / MapCapture.SIZE * 1000).roundToInt()
                    val idxFull = RasterCodec.toIndices(shot, MapCapture.SIZE)
                    var bytes = RasterCodec.encodeRaster(
                        f.id, x0, y0, MapCapture.SIZE, MapCapture.SIZE, mm, idxFull,
                    )
                    // 固件单块上限 500 KB；太大就降到 600×600（范围不变，像素翻倍）
                    if (bytes.size > 480 * 1024) {
                        val small = ByteArray(600 * 600)
                        for (y in 0 until 600) {
                            for (x in 0 until 600) {
                                small[y * 600 + x] = idxFull[(y * 2) * MapCapture.SIZE + x * 2]
                            }
                        }
                        bytes = RasterCodec.encodeRaster(f.id, x0, y0, 600, 600, mm * 2, small)
                        log("底图较大，已降到 600×600")
                    }
                    // 整帧预览用的 620×620 底图（旋转补角靠它四周的余量）
                    val idxBase = RasterCodec.toIndices(shot, PreviewRenderer.BASE)
                    val sdk = PreviewRenderer.sdkBase(shot)
                    val effect = PreviewRenderer.effectBase(idxBase, PreviewRenderer.BASE)
                    main.post {
                        lastRaster = bytes
                        sendRasterBytes(bytes)
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
                        log("底图 ${bytes.size / 1024} KB 已生成")
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
        val pts = DoubleArray(coords.size * 2)
        for (i in coords.indices) {
            pts[2 * i] = f.x(coords[i].longitude)
            pts[2 * i + 1] = f.y(coords[i].latitude)
        }
        lastRoutePts = pts
        worker.execute {
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
        btnZoom.text = String.format(Locale.US, "视图：%.2f×", zoom)
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
        val base = if (showSdkPreview) sdkBase else effectBase
        if (base == null) return
        val f = frame
        val riderX: Double
        val riderY: Double
        if (f != null && !lastLat.isNaN()) {
            riderX = f.x(lastLng)
            riderY = f.y(lastLat)
        } else {
            riderX = lastX0 + 1200.0
            riderY = lastY0 - 1200.0
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

        val traffic = if (f != null) rawTraffic.map { s ->
            val pts = DoubleArray(s.pts.size * 2)
            for (i in s.pts.indices) {
                pts[2 * i] = f.x(s.pts[i].longitude)
                pts[2 * i + 1] = f.y(s.pts[i].latitude)
            }
            PreviewRenderer.TrafficSeg(s.status, pts)
        } else {
            emptyList()
        }
        val cams = if (f != null) cameras.mapNotNull {
            if (it.lat == 0.0 && it.lng == 0.0) null
            else PreviewRenderer.CamMarker(f.x(it.lng), f.y(it.lat), it.type, it.speed)
        } else {
            emptyList()
        }

        val bmp = PreviewRenderer.overlay(
            base,
            lastRoutePts,
            lastX0,
            lastY0,
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
            turnIcon,
            speedLimit,
            showSdkPreview,
        )
        ivPreview.setImageBitmap(bmp)
        val old = currentPreview
        currentPreview = bmp
        if (old !== bmp && old != null && !old.isRecycled) old.recycle()
        btnPreviewMode.text = if (showSdkPreview) "预览：SDK 原图" else "预览：圆屏效果"
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
                        val idx = FrameCodec.quantize(copy)
                        val bytes = FrameCodec.encode(id, idx)
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

    private fun restartApp() {
        val i = packageManager.getLaunchIntentForPackage(packageName) ?: return
        i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
        startActivity(i)
        Runtime.getRuntime().exit(0)
    }

    override fun onResume() {
        super.onResume()
        if (MediaControl.hasAccess(this)) mediaPrompted = false
        updateMediaAccessText()
        lastMediaState = null
        sendMediaState()
    }

    private fun updateMediaAccessText() {
        if (!::btnMediaAccess.isInitialized) return
        btnMediaAccess.text = if (MediaControl.hasAccess(this)) {
            "媒体控制权限：已授权"
        } else {
            "媒体控制权限：未授权（点此开启）"
        }
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
            updateMediaAccessText()
            MediaControl.openSettings(this)
        }
        main.postDelayed({ sendMediaState() }, 450)
    }

    override fun onDestroy() {
        searchHandler.removeCallbacksAndMessages(null)
        main.removeCallbacks(framePump)
        main.removeCallbacks(mediaStatePump)
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
        frameWorker.shutdownNow()
        super.onDestroy()
    }
}
