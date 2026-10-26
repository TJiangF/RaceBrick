package com.example.navbridge

import android.content.Context
import android.graphics.Bitmap
import android.location.Location
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import com.amap.api.location.AMapLocationClient
import com.amap.api.location.AMapLocationClientOption
import com.amap.api.location.AMapLocationListener
import com.amap.api.maps.model.LatLng
import com.amap.api.navi.AMapNavi
import com.amap.api.navi.AMapNaviListener
import com.amap.api.navi.enums.IconType
import com.amap.api.navi.enums.NaviType
import com.amap.api.navi.enums.PathPlanningStrategy
import com.amap.api.navi.model.AMapNaviCameraInfo
import com.amap.api.navi.model.AMapNaviLocation
import com.amap.api.navi.model.NaviInfo
import com.amap.api.navi.model.NaviLatLng
import java.lang.reflect.Proxy
import java.util.Locale
import kotlin.math.roundToInt

/**
 * 高德导航封装：算路、真实 GPS 导航、旁听转向/位置、TTS 播报。
 *
 * 用反射 Proxy 挂 AMapNaviListener，避免不同 SDK 版本接口数量变化导致编译不过。
 */
class NavEngine(private val app: Context, private val cb: Callback) {

    class CamInfo(val lat: Double, val lng: Double, val type: Int, val speed: Int, val dist: Int)
    class TrafficSeg(val status: Int, val pts: List<LatLng>)

    interface Callback {
        fun onStatus(msg: String)
        fun onFix(lat: Double, lng: Double)
        fun onLocation(lat: Double, lng: Double, bearing: Float, kmh: Int)
        fun onGuide(dir: String, dist: Int, nextRoad: String, icon: Bitmap?)
        fun onRoute(pathKey: String, coords: List<LatLng>)
        fun onProgress(remainM: Int, remainS: Int, lights: Int)
        fun onLightDistance(dist: Int)
        fun onLanes(background: IntArray, recommended: IntArray)
        fun onCameras(cameras: List<CamInfo>, speedLimit: Int)
        fun onTraffic(segments: List<TrafficSeg>)
        fun onArrive()
    }

    private val main = Handler(Looper.getMainLooper())
    private var navi: AMapNavi? = null
    private var locClient: AMapLocationClient? = null
    private var lastRouteKey = ""
    private var started = false
    private var simulate = false

    private var useExternalGps = false
    private var externalFix: Location? = null
    private var hasPendingDest = false
    private var pendingDestLat = 0.0
    private var pendingDestLng = 0.0

    private var tts: TextToSpeech? = null
    private var ttsReady = false

    private val listener: AMapNaviListener = Proxy.newProxyInstance(
        AMapNaviListener::class.java.classLoader,
        arrayOf<Class<*>>(AMapNaviListener::class.java),
    ) { proxy, method, args ->
        when (method.name) {
            "hashCode" -> System.identityHashCode(proxy)
            "equals" -> proxy === args?.getOrNull(0)
            "toString" -> "NavBridgeListener"
            "onLocationChange" -> {
                val loc = args?.getOrNull(0) as? AMapNaviLocation
                val c = loc?.coord
                if (loc != null && c != null) {
                    val kmh = loc.speed.roundToInt().coerceAtLeast(0)
                    main.post { cb.onLocation(c.latitude, c.longitude, loc.bearing, kmh) }
                }
                null
            }
            "onNaviInfoUpdate" -> {
                val info = args?.getOrNull(0) as? NaviInfo
                if (info != null) {
                    val icon = runCatching {
                        info.iconBitmap?.copy(Bitmap.Config.ARGB_8888, false)
                    }.getOrNull()
                    main.post {
                        cb.onGuide(
                            iconToDir(info.iconType),
                            info.curStepRetainDistance.coerceAtLeast(0),
                            info.nextRoadName ?: "",
                            icon,
                        )
                        cb.onProgress(info.pathRetainDistance, info.pathRetainTime, info.routeRemainLightCount)
                        cb.onLightDistance(
                            nextLightDist(info.curStep, info.curLink, info.curStepRetainDistance) ?: -1,
                        )
                        checkRoute()
                        pushTrafficIfDue()
                    }
                }
                null
            }
            "showLaneInfo" -> {
                onLaneInfo(args)
                null
            }
            "hideLaneInfo" -> {
                main.post { cb.onLanes(IntArray(0), IntArray(0)) }
                null
            }
            "updateCameraInfo" -> {
                onCameras(args?.getOrNull(0) as? Array<*>)
                null
            }
            "onTrafficStatusUpdate" -> {
                main.post { pushTraffic() }
                null
            }
            "onCalculateRouteSuccess" -> {
                main.post { startNavi() }
                null
            }
            "onCalculateRouteFailure" -> {
                val code = args?.getOrNull(0)
                main.post { cb.onStatus("算路失败：$code") }
                null
            }
            "onReCalculateRouteForYaw" -> {
                main.post { cb.onStatus("偏航，正在重新规划…") }
                null
            }
            "onArriveDestination", "onEndEmulatorNavi" -> {
                main.post { cb.onArrive() }
                null
            }
            "onGetNavigationText" -> {
                val text = args?.firstOrNull { it is String } as? String
                if (!text.isNullOrBlank()) speak(text)
                null
            }
            else -> defaultValue(method.returnType)
        }
    } as AMapNaviListener

