package com.example.navbridge

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PorterDuff
import android.graphics.PorterDuffColorFilter
import android.graphics.RectF
import android.graphics.Typeface
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * 手机端导航画面模拟（320×240 横向）。
 *
 * 不放大方案：
 * - 地图块抓 4650×4650 米
 * - 可见画面 320×240 对应 2400×1800 米（7.5 米/像素）
 * - 车头朝上时围绕车标旋转 620×620 的底图，用块四周的余量补角，不做缩放
 */
object PreviewRenderer {

    const val OUT_W = 320
    const val OUT_H = 240
    const val BASE = 760
    const val BLOCK = 5700.0

    private const val RIDER_X = OUT_W / 2f
    private const val RIDER_Y = OUT_H * 0.70f

    class NavHud(
        val dir: String = "S",
        val dist: Int = -1,
        val nextRoad: String = "",
        val remainM: Int = -1,
        val remainS: Int = -1,
        val lights: Int = 0,
        val lightDist: Int = -1,
        val speed: Int = 0,
    )

    class TrafficSeg(val status: Int, val pts: DoubleArray)
    class CamMarker(val x: Double, val y: Double, val type: Int, val speed: Int)

    /** 高德截图 → BASE×BASE 的底图（覆盖整个 4650m 块）。 */
    fun sdkBase(shot: RasterCodec.Shot, out: Int = BASE): Bitmap {
        val full = Bitmap.createBitmap(shot.pixels, shot.width, shot.height, Bitmap.Config.ARGB_8888)
        val w = shot.width
        val h = shot.height
        val left = shot.left.toInt().coerceIn(0, w - 2)
        val right = shot.right.toInt().coerceIn(left + 2, w)
        val top = shot.top.toInt().coerceIn(0, h - 2)
        val bottom = shot.bottom.toInt().coerceIn(top + 2, h)
        val cropped = Bitmap.createBitmap(full, left, top, right - left, bottom - top)
        val scaled = Bitmap.createScaledBitmap(cropped, out, out, true)
        if (full !== cropped) full.recycle()
        if (cropped !== scaled) cropped.recycle()
        return scaled
    }

    /** 4 色索引图（BASE×BASE）→ 4 色底图。 */
    fun effectBase(idx: ByteArray, n: Int, out: Int = BASE): Bitmap =
        RasterCodec.previewBitmap(idx, n, out)

    fun overlay(
        base: Bitmap,
        route: DoubleArray?,
        blockX0: Int,
        blockY0: Int,
        riderX: Double,
        riderY: Double,
        heading: Float,
        headingUp: Boolean,
        hud: NavHud,
        zoom: Float = 1f,
        traffic: List<TrafficSeg> = emptyList(),
        cameras: List<CamMarker> = emptyList(),
        laneBg: IntArray = IntArray(0),
        laneFr: IntArray = IntArray(0),
        turnIcon: Bitmap? = null,
        speedLimit: Int = 0,
        sdkOriginal: Boolean = true,
    ): Bitmap {
        val s = base.width.toFloat()
        fun px(x: Double): Float = ((x - blockX0) / BLOCK * s).toFloat()
        fun py(y: Double): Float = ((blockY0 - y) / BLOCK * s).toFloat()

        val rbx = px(riderX)
        val rby = py(riderY)
        val cropW = OUT_W / zoom
        val cropH = OUT_H / zoom
        val cropLeft = (rbx - RIDER_X / zoom).coerceIn(0f, (s - cropW).coerceAtLeast(0f))
        val cropTop = (rby - RIDER_Y / zoom).coerceIn(0f, (s - cropH).coerceAtLeast(0f))

        val out = Bitmap.createBitmap(OUT_W, OUT_H, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        c.drawColor(Color.BLACK)

        c.save()
        c.scale(zoom, zoom)
        c.translate(-cropLeft, -cropTop)
        if (headingUp) c.rotate(-heading, rbx, rby)
        c.drawBitmap(base, 0f, 0f, null)
        if (route != null && route.size >= 4) {
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xFFB45CFF.toInt()
                style = Paint.Style.STROKE
                // 缩放后线宽会跟着变，补偿回固定视觉粗细
                strokeWidth = OUT_W / 90f / zoom
                strokeCap = Paint.Cap.ROUND
                strokeJoin = Paint.Join.ROUND
            }
            val path = Path()
            path.moveTo(px(route[0]), py(route[1]))
            var i = 2
            while (i + 1 < route.size) {
                path.lineTo(px(route[i]), py(route[i + 1]))
                i += 2
            }
            c.drawPath(path, paint)
        }
        if (traffic.isNotEmpty()) {
            val tw = OUT_W / 80f / zoom
            for (seg in traffic) {
                if (seg.pts.size < 4) continue
                val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = trafficColor(seg.status)
                    style = Paint.Style.STROKE
                    strokeWidth = tw
                    strokeCap = Paint.Cap.ROUND
                    strokeJoin = Paint.Join.ROUND
                }
                val path = Path()
                path.moveTo(px(seg.pts[0]), py(seg.pts[1]))
                var i = 2
                while (i + 1 < seg.pts.size) {
                    path.lineTo(px(seg.pts[i]), py(seg.pts[i + 1]))
                    i += 2
                }
                c.drawPath(path, paint)
            }
        }
        c.restore()

