package com.example.navbridge

import android.graphics.Bitmap

/**
 * 手机整帧渲染的最后一环：320×240 全帧 → 16 色索引 → 调色板 RLE。
 *
 * 整帧已经包含底图、路线、车标、HUD，所以设备端只需要解码 + 刷屏，
 * 不需要在 ESP32 上再实现地图/中文 HUD。
 *
 * 包格式与 RasterCodec 的调色板格式一致：
 * u16 frameId, i32 x0, i32 y0, u16 w, u16 h, u16 (mm|0x8000),
 * 16 × u16 RGB565 palette, RLE(低4位颜色, 高4位 run-1)。
 */
object FrameCodec {
    const val W = 320
    const val H = 240

    // 每种风格有自己的 16 色调色板；LUT 首次使用时构建并缓存。
    private val luts = HashMap<String, ByteArray>()

    fun encode(frameId: Int, indices: ByteArray, style: MapStyle): ByteArray =
        RasterCodec.encodeRaster(0, 0, 0, W, H, 1000, indices, style.palette565)

    /** 把 320×240（或任意尺寸）的 ARGB 帧量化成 320×240 的 16 色索引。 */
    fun quantize(bitmap: Bitmap, style: MapStyle): ByteArray {
        val w = bitmap.width
        val h = bitmap.height
        val px = IntArray(w * h)
        bitmap.getPixels(px, 0, w, 0, 0, w, h)
        val out = ByteArray(W * H)
        val lut = luts.getOrPut(style.id) { buildLut(style.palette) }
        for (y in 0 until H) {
            val sy = y * h / H
            for (x in 0 until W) {
                val sx = x * w / W
                val c = px[sy * w + sx]
                val r = (c shr 16) and 255
                val g = (c shr 8) and 255
                val b = c and 255
                out[y * W + x] = lut[((r shr 3) shl 10) or ((g shr 3) shl 5) or (b shr 3)]
            }
        }
        return out
    }

    private fun buildLut(palette: IntArray): ByteArray {
        val lut = ByteArray(32 * 32 * 32)
        for (r in 0..31) {
            val rr = r * 255 / 31
            for (g in 0..31) {
                val gg = g * 255 / 31
                for (b in 0..31) {
                    val bb = b * 255 / 31
                    var best = 0
                    var bestD = Int.MAX_VALUE
                    for (i in palette.indices) {
                        val p = palette[i]
                        val dr = rr - ((p shr 16) and 255)
                        val dg = gg - ((p shr 8) and 255)
                        val db = bb - (p and 255)
                        val d = dr * dr + dg * dg + db * db
                        if (d < bestD) { bestD = d; best = i }
                    }
                    lut[(r shl 10) or (g shl 5) or b] = best.toByte()
                }
            }
        }
        return lut
    }


}
