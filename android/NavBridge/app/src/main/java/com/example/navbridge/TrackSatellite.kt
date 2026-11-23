package com.example.navbridge

import android.graphics.Bitmap
import android.graphics.RectF
import android.net.Network
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.cos
import kotlin.math.max

data class TrackSatellite(
    val bitmap: Bitmap,
    val pal16: ByteArray,
    val bounds: RectF,
    val spanM: Double,
)

object TrackSatelliteApi {
    private const val NAV_URL = "https://racebrick.tfworks.cn/api/v1/nav"
    private const val SIZE = 256
    private const val GEO_R = 6378137.0
    private val seq = AtomicInteger(1)

    fun load(detail: TrackDetail, network: Network? = null): TrackSatellite? {
        val geo = detail.geoRef ?: return null
        val pts = detail.points.firstOrNull()?.points ?: return null
        if (pts.size < 2 || geo.nscale <= 1e-9) return null
        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        for (p in pts) {
            minX = minOf(minX, p.x); maxX = maxOf(maxX, p.x)
            minY = minOf(minY, p.y); maxY = maxOf(maxY, p.y)
        }
        val cx = (minX + maxX) / 2f
        val cy = (minY + maxY) / 2f
        val spanM = (max(maxX - minX, maxY - minY) / geo.nscale * 1.12)
            .coerceIn(60.0, 5000.0)
        val mx = (cx - geo.nox) / geo.nscale
        val my = (cy - geo.noy) / geo.nscale
        val lat = geo.lat0 + (my / GEO_R) * (180.0 / Math.PI)
        val lon = geo.lon0 + (mx / (GEO_R * cos(Math.toRadians(geo.lat0)))) * (180.0 / Math.PI)
        val body = JSONObject()
            .put("device_id", "android-navbridge")
            .put("seq", seq.getAndIncrement())
            .put("lng", lon)
            .put("lat", lat)
            .put("size_m", spanM)
            .put("heading", 0)
            .put("size", SIZE)
            .put("width", SIZE)
            .put("height", SIZE)
            .put("zoom", 1.0)
            .put("format", "pal16")
            .put("mode", "satellite")
            .put("coord", "wgs84")
            .toString()
            .toByteArray(Charsets.UTF_8)

        val url = java.net.URL(NAV_URL)
        val conn = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = 5000
            conn.readTimeout = 12000
            conn.useCaches = false
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/json")
            conn.setRequestProperty("Accept", "application/octet-stream")
            conn.setFixedLengthStreamingMode(body.size)
            conn.outputStream.use { it.write(body) }
            if (conn.responseCode !in 200..299) {
                throw IllegalStateException("HTTP ${conn.responseCode}")
            }
            val data = BufferedInputStream(conn.inputStream).use { input ->
                val out = ByteArrayOutputStream()
                val buf = ByteArray(8192)
                while (true) {
                    val n = input.read(buf)
                    if (n <= 0) break
                    out.write(buf, 0, n)
                }
                out.toByteArray()
            }
            val actualSpan = conn.getHeaderField("X-Nav-Span-M")?.toDoubleOrNull() ?: spanM
            val decoded = decode(data) ?: return null
            val half = actualSpan * geo.nscale / 2.0
            val bounds = RectF(
                (cx - half).toFloat(), (cy - half).toFloat(),
                (cx + half).toFloat(), (cy + half).toFloat(),
            )
            return TrackSatellite(decoded, data, bounds, actualSpan)
        } finally {
            conn.disconnect()
        }
    }

    private fun decode(data: ByteArray): Bitmap? {
        if (data.size < 6) return null
        fun u8(i: Int) = data[i].toInt() and 0xFF
        val w = u8(0) or (u8(1) shl 8)
        val h = u8(2) or (u8(3) shl 8)
        val pc = u8(4) or (u8(5) shl 8)
        if (w <= 0 || h <= 0 || w > 320 || h > 320 || pc <= 0 || pc > 16) return null
        if (data.size < 6 + pc * 2) return null
        val pal = IntArray(pc)
        var p = 6
        for (i in 0 until pc) {
            val c = u8(p) or (u8(p + 1) shl 8)
            val r = ((c shr 11) and 31) * 255 / 31
            val g = ((c shr 5) and 63) * 255 / 63
            val b = (c and 31) * 255 / 31
            pal[i] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
            p += 2
        }
        val px = IntArray(w * h)
        var k = 0
        while (p < data.size && k < px.size) {
            val v = u8(p++)
            var run = (v shr 4) + 1
            val col = v and 0x0F
            if ((v shr 4) == 15) {
                if (p >= data.size) break
                run = 16 + u8(p++)
            }
            val c = pal.getOrElse(col) { 0 }
            val end = minOf(px.size, k + run)
            while (k < end) px[k++] = c
        }
        if (k != px.size) return null
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
    }
}