        // 电子眼：按地图变换算出屏幕位置，图标保持正立
        fun cameraPos(x: Double, y: Double): Pair<Float, Float> {
            val bx = px(x)
            val by = py(y)
            return if (headingUp) {
                val rad = Math.toRadians(-heading.toDouble())
                val dx = bx - rbx
                val dy = by - rby
                val rx = dx * cos(rad) - dy * sin(rad)
                val ry = dx * sin(rad) + dy * cos(rad)
                (((rx + rbx - cropLeft) * zoom).toFloat()) to (((ry + rby - cropTop) * zoom).toFloat())
            } else {
                (((bx - cropLeft) * zoom).toFloat()) to (((by - cropTop) * zoom).toFloat())
            }
        }
        for (cam in cameras.take(24)) {
            if (cam.type == 2) continue // 红绿灯点位不画
            val (sx, sy) = cameraPos(cam.x, cam.y)
            if (sx < 8f || sx > OUT_W - 8f || sy < 8f || sy > OUT_H - 8f) continue
            val col = when (cam.type) {
                0, 8, 9, 10 -> 0xFFE53935.toInt()
                1 -> 0xFF1E88E5.toInt()
                3 -> 0xFFD81B60.toInt()
                else -> 0xFFFB8C00.toInt()
            }
            c.drawCircle(sx, sy, 7f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = col })
            c.drawCircle(sx, sy, 7f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE
                style = Paint.Style.STROKE
                strokeWidth = 1.5f
            })
            val label = when (cam.type) {
                0, 8, 9, 10 -> "速"
                1 -> "监"
                3 -> "违"
                else -> "电"
            }
            c.drawText(label, sx, sy + 3.5f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE
                textSize = 8f
                textAlign = Paint.Align.CENTER
                typeface = Typeface.DEFAULT_BOLD
            })
            if (cam.speed > 0 && (cam.type == 0 || cam.type == 8 || cam.type == 9 || cam.type == 10)) {
                c.drawText("${cam.speed}", sx, sy + 19f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = Color.WHITE
                    textSize = 8f
                    textAlign = Paint.Align.CENTER
                    typeface = Typeface.DEFAULT_BOLD
                })
            }
        }

        // 车标：车头朝上时箭头固定朝上，朝北时按 heading 旋转
        val sunkX = (rbx - cropLeft) * zoom
        val sunY = (rby - cropTop) * zoom
        val arrowHeading = if (headingUp) 0f else heading
        val r = OUT_W / 42f
        c.drawCircle(sunkX, sunY, r, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFF00E5FF.toInt()
            style = Paint.Style.FILL
        })
        c.drawCircle(sunkX, sunY, r, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = OUT_W / 220f
        })
        val rad = Math.toRadians(arrowHeading.toDouble())
        c.drawLine(
            sunkX, sunY,
            sunkX + (sin(rad) * r * 1.9f).toFloat(),
            sunY - (cos(rad) * r * 1.9f).toFloat(),
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE
                strokeWidth = OUT_W / 130f
                strokeCap = Paint.Cap.ROUND
            },
        )

        drawHud(c, hud, OUT_W.toFloat(), OUT_H.toFloat(), turnIcon, speedLimit, laneBg, laneFr, sdkOriginal)
        return out
    }

    private fun trafficColor(status: Int): Int = when (status) {
        1 -> 0xFF34C759.toInt() // 畅通
        2 -> 0xFFFFD60A.toInt() // 缓行
        3 -> 0xFFFF3B30.toInt() // 拥堵
        4 -> 0xFFB00020.toInt() // 严重拥堵
        else -> 0xFFB45CFF.toInt()
    }

    private fun drawHud(
        c: Canvas,
        hud: NavHud,
        w: Float,
        h: Float,
        turnIcon: Bitmap?,
        speedLimit: Int,
        laneBg: IntArray,
        laneFr: IntArray,
        sdkOriginal: Boolean,
    ) {
        val hudColor = if (sdkOriginal) Color.BLACK else 0xFF1E88E5.toInt()
        val speedColor = 0xFF44FF44.toInt()
        fun paint(size: Float, color: Int, bold: Boolean = false): Paint =
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                textSize = size
                this.color = color
                if (bold) typeface = Typeface.DEFAULT_BOLD
            }

        // 左上：官方转向图标（没有就用箭头字符）+ 到下一动作距离
        if (turnIcon != null && !turnIcon.isRecycled) {
            val size = w * 0.115f
            c.drawBitmap(
                turnIcon,
                null,
                RectF(w * 0.025f, h * 0.065f, w * 0.025f + size, h * 0.065f + size),
                Paint(Paint.FILTER_BITMAP_FLAG).apply {
                    colorFilter = PorterDuffColorFilter(hudColor, PorterDuff.Mode.SRC_IN)
                },
            )
        } else {
            c.drawText(turnArrow(hud.dir), w * 0.025f, h * 0.235f, paint(w / 8f, hudColor, true))
        }
        c.drawText(distance(hud.dist), w * 0.17f, h * 0.165f, paint(w / 11f, hudColor, true))

        // 顶部中间：限速；其下：车道背景 + 推荐车道
        drawSpeedLimit(c, speedLimit, w, h)
        drawLanes(c, laneBg, laneFr, w, h)

        // 右上：速度
        val speed = paint(w / 10f, speedColor, true).apply { textAlign = Paint.Align.RIGHT }
        c.drawText("${hud.speed}", w - w * 0.03f, h * 0.18f, speed)
        val unit = paint(w / 28f, speedColor).apply { textAlign = Paint.Align.RIGHT }
        c.drawText("km/h", w - w * 0.03f, h * 0.27f, unit)

        // 左下：下一条路 + 剩余距离/时间
        val road = hud.nextRoad.ifBlank { "沿当前道路" }.take(10)
        c.drawText(road, w * 0.03f, h - h * 0.105f, paint(w / 21f, hudColor))
        c.drawText(
            "${distance(hud.remainM)}  ${duration(hud.remainS)}",
            w * 0.03f,
            h - h * 0.025f,
            paint(w / 24f, hudColor),
        )
    }

    private fun drawSpeedLimit(c: Canvas, limit: Int, w: Float, h: Float) {
        if (limit <= 0) return
        val cx = w / 2f
        val cy = h * 0.115f
        val r = 14f
        c.drawCircle(cx, cy, r + 2f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x99000000.toInt() })
        c.drawCircle(cx, cy, r, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE })
        c.drawCircle(cx, cy, r, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xFFD32F2F.toInt()
            style = Paint.Style.STROKE
            strokeWidth = 3f
        })
        c.drawText("$limit", cx, cy + 5f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            textSize = if (limit >= 100) w / 30f else w / 20f
            typeface = Typeface.DEFAULT_BOLD
            textAlign = Paint.Align.CENTER
        })
    }

    private fun drawLanes(c: Canvas, bg: IntArray, fr: IntArray, w: Float, h: Float) {
        val n = min(min(bg.size, fr.size), 8)
        if (n <= 0) return
        val laneW = min(22f, (w - 30f) / n)
        val laneH = 28f
        val total = laneW * n
        val x0 = (w - total) / 2f
        val y0 = h * 0.22f

        c.drawRoundRect(
            RectF(x0 - 3f, y0 - 2f, x0 + total + 3f, y0 + laneH + 2f),
            6f, 6f,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x66000000.toInt() },
        )
        for (i in 0 until n) {
            val recommended = fr[i] in 0..14
            val code = if (recommended) fr[i] else bg[i]
            val rect = RectF(x0 + i * laneW + 1f, y0, x0 + (i + 1) * laneW - 1f, y0 + laneH)
            c.drawRoundRect(rect, 4f, 4f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = if (recommended) 0xCC0A8F5B.toInt() else 0xAA202020.toInt()
            })
            val gp = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = if (recommended) Color.WHITE else 0xFFAAAAAA.toInt()
                textSize = laneW * 0.78f
                textAlign = Paint.Align.CENTER
                typeface = Typeface.DEFAULT_BOLD
            }
            c.drawText(laneGlyph(code), rect.centerX(), rect.bottom - 6f, gp)
        }
    }

    private fun laneGlyph(code: Int): String = when (code) {
        1 -> "←"
        2 -> "↖"
        3 -> "→"
        4 -> "↗"
        5 -> "↺"
        6 -> "↔"
        7 -> "↔"
        8 -> "↻"
        9 -> "↺"
        10 -> "↻"
        11 -> "↺"
        12 -> "↻"
        else -> "↑"
    }

    private fun turnArrow(dir: String): String = when (dir) {
        "L" -> "←"
        "R" -> "→"
        "SL" -> "↖"
        "SR" -> "↗"
        "U" -> "↺"
        "A" -> "✓"
        else -> "↑"
    }

    private fun distance(m: Int): String = when {
        m < 0 -> "--"
        m >= 1000 -> String.format(java.util.Locale.US, "%.1fkm", m / 1000.0)
        else -> "${m}m"
    }

    private fun duration(s: Int): String = when {
        s < 0 -> "--"
        s >= 3600 -> String.format(java.util.Locale.US, "%.1fh", s / 3600.0)
        else -> "${s / 60}min"
    }
}