    fun start(
        destLat: Double,
        destLng: Double,
        simulate: Boolean = false,
        startLat: Double? = null,
        startLng: Double? = null,
    ) {
        val n = runCatching { AMapNavi.getInstance(app) }.getOrNull()
        if (n == null) { cb.onStatus("高德导航初始化失败，检查 Key"); return }
        navi = n
        this.simulate = simulate
        lastRouteKey = ""
        started = false
        hasPendingDest = false

        val useExtra = useExternalGps && !simulate
        runCatching { n.setIsUseExtraGPSData(useExtra) }
        runCatching { n.removeAMapNaviListener(listener) }
        runCatching { n.addAMapNaviListener(listener) }
        initTts()

        if (startLat != null && startLng != null) {
            cb.onStatus(if (simulate) "准备模拟路线…" else "正在规划路线…")
            calculateRoute(startLat, startLng, destLat, destLng)
        } else if (useExtra) {
            val fix = externalFix
            if (fix != null) {
                val gcj = CoordConv.wgsToGcj(fix.latitude, fix.longitude)
                main.post { calculateRoute(gcj[0], gcj[1], destLat, destLng) }
            } else {
                pendingDestLat = destLat
                pendingDestLng = destLng
                hasPendingDest = true
                cb.onStatus("等待设备 GPS 定位…")
            }
        } else {
            cb.onStatus("正在定位…")
            onceLocation { lat, lng ->
                main.post {
                    if (lat == null || lng == null) {
                        cb.onStatus("定位失败：检查定位权限、GPS、网络")
                        return@post
                    }
                    calculateRoute(lat, lng, destLat, destLng)
                }
            }
        }
    }

    /** 切换定位来源。设备 GPS 模式下，导航完全由 ESP32 的 NEO 数据驱动。 */
    fun setExternalGps(enabled: Boolean) {
        useExternalGps = enabled
        runCatching { navi?.setIsUseExtraGPSData(enabled && !simulate) }
        if (!enabled) externalFix = null
        cb.onStatus(if (enabled) "定位来源：设备 GPS" else "定位来源：手机 GPS")
    }

    /**
     * ESP32 通过 BLE 发来的 NEO 定位（WGS84）。
     *
     * @param speed m/s；@param accuracy 米；@param timeMs epoch 毫秒（0 = 用手机时间）
     */
    fun feedExternalGps(
        lat: Double,
        lng: Double,
        bearing: Float,
        speed: Float,
        accuracy: Float,
        altitude: Double,
        timeMs: Long,
    ) {
        if (!useExternalGps) return
        val loc = Location("esp32-neo").apply {
            latitude = lat
            longitude = lng
            this.bearing = bearing
            this.speed = speed
            this.accuracy = if (accuracy > 0f) accuracy else 5f
            this.altitude = altitude
            time = if (timeMs > 0) timeMs else System.currentTimeMillis()
        }
        externalFix = loc
        // type=1：WGS84（系统默认坐标）；NEO 原始输出就是 WGS84
        runCatching { navi?.setExtraGPSData(1, loc) }

        if (hasPendingDest) {
            hasPendingDest = false
            val gcj = CoordConv.wgsToGcj(lat, lng)
            main.post { calculateRoute(gcj[0], gcj[1], pendingDestLat, pendingDestLng) }
        }
    }

    private fun calculateRoute(fromLat: Double, fromLng: Double, destLat: Double, destLng: Double) {
        val n = navi ?: return
        cb.onFix(fromLat, fromLng)
        cb.onStatus("正在规划路线…")
        val result = runCatching {
            n.calculateDriveRoute(
                arrayListOf(NaviLatLng(fromLat, fromLng)),
                arrayListOf(NaviLatLng(destLat, destLng)),
                arrayListOf<NaviLatLng>(),
                PathPlanningStrategy.DRIVING_DEFAULT,
            )
        }
        result.onFailure { cb.onStatus("发起算路失败：${it.message}") }
    }

