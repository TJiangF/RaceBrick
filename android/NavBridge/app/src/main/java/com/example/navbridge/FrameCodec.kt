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

    // 16 色调色板：地图 + HUD 常用色
    val PALETTE = intArrayOf(
        0xFF000000.toInt(), // 0 黑（HUD 描边/文字）
        0xFF2B2B2B.toInt(), // 1 深灰
        0xFF6E6E6E.toInt(), // 2 中灰（地图文字）
        0xFFB0B0B0.toInt(), // 3 浅灰（普通道路/建筑）
        0xFFF0F0F0.toInt(), // 4 白（道路/圆牌）
        0xFFF5F0E8.toInt(), // 5 米白（地图背景）
        0xFF8FC8E8.toInt(), // 6 浅蓝（河/水面）
        0xFF1E88E5.toInt(), // 7 蓝（HUD 像素风文字/电子眼）
        0xFFA8D8A0.toInt(), // 8 浅绿（公园/绿地）
        0xFF2E6B3A.toInt(), // 9 深绿（交通畅通/绿地）
        0xFFF0C850.toInt(), // 10 黄（高架/主干道）
        0xFFFF9500.toInt(), // 11 橙（高架/电子眼）
        0xFFFF3B30.toInt(), // 12 红（拥堵/电子眼）
        0xFFB00020.toInt(), // 13 深红（严重拥堵）
        0xFFB45CFF.toInt(), // 14 路线紫
        0xFF00E5FF.toInt(), // 15 车标青
    )

    val PALETTE565: IntArray = IntArray(16) { argbTo565(PALETTE[it]) }

    private val LUT: ByteArray by lazy { buildLut() }

    fun encode(frameId: Int, indices: ByteArray): ByteArray =
        RasterCodec.encodeRaster(0, 0, 0, W, H, 1000, indices, PALETTE565)

    /** 把 320×240（或任意尺寸）的 ARGB 帧量化成 320×240 的 16 色索引。 */
    fun quantize(bitmap: Bitmap): ByteArray {
        val w = bitmap.width
        val h = bitmap.height
        val px = IntArray(w * h)
        bitmap.getPixels(px, 0, w, 0, 0, w, h)
        val out = ByteArray(W * H)
        val lut = LUT
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

    private fun buildLut(): ByteArray {
        val lut = ByteArray(32 * 32 * 32)
        for (r in 0..31) {
            val rr = r * 255 / 31
            for (g in 0..31) {
                val gg = g * 255 / 31
                for (b in 0..31) {
                    val bb = b * 255 / 31
                    var best = 0
                    var bestD = Int.MAX_VALUE
                    for (i in PALETTE.indices) {
                        val p = PALETTE[i]
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

    private fun argbTo565(c: Int): Int {
        val r = (c shr 16) and 255
        val g = (c shr 8) and 255
        val b = c and 255
        return ((r and 0xF8) shl 8) or ((g and 0xFC) shl 3) or (b shr 3)
    }
}
