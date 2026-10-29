package com.example.navbridge

import android.graphics.Bitmap
import java.io.ByteArrayOutputStream
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * 底图位图 -> 4 色 RLE 包，以及路线折线包。
 *
 * 与 MotoNav 固件协议一致：
 * 底图：u16 frameId, i32 x0, i32 y0, u16 w, u16 h, u16 每像素毫米数,
 *       之后每字节 低4位=颜色、高4位=重复次数-1（15 表示再读 1 字节：16+该字节）。
 * 路线：u16 frameId, i32 x0, i32 y0, u16 点数, 之后每点 i16 dx, i16 dy（相对上一点）。
 */
object RasterCodec {
    const val PX_BLOCK = 0
    const val PX_ROAD = 1
    const val PX_PARK = 2
    const val PX_WATER = 3

    class Shot(
        val pixels: IntArray,
        val width: Int,
        val height: Int,
        val left: Float,
        val top: Float,
        val right: Float,
        val bottom: Float,
    )

    private fun lum(c: Int): Int =
        (((c shr 16) and 255) * 30 + ((c shr 8) and 255) * 59 + (c and 255) * 11) / 100

    /** 背景亮度：低饱和像素里出现最多的亮度（街区底色）。 */
    private fun backgroundLum(px: IntArray): Int {
        val hist = IntArray(256)
        var i = 0
        while (i < px.size) {
            val c = px[i]
            val r = (c shr 16) and 255
            val g = (c shr 8) and 255
            val b = c and 255
            val mx = max(r, max(g, b))
            val mn = min(r, min(g, b))
            if (mx > 0 && (mx - mn) * 100 / mx < 8) hist[lum(c)]++
            i += 3
        }
        var best = 240
        for (l in 0..255) if (hist[l] > hist[best]) best = l
        return best
    }

    private fun classifyPixel(c: Int, bgLum: Int): Int {
        val r = (c shr 16) and 255
        val g = (c shr 8) and 255
        val b = c and 255
        val mx = max(r, max(g, b))
        val mn = min(r, min(g, b))
        val sat = if (mx == 0) 0.0 else (mx - mn).toDouble() / mx
        var hue = 0.0
        if (mx != mn) {
            val d = (mx - mn).toDouble()
            hue = when (mx) {
                r -> 60.0 * (((g - b) / d) % 6)
                g -> 60.0 * ((b - r) / d + 2)
                else -> 60.0 * ((r - g) / d + 4)
            }
            if (hue < 0) hue += 360.0
        }
        if (sat > 0.12 && hue in 180.0..255.0) return PX_WATER
        if (sat > 0.10 && hue in 60.0..170.0) return PX_PARK
        if (sat > 0.18 && hue in 10.0..60.0) return PX_ROAD
        if (lum(c) >= bgLum + 6) return PX_ROAD
        return PX_BLOCK
    }

    private fun dilateRoads(idx: ByteArray, w: Int, h: Int): ByteArray {
        val out = idx.copyOf()
        for (y in 1 until h - 1) {
            val row = y * w
            for (x in 1 until w - 1) {
                val k = row + x
                if (idx[k].toInt() != PX_BLOCK) continue
                if (idx[k - 1].toInt() == PX_ROAD || idx[k + 1].toInt() == PX_ROAD ||
                    idx[k - w].toInt() == PX_ROAD || idx[k + w].toInt() == PX_ROAD
                ) out[k] = PX_ROAD.toByte()
            }
        }
        return out
    }

    private fun sampleBilinear(s: Shot, x: Float, y: Float): Int {
        val fx = x.coerceIn(0f, (s.width - 1).toFloat())
        val fy = y.coerceIn(0f, (s.height - 1).toFloat())
        val x0 = fx.toInt().coerceIn(0, s.width - 1)
        val y0 = fy.toInt().coerceIn(0, s.height - 1)
        val x1 = (x0 + 1).coerceAtMost(s.width - 1)
        val y1 = (y0 + 1).coerceAtMost(s.height - 1)
        val tx = fx - x0
        val ty = fy - y0
        val c00 = s.pixels[y0 * s.width + x0]
        val c10 = s.pixels[y0 * s.width + x1]
        val c01 = s.pixels[y1 * s.width + x0]
        val c11 = s.pixels[y1 * s.width + x1]
        fun ch(c: Int, sh: Int) = (c shr sh) and 255
        fun h(a: Int, b: Int): Float = a + (b - a) * tx
        fun v(a: Float, b: Float): Int = (a + (b - a) * ty).roundToInt()
        val r = v(h(ch(c00, 16), ch(c10, 16)), h(ch(c01, 16), ch(c11, 16)))
        val g = v(h(ch(c00, 8), ch(c10, 8)), h(ch(c01, 8), ch(c11, 8)))
        val bl = v(h(ch(c00, 0), ch(c10, 0)), h(ch(c01, 0), ch(c11, 0)))
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
    }

    /** 把截图的可视区域重采样成 n×n 的 4 色索引图。 */
    fun toIndices(shot: Shot, n: Int): ByteArray {
        val bg = backgroundLum(shot.pixels)
        val idx = ByteArray(n * n)
        val dx = (shot.right - shot.left) / n
        val dy = (shot.bottom - shot.top) / n
        for (py in 0 until n) {
            val sy = shot.top + (py + 0.5f) * dy
            for (px in 0 until n) {
                val sx = shot.left + (px + 0.5f) * dx
                idx[py * n + px] = classifyPixel(sampleBilinear(shot, sx, sy), bg).toByte()
            }
        }
        return dilateRoads(idx, n, n)
    }

