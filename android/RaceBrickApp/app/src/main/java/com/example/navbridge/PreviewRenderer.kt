package com.example.navbridge

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * 手机端导航画面模拟（320×240 横向）。
 *
 * 不放大方案：
 * - 地图块抓 7500×7500 米
 * - 可见画面 320×240 对应 2400×1800 米（7.5 米/像素）
 * - 车头朝上时围绕车标旋转，四周保留足够余量补角；2x 超采样仍在手机端完成
 */
object PreviewRenderer {

    const val OUT_W = 320
    const val OUT_H = 240
    const val BASE = 1000
    /** Tianditu 底图：800px 足够保留支路细节，再经 2× 超采样缩到 320×240。 */
    const val TDT_BASE = 800
    const val BLOCK = 7500.0
    private const val SS = 2

    // MotoNav 默认主题（罪恶都市）：品红路线 + 深品红描边。
    private val ROUTE_COLOR = 0xFFF749C5.toInt()
    private val ROUTE_CASE = 0xFF7B1C63.toInt()

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

    /** 高德截图 → BASE×BASE 的底图（覆盖整个 7500m 块）。 */
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

    /** 4 色索引图（BASE×BASE）→ 当前风格底图。 */
    fun effectBase(
        idx: ByteArray,
        n: Int,
        out: Int = BASE,
        style: MapStyle = MapStyles.DEFAULT,
    ): Bitmap = RasterCodec.previewBitmap(idx, n, out, style.baseColors)

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
        speedLimit: Int = 0,
        mapStyle: MapStyle = MapStyles.DEFAULT,
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

        // 2x supersampling: lines/text are rendered at 640x480 then
        // downsampled on the phone, which softens the stair-step edges before
        // the 16-colour frame quantization.
        val hi = Bitmap.createBitmap(OUT_W * SS, OUT_H * SS, Bitmap.Config.ARGB_8888)
        val c = Canvas(hi)
        c.drawColor(Color.BLACK)
        c.scale(SS.toFloat(), SS.toFloat())