    private fun startNavi() {
        // onCalculateRouteSuccess 有两个重载，可能回调两次
        if (started) return
        val n = navi ?: return
        started = true
        checkRoute()
        val mode = if (simulate) NaviType.EMULATOR else NaviType.GPS
        runCatching { n.setEmulatorNaviSpeed(90) }
        runCatching { n.startNavi(mode) }
            .onFailure { cb.onStatus("启动导航失败：${it.message}") }
        cb.onStatus(if (simulate) "模拟导航中" else "导航中")
    }

    /**
     * 前方最近一个红绿灯的距离（米）。
     * 高德没有“红绿灯秒数”，只有每个 link 的 getTrafficLights() 布尔标记和长度。
     */
    private fun nextLightDist(step: Int, curLink: Int, stepRemain: Int): Int? {
        if (curLink < 0) return null
        val steps = runCatching { navi?.naviPath?.steps }.getOrNull() ?: return null
        if (step < 0 || step >= steps.size) return null
        var links = runCatching { steps[step].links }.getOrNull() ?: return null
        if (curLink >= links.size) return null

        var after = 0
        for (k in curLink + 1 until links.size) after += links[k].length
        var acc = (stepRemain - after).coerceAtLeast(0)
        if (links[curLink].trafficLights) return acc

        var s = step
        var k = curLink + 1
        while (acc <= 1000) {
            if (k >= links.size) {
                s++
                if (s >= steps.size) return null
                links = runCatching { steps[s].links }.getOrNull() ?: return null
                k = 0
                continue
            }
            acc += links[k].length
            if (links[k].trafficLights) return acc
            k++
        }
        return null
    }

    private fun checkRoute() {
        val n = navi ?: return
        val path = runCatching { n.naviPath }.getOrNull() ?: return
        val key = "${path.pathid}:${path.allLength}"
        if (key == lastRouteKey) return
        lastRouteKey = key
        val raw = runCatching { path.coordList }.getOrNull() ?: return
        val coords = raw.map { LatLng(it.latitude, it.longitude) }
        if (coords.size >= 2) {
            cb.onRoute(key, coords)
            pushTraffic()
        }
    }

    // ---------------- 车道 / 电子眼 / 路况 ----------------

    private fun onLaneInfo(args: Array<out Any?>?) {
        if (args.isNullOrEmpty()) return
        var bg: List<Int> = emptyList()
        var fr: List<Int> = emptyList()
        var count = 0
        if (args.size >= 3) {
            // 旧版：showLaneInfo(AMapLaneInfo[], byte[] background, byte[] front)
            count = (args[0] as? Array<*>)?.size ?: 0
            bg = toIntList(args[1])
            fr = toIntList(args[2])
        } else {
            // 新版：showLaneInfo(AMapLaneInfo)，字段是 private，需要反射
            val obj = args[0] ?: return
            bg = toIntList(fieldOf(obj, "backgroundLane"))
            fr = toIntList(fieldOf(obj, "frontLane"))
            count = (fieldOf(obj, "laneCount") as? Number)?.toInt() ?: maxOf(bg.size, fr.size)
        }
        val n = minOf(count, 8, maxOf(bg.size, fr.size))
        if (n <= 0) {
            main.post { cb.onLanes(IntArray(0), IntArray(0)) }
            return
        }
        val b = IntArray(n) { bg.getOrElse(it) { 15 } and 0xFF }
        val f = IntArray(n) { fr.getOrElse(it) { 15 } and 0xFF }
        main.post { cb.onLanes(b, f) }
    }

    private fun toIntList(v: Any?): List<Int> = when (v) {
        is ByteArray -> v.map { it.toInt() and 0xFF }
        is IntArray -> v.toList()
        is Array<*> -> v.mapNotNull { (it as? Number)?.toInt() }
        is List<*> -> v.mapNotNull { (it as? Number)?.toInt() }
        else -> emptyList()
    }

    private fun fieldOf(o: Any, name: String): Any? {
        var c: Class<*>? = o.javaClass
        while (c != null) {
            try {
                val f = c.getDeclaredField(name)
                f.isAccessible = true
                return f.get(o)
            } catch (_: NoSuchFieldException) {
                c = c.superclass
            } catch (_: Exception) {
                return null
            }
        }
        return null
    }