    fun previewBitmap(idx: ByteArray, n: Int, out: Int = 240): Bitmap {
        // Racebrick style: brighter ground, white roads, vivid park/water.
        val colors = intArrayOf(0xFF1B1B22.toInt(), 0xFFF2F2F2.toInt(), 0xFF2ECC71.toInt(), 0xFF2F9BFF.toInt())
        val px = IntArray(out * out)
        for (y in 0 until out) {
            val sy = y * n / out
            for (x in 0 until out) {
                val sx = x * n / out
                px[y * out + x] = colors[idx[sy * n + sx].toInt() and 3]
            }
        }
        return Bitmap.createBitmap(px, out, out, Bitmap.Config.ARGB_8888)
    }

    fun encodeRaster(
        frameId: Int,
        x0: Int,
        y0: Int,
        w: Int,
        h: Int,
        mmPerPx: Int,
        idx: ByteArray,
        palette565: IntArray? = null,
    ): ByteArray {
        val b = ByteArrayOutputStream(w * h / 6 + 64)
        fun u8(v: Int) = b.write(v and 0xFF)
        fun u16(v: Int) { u8(v); u8(v shr 8) }
        fun i32(v: Int) { u8(v); u8(v shr 8); u8(v shr 16); u8(v shr 24) }

        u16(frameId); i32(x0); i32(y0); u16(w); u16(h)
        u16(if (palette565 != null) mmPerPx or 0x8000 else mmPerPx)
        if (palette565 != null) {
            for (i in 0 until 16) u16(palette565.getOrElse(i) { 0 })
        }

        var i = 0
        val total = w * h
        while (i < total) {
            val c = idx[i].toInt() and 15
            var run = 1
            while (i + run < total && run < 271 && (idx[i + run].toInt() and 15) == c) run++
            if (run <= 15) u8(((run - 1) shl 4) or c)
            else { u8((15 shl 4) or c); u8(run - 16) }
            i += run
        }
        return b.toByteArray()
    }

    // ---------------- 路线 ----------------

    private fun simplify(pts: DoubleArray, tol: Double): DoubleArray {
        val n = pts.size / 2
        if (n <= 2) return pts
        val keep = BooleanArray(n)
        keep[0] = true
        keep[n - 1] = true
        val stack = ArrayDeque<IntArray>()
        stack.addLast(intArrayOf(0, n - 1))
        val tol2 = tol * tol
        while (stack.isNotEmpty()) {
            val pair = stack.removeLast()
            val s = pair[0]
            val e = pair[1]
            if (e <= s + 1) continue
            val ax = pts[2 * s]; val ay = pts[2 * s + 1]
            val bx = pts[2 * e]; val by = pts[2 * e + 1]
            val dx = bx - ax; val dy = by - ay
            val len2 = dx * dx + dy * dy
            var maxD = -1.0; var idx = -1
            for (i in s + 1 until e) {
                val px = pts[2 * i] - ax; val py = pts[2 * i + 1] - ay
                val d2 = if (len2 < 1e-9) px * px + py * py else {
                    val c = px * dy - py * dx
                    c * c / len2
                }
                if (d2 > maxD) { maxD = d2; idx = i }
            }
            if (maxD > tol2) {
                keep[idx] = true
                stack.addLast(intArrayOf(s, idx))
                stack.addLast(intArrayOf(idx, e))
            }
        }
        val out = ArrayList<Double>()
        for (i in 0 until n) if (keep[i]) { out.add(pts[2 * i]); out.add(pts[2 * i + 1]) }
        return out.toDoubleArray()
    }

    fun encodeRoute(frameId: Int, pts: DoubleArray, maxPts: Int = 20000): ByteArray {
        var s = simplify(pts, 2.0)
        var tol = 2.0
        while (s.size / 2 > maxPts && tol < 60) { tol *= 1.6; s = simplify(pts, tol) }

        val xs = ArrayList<Int>()
        val ys = ArrayList<Int>()
        var lx = 0
        var ly = 0
        for (i in 0 until s.size / 2) {
            val x = s[2 * i].roundToInt()
            val y = s[2 * i + 1].roundToInt()
            if (xs.isNotEmpty()) {
                var ddx = x - lx
                var ddy = y - ly
                while (abs(ddx) > 30000 || abs(ddy) > 30000) {
                    val sx = ddx.coerceIn(-30000, 30000)
                    val sy = ddy.coerceIn(-30000, 30000)
                    lx += sx; ly += sy
                    xs.add(lx); ys.add(ly)
                    ddx = x - lx; ddy = y - ly
                }
                if (x == lx && y == ly) continue
            }
            xs.add(x); ys.add(y); lx = x; ly = y
        }

        val n = min(xs.size, 65535)
        val b = ByteArrayOutputStream(12 + n * 4)
        fun u8(v: Int) = b.write(v and 0xFF)
        fun u16(v: Int) { u8(v); u8(v shr 8) }
        fun i32(v: Int) { u8(v); u8(v shr 8); u8(v shr 16); u8(v shr 24) }

        u16(frameId)
        if (n == 0) { i32(0); i32(0); u16(0); return b.toByteArray() }
        i32(xs[0]); i32(ys[0]); u16(n)
        for (i in 1 until n) { u16(xs[i] - xs[i - 1]); u16(ys[i] - ys[i - 1]) }
        return b.toByteArray()
    }
}