        c.save()
        c.scale(zoom, zoom)
        c.translate(-cropLeft, -cropTop)
        if (headingUp) c.rotate(-heading, rbx, rby)
        c.drawBitmap(base, 0f, 0f, null)
        if (route != null && route.size >= 4) {
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = mapStyle.route
                style = Paint.Style.STROKE
                // 缩放后线宽会跟着变，补偿回固定视觉粗细
                strokeWidth = OUT_W / 64f / zoom
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
            if (!sdkOriginal) {
                // MotoNav 风格的路线描边是深品红，不是纯黑。
                val casing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = mapStyle.routeCase
                    style = Paint.Style.STROKE
                    strokeWidth = (OUT_W / 64f + 2.5f) / zoom
                    strokeCap = Paint.Cap.ROUND
                    strokeJoin = Paint.Join.ROUND
                }
                c.drawPath(path, casing)
            }
            c.drawPath(path, paint)
        }
        if (traffic.isNotEmpty()) {
            val tw = OUT_W / 64f / zoom
            for (seg in traffic) {
                // MotoNav 只把缓行/拥堵叠到路线上；畅通段保留品红路线，不铺绿线。
                if (seg.status <= 1 || seg.pts.size < 4) continue
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
                if (!sdkOriginal) {
                    val casing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                        color = mapStyle.routeCase
                        style = Paint.Style.STROKE
                        strokeWidth = (OUT_W / 64f + 2.5f) / zoom
                        strokeCap = Paint.Cap.ROUND
                        strokeJoin = Paint.Join.ROUND
                    }
                    c.drawPath(path, casing)
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
            val (sx, sy) = cameraPos(cam.x, cam.y)
            if (sx < 10f || sx > OUT_W - 10f || sy < 10f || sy > OUT_H - 10f) continue
            if (cam.type == 2) {
                drawTrafficLightMarker(c, sx, sy)
                continue
            }
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

        // 车标：MotoNav 的白色箭头 + 深色描边，不再用蓝色圆点。
        val sunkX = (rbx - cropLeft) * zoom
        val sunY = (rby - cropTop) * zoom
        val arrowHeading = if (headingUp) 0f else heading
        c.save()
        c.translate(sunkX, sunY)
        c.rotate(arrowHeading)
        c.drawPath(riderArrowPath(1.35f, 1.2f), Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.FILL
        })
        c.drawPath(riderArrowPath(1f, 0f), Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.FILL
        })
        c.restore()

        drawHud(c, hud, OUT_W.toFloat(), OUT_H.toFloat(), speedLimit, laneBg, laneFr, mapStyle, sdkOriginal)
        val out = Bitmap.createScaledBitmap(hi, OUT_W, OUT_H, true)
        hi.recycle()
        return out
    }

    private fun trafficColor(status: Int): Int = when (status) {
        2 -> 0xFFFFB600.toInt() // 缓行
        3 -> 0xFFEE1C20.toInt() // 拥堵
        4 -> 0xFF940000.toInt() // 严重拥堵
        else -> ROUTE_COLOR
    }

    private fun drawHud(
        c: Canvas,
        hud: NavHud,
        w: Float,
        h: Float,
        speedLimit: Int,
        laneBg: IntArray,
        laneFr: IntArray,
        mapStyle: MapStyle,
        sdkOriginal: Boolean,
    ) {
        val hudColor = if (sdkOriginal) Color.BLACK else mapStyle.hud
        fun paint(size: Float, color: Int, bold: Boolean = false): Paint =
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                textSize = size
                this.color = color
                if (bold) typeface = Typeface.DEFAULT_BOLD
                if (!sdkOriginal) setShadowLayer(1.4f, 0f, 1.4f, Color.BLACK)
            }
        fun infoPanel(rect: RectF) {
            c.drawRoundRect(rect, 4f, 4f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE })
            if (mapStyle.light) {
                c.drawRoundRect(rect, 4f, 4f, Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = Color.BLACK
                    style = Paint.Style.STROKE
                    strokeWidth = 1.2f
                })
            }
        }

        // 左上：转向箭头 + 距离，放在白色信息底框里；浅色风格带黑边。
        val arrowText = turnArrow(hud.dir)
        val distText = distance(hud.dist)
        val arrowPaint = paint(w / 11f, Color.BLACK, true)
        val distPaint = paint(w / 13f, Color.BLACK, true)
        val gap = w * 0.018f
        val padX = w * 0.015f
        val padY = 2.5f
        val textH = maxOf(
            arrowPaint.fontMetrics.descent - arrowPaint.fontMetrics.ascent,
            distPaint.fontMetrics.descent - distPaint.fontMetrics.ascent,
        )
        val turnCy = h * 0.11f
        val panelLeft = w * 0.025f
        val panelTop = turnCy - textH / 2 - padY
        val panelRight = panelLeft + arrowPaint.measureText(arrowText) + gap +
            distPaint.measureText(distText) + padX * 2
        infoPanel(RectF(panelLeft, panelTop, panelRight, panelTop + textH + padY * 2))
        val baseline = turnCy - (arrowPaint.fontMetrics.ascent + arrowPaint.fontMetrics.descent) / 2
        c.drawText(arrowText, panelLeft + padX, baseline, arrowPaint)
        c.drawText(
            distText,
            panelLeft + padX + arrowPaint.measureText(arrowText) + gap,
            baseline,
            distPaint,
        )

        // 顶部中间：限速；其下：车道背景 + 推荐车道
        drawSpeedLimit(c, speedLimit, w, h)
        drawLanes(c, laneBg, laneFr, w, h)

        // 右上：速度圆牌，中心到上沿/右边距离相等；整体略缩小。
        val badgeCy = w * 0.125f
        val badgeCx = w - badgeCy
        val badgeR = w * 0.095f
        val textColor: Int
        c.drawCircle(badgeCx, badgeCy, badgeR, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE })
        c.drawCircle(badgeCx, badgeCy, badgeR, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 2.5f
        })
        textColor = Color.BLACK
        val speed = paint(w / 11.5f, textColor, true).apply { textAlign = Paint.Align.CENTER }
        c.drawText("${hud.speed}", badgeCx, badgeCy + w * 0.010f, speed)
        val unit = paint(w / 36f, textColor).apply { textAlign = Paint.Align.CENTER }
        c.drawText("km/h", badgeCx, badgeCy + w * 0.065f, unit)

        // 左下道路名 + 右下剩余距离/时间：同字号、同底边、都有白色底框。
        val bottomPadX = w * 0.018f
        val bottomPadY = 3f
        val bottomMargin = w * 0.03f
        val bottomY = h - h * 0.025f
        val bottomPaint = paint(w / 24f, Color.BLACK)
        val bm = bottomPaint.fontMetrics
        val bottomH = bm.descent - bm.ascent

        val road = hud.nextRoad.ifBlank { "沿当前道路" }.take(10)
        val roadRect = RectF(
            bottomMargin,
            bottomY - bottomH - bottomPadY * 2,
            bottomMargin + bottomPaint.measureText(road) + bottomPadX * 2,
            bottomY,
        )
        infoPanel(roadRect)
        c.drawText(road, roadRect.left + bottomPadX, roadRect.top + bottomPadY - bm.ascent, bottomPaint)

        val remainText = "${distance(hud.remainM)}  ${duration(hud.remainS)}"
        val remainRight = w - bottomMargin
        val remainRect = RectF(
            remainRight - bottomPaint.measureText(remainText) - bottomPadX * 2,
            bottomY - bottomH - bottomPadY * 2,
            remainRight,
            bottomY,
        )
        infoPanel(remainRect)
        c.drawText(
            remainText,
            remainRect.left + bottomPadX,
            remainRect.top + bottomPadY - bm.ascent,
            bottomPaint,
        )
    }


    /** MotoNav drawPlayer 的箭头轮廓：白芯 + 黑色外框。 */
    private fun riderArrowPath(scale: Float, dy: Float): Path {
        val k = OUT_W / 320f * scale
        return Path().apply {
            moveTo(0f, (-13f * k) + dy)
            lineTo(-10f * k, (10.5f * k) + dy)
            lineTo(0f, (5f * k) + dy)
            lineTo(10f * k, (10.5f * k) + dy)
            close()
        }
    }

    private fun drawTrafficLightMarker(c: Canvas, x: Float, y: Float) {
        val w0 = 10f
        val h0 = 20f
        val left = x - w0 / 2f
        val top = y - h0 / 2f
        c.drawRoundRect(RectF(left - 1f, top - 1f, left + w0 + 1f, top + h0 + 1f),
            4f, 4f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xE6FFFFFF.toInt() })
        c.drawRoundRect(RectF(left, top, left + w0, top + h0),
            3f, 3f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xE0202020.toInt() })
        val cx = x
        val r = 2.6f
        c.drawCircle(cx, top + 4f, r, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFF3B30.toInt() })
        c.drawCircle(cx, top + 10f, r, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFD60A.toInt() })
        c.drawCircle(cx, top + 16f, r, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF34C759.toInt() })
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