    private fun onCameras(arr: Array<*>?) {
        if (arr == null) return
        val list = ArrayList<CamInfo>()
        var limit = 0
        var best = Int.MAX_VALUE
        for (o in arr) {
            val c = o as? AMapNaviCameraInfo ?: continue
            val lat = c.y
            val lng = c.x
            val speed = c.cameraSpeed
            val dist = c.cameraDistance
            if (lat != 0.0 || lng != 0.0) list.add(CamInfo(lat, lng, c.cameraType, speed, dist))
            if (speed > 0 && dist in 0..1000 && dist < best) {
                best = dist
                limit = speed
            }
        }
        main.post { cb.onCameras(list, limit) }
    }

    private var lastTrafficAt = 0L

    private fun pushTrafficIfDue() {
        val now = System.currentTimeMillis()
        if (now - lastTrafficAt < 60_000) return
        pushTraffic()
    }

    private fun pushTraffic() {
        lastTrafficAt = System.currentTimeMillis()
        val path = runCatching { navi?.naviPath }.getOrNull() ?: return
        val steps = runCatching { path.steps }.getOrNull() ?: return
        val segs = ArrayList<TrafficSeg>()
        for (step in steps) {
            val links = runCatching { step.links }.getOrNull() ?: continue
            for (link in links) {
                val coords = runCatching { link.coords }.getOrNull() ?: continue
                if (coords.size < 2) continue
                segs.add(TrafficSeg(link.trafficStatus, coords.map { LatLng(it.latitude, it.longitude) }))
            }
        }
        if (segs.isNotEmpty()) cb.onTraffic(segs)
    }

    fun stop() {
        stopLoc()
        runCatching { navi?.stopNavi() }
        runCatching { navi?.removeAMapNaviListener(listener) }
        navi = null
        lastRouteKey = ""
        started = false
        simulate = false
        hasPendingDest = false
        cb.onStatus("已停止")
    }

    private fun onceLocation(done: (Double?, Double?) -> Unit) {
        stopLoc()
        val c = runCatching { AMapLocationClient(app) }.getOrNull()
        if (c == null) { done(null, null); return }
        locClient = c
        val opt = AMapLocationClientOption().apply {
            locationMode = AMapLocationClientOption.AMapLocationMode.Hight_Accuracy
            isOnceLocation = true
            isOnceLocationLatest = true
            httpTimeOut = 8000
        }
        c.setLocationOption(opt)
        val l = AMapLocationListener { loc ->
            stopLoc()
            if (loc != null && loc.errorCode == 0) done(loc.latitude, loc.longitude)
            else done(null, null)
        }
        c.setLocationListener(l)
        runCatching { c.startLocation() }.onFailure { done(null, null) }
    }

    private fun stopLoc() {
        runCatching { locClient?.stopLocation() }
        runCatching { locClient?.onDestroy() }
        locClient = null
    }

    private fun initTts() {
        if (tts != null) return
        tts = TextToSpeech(app) { status ->
            ttsReady = status == TextToSpeech.SUCCESS
            if (ttsReady) runCatching { tts?.language = Locale.CHINA }
        }
    }

    private fun speak(text: String) {
        val t = tts ?: return
        if (!ttsReady) return
        runCatching { t.speak(text, TextToSpeech.QUEUE_FLUSH, null, "nav") }
    }

    fun iconToDir(icon: Int): String = when (icon) {
        IconType.LEFT, IconType.LEFT_BACK, IconType.ENTRY_RING_LEFT -> "L"
        IconType.RIGHT, IconType.RIGHT_BACK, IconType.ENTRY_RING_RIGHT -> "R"
        IconType.LEFT_FRONT, IconType.MERGE_LEFT -> "SL"
        IconType.RIGHT_FRONT, IconType.MERGE_RIGHT -> "SR"
        IconType.LEFT_TURN_AROUND, IconType.U_TURN_RIGHT, IconType.ENTRY_RING_UTURN -> "U"
        IconType.ARRIVED_DESTINATION -> "A"
        else -> "S"
    }

    private fun defaultValue(t: Class<*>): Any? = when (t) {
        java.lang.Boolean.TYPE -> false
        java.lang.Byte.TYPE -> 0.toByte()
        java.lang.Character.TYPE -> ' '
        java.lang.Short.TYPE -> 0.toShort()
        java.lang.Integer.TYPE -> 0
        java.lang.Long.TYPE -> 0L
        java.lang.Float.TYPE -> 0f
        java.lang.Double.TYPE -> 0.0
        else -> null
    }
}
