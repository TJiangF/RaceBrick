package com.example.navbridge

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.LruCache
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.log2
import kotlin.math.max
import kotlin.math.min

/**
 * 天地图 vec_w 矢量底图（WGS-84）。优先直连 tianditu.gov.cn（需要 tk 的 IP
 * 白名单）；直连失败时回退到 RaceBrick 服务端的 /api/tile?layer=road 代理。
 *
 * 输出是给“Racebrick 风格”用的 outSize×outSize 全彩底图，随后再由 RasterCodec
 * 做 4 色分类/RLE 映射。
 */
class TiandituTiles {
    companion object {
        private const val TAG = "TiandituTiles"
        private const val TILE = 256
        private const val PROXY = "https://racebrick.tfworks.cn/api/tile"
        private const val MAX_TILES_PER_SIDE = 8
    }

    private val cache = object : LruCache<String, Bitmap>(48 * 1024) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.byteCount / 1024
    }
    private val worker = Executors.newSingleThreadExecutor()
    private val io = Executors.newFixedThreadPool(6)
    private val handler = Handler(Looper.getMainLooper())
    @Volatile private var alive = true
    @Volatile private var directBlocked = false

    fun renderBase(
        centerLat: Double,
        centerLng: Double,
        blockMeters: Double,
        outSize: Int,
        done: (Bitmap?) -> Unit,
    ) {
        worker.execute {
            val bmp = runCatching { render(centerLat, centerLng, blockMeters, outSize) }
                .onFailure { Log.w(TAG, "render failed", it) }
                .getOrNull()
            handler.post {
                if (alive) done(bmp) else bmp?.recycle()
            }
        }
    }

    fun close() {
        alive = false
        worker.shutdownNow()
        io.shutdownNow()
        cache.evictAll()
    }

    private fun render(centerLat: Double, centerLng: Double, blockMeters: Double, outSize: Int): Bitmap? {
        val lat = centerLat.coerceIn(-85.0, 85.0)
        val targetMpp = blockMeters / outSize
        // 比理论层级高一级，多取支路；输出仍会缩到 outSize。
        var zoom = (floor(log2(156543.03392 * cos(Math.toRadians(lat)) / targetMpp)).toInt() + 1)
            .coerceIn(2, 18)
        val half = blockMeters / 2.0
        val dLat = half / 111320.0
        val dLng = half / (111320.0 * max(1e-6, cos(Math.toRadians(lat))))
        val nwLat = lat + dLat
        val seLat = lat - dLat
        val nwLng = centerLng - dLng
        val seLng = centerLng + dLng

        var x0: Int
        var x1: Int
        var y0: Int
        var y1: Int
        while (true) {
            x0 = floor(tileX(nwLng, zoom)).toInt()
            x1 = floor(tileX(seLng, zoom)).toInt()
            y0 = floor(tileY(nwLat, zoom)).toInt()
            y1 = floor(tileY(seLat, zoom)).toInt()
            if (x1 - x0 + 1 <= MAX_TILES_PER_SIDE && y1 - y0 + 1 <= MAX_TILES_PER_SIDE) break
            zoom--
            if (zoom <= 2) break
        }
        val coords = ArrayList<Triple<Int, Int, Int>>()
        for (x in x0..x1) for (y in y0..y1) coords.add(Triple(x, y, zoom))
        Log.d(TAG, "render zoom=$zoom tiles=${coords.size} block=$blockMeters center=$lat,$centerLng")
        if (coords.size > MAX_TILES_PER_SIDE * MAX_TILES_PER_SIDE) return null

        val missing = coords.filter { cache.get(key(it.third, it.first, it.second)) == null }
        Log.d(TAG, "missing=${missing.size}")
        val futures = missing.map { c ->
            io.submit(Callable { fetchTile(c.third, c.first, c.second) })
        }
        futures.forEach { runCatching { it.get() } }

        val width = (x1 - x0 + 1) * TILE
        val height = (y1 - y0 + 1) * TILE
        val canvasBmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(canvasBmp)
        canvas.drawColor(0xFFF5F0E8.toInt())
        var drawn = 0
        for (x in x0..x1) for (y in y0..y1) {
            val t = cache.get(key(zoom, x, y)) ?: continue
            canvas.drawBitmap(t, ((x - x0) * TILE).toFloat(), ((y - y0) * TILE).toFloat(), null)
            drawn++
        }
        Log.d(TAG, "drawn=$drawn size=${width}x${height}")
        if (drawn == 0) {
            canvasBmp.recycle()
            return null
        }

        val left = floor(tileX(nwLng, zoom) * TILE - x0 * TILE).toInt().coerceIn(0, width - 1)
        val right = ceil(tileX(seLng, zoom) * TILE - x0 * TILE).toInt().coerceIn(left + 1, width)
        val top = floor(tileY(nwLat, zoom) * TILE - y0 * TILE).toInt().coerceIn(0, height - 1)
        val bottom = ceil(tileY(seLat, zoom) * TILE - y0 * TILE).toInt().coerceIn(top + 1, height)
        val cropped = Bitmap.createBitmap(canvasBmp, left, top, right - left, bottom - top)
        val out = Bitmap.createScaledBitmap(cropped, outSize, outSize, true)
        if (cropped !== out) cropped.recycle()
        canvasBmp.recycle()
        return out
    }

    private fun fetchTile(z: Int, x: Int, y: Int): Bitmap? {
        val k = key(z, x, y)
        cache.get(k)?.let { return it }
        val key = BuildConfig.TIANDITU_KEY.trim()
        if (key.isNotEmpty() && !directBlocked) {
            val host = "t${(x + y) % 8}.tianditu.gov.cn"
            val url = "https://$host/DataServer?T=vec_w&x=$x&y=$y&l=$z&tk=${URLEncoder.encode(key, "UTF-8")}"
            download(url, direct = true)?.let { bytes ->
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.let {
                    cache.put(k, it)
                    return it
                }
            }
        }
        val proxy = "$PROXY?layer=road&z=$z&x=$x&y=$y"
        download(proxy, direct = false)?.let { bytes ->
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.let {
                cache.put(k, it)
                return it
            }
        }
        Log.w(TAG, "tile $z/$x/$y failed")
        return null
    }

    private fun download(url: String, direct: Boolean): ByteArray? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 6000
                readTimeout = 8000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", "NavBridge/0.9")
                setRequestProperty("Accept", "image/png,image/*")
            }
            val code = conn.responseCode
            if (code != 200) {
                if (direct && code == 403) directBlocked = true
                Log.w(TAG, "HTTP $code ${if (direct) "direct" else "proxy"}")
                return null
            }
            val input = conn.inputStream
            val out = ByteArrayOutputStream()
            val buf = ByteArray(8192)
            var total = 0
            while (true) {
                val n = input.read(buf)
                if (n <= 0) break
                total += n
                if (total > 1024 * 1024) break
                out.write(buf, 0, n)
            }
            input.close()
            out.toByteArray()
        } catch (e: Exception) {
            Log.w(TAG, "download failed: ${e.javaClass.simpleName} ${e.message} $url")
            null
        } finally {
            conn?.disconnect()
        }
    }

    private fun key(z: Int, x: Int, y: Int): String = "$z/$x/$y"

    private fun tileX(lng: Double, zoom: Int): Double =
        (lng + 180.0) / 360.0 * (1 shl zoom)

    private fun tileY(lat: Double, zoom: Int): Double {
        val r = Math.toRadians(lat)
        return (1.0 - Math.log(Math.tan(r) + 1.0 / Math.cos(r)) / Math.PI) / 2.0 * (1 shl zoom)
    }
}
