package com.example.navbridge

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import kotlin.math.max
import kotlin.math.min

class TrackMapView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {
    data class Layer(
        val points: List<PointF>,
        val color: Int,
        val label: String = "",
        val segmentColors: IntArray? = null,
    )
    data class Marker(val point: PointF, val color: Int, val label: String = "")
    data class Satellite(val bitmap: Bitmap, val bounds: RectF)

    private val satellitePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        alpha = 128
        isFilterBitmap = true
    }
    private val satelliteRect = RectF()
    private var satellite: Satellite? = null
    private var marker: Marker? = null
    private val markerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL_AND_STROKE
        strokeWidth = dp(1.5f)
        color = 0xFFFFB300.toInt()
    }
    private val markerTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFFFFFF.toInt()
        textSize = dp(10f)
        typeface = android.graphics.Typeface.DEFAULT_BOLD
    }
    private val pathPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(2.2f)
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(0.5f)
        color = 0xFF1E2830.toInt()
    }
    private val framePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(1f)
        color = 0xFF33434E.toInt()
    }
    private val startPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(3f)
        strokeCap = Paint.Cap.ROUND
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFD7E7E4.toInt()
        textSize = dp(11f)
    }

    private var layers: List<Layer> = emptyList()
    var startF: Float = 0f
        set(value) { field = value; invalidate() }
    var widthM: Float = 5f
        set(value) { field = value; invalidate() }
    var trackLengthM: Float = 0f
        set(value) { field = value; invalidate() }
    var startOffsetM: Float = 0f
        set(value) { field = value; invalidate() }
    var reverse: Boolean = false
        set(value) { field = value; invalidate() }

    fun setLayers(value: List<Layer>) {
        layers = value
        invalidate()
    }

    fun setSatellite(value: Satellite?) {
        satellite = value
        invalidate()
    }

    fun setMarker(value: Marker?) {
        marker = value
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), framePaint)
        if (layers.isEmpty()) {
            canvas.drawText("NO TRACE", dp(10f), height / 2f, textPaint)
            return
        }
        var minX = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        for (layer in layers) for (p in layer.points) {
            minX = min(minX, p.x); maxX = max(maxX, p.x)
            minY = min(minY, p.y); maxY = max(maxY, p.y)
        }
        if (maxX <= minX || maxY <= minY) return
        val pad = dp(14f)
        val sx = (width - pad * 2f) / (maxX - minX)
        val sy = (height - pad * 2f) / (maxY - minY)
        val s = min(sx, sy)
        val ox = (width - (maxX - minX) * s) / 2f
        val oy = (height - (maxY - minY) * s) / 2f

        fun map(p: PointF) = PointF(ox + (p.x - minX) * s, height - oy - (p.y - minY) * s)

        satellite?.let { sat ->
            val a = map(PointF(sat.bounds.left, sat.bounds.top))
            val b = map(PointF(sat.bounds.right, sat.bounds.bottom))
            satelliteRect.set(
                min(a.x, b.x), min(a.y, b.y),
                max(a.x, b.x), max(a.y, b.y),
            )
            canvas.drawBitmap(sat.bitmap, null, satelliteRect, satellitePaint)
        }

        for (layer in layers) {
            if (layer.points.size < 2) continue
            if (layer.segmentColors != null) {
                for (i in 1 until layer.points.size) {
                    val a = map(layer.points[i - 1])
                    val b = map(layer.points[i])
                    pathPaint.color = layer.segmentColors.getOrElse(i - 1) { layer.color }
                    canvas.drawLine(a.x, a.y, b.x, b.y, pathPaint)
                }
            } else {
                val path = Path()
                val first = map(layer.points[0])
                path.moveTo(first.x, first.y)
                for (i in 1 until layer.points.size) {
                    val p = map(layer.points[i])
                    path.lineTo(p.x, p.y)
                }
                pathPaint.color = layer.color
                canvas.drawPath(path, pathPaint)
            }
        }

        val base = layers.first().points
        if (base.size >= 2) {
            val f = startF.coerceIn(0f, 0.9999f)
            val pos = f * (base.size - 1)
            val i = pos.toInt().coerceIn(0, base.size - 2)
            val frac = pos - i
            val a = map(base[i])
            val b = map(base[i + 1])
            val baseX = a.x + (b.x - a.x) * frac
            val baseY = a.y + (b.y - a.y) * frac
            var tx = b.x - a.x
            var ty = b.y - a.y
            val len = max(1f, kotlin.math.hypot(tx, ty))
            tx /= len; ty /= len
            if (reverse) { tx = -tx; ty = -ty }
            val nx = -ty
            val ny = tx
            var normLen = 0f
            for (j in base.indices) {
                val p0 = base[j]
                val p1 = base[(j + 1) % base.size]
                normLen += kotlin.math.hypot(p1.x - p0.x, p1.y - p0.y)
            }
            val offsetPx = if (trackLengthM > 0f && normLen > 1e-6f) {
                startOffsetM * s / (trackLengthM / normLen)
            } else 0f
            val x = baseX + nx * offsetPx
            val y = baseY + ny * offsetPx
            val half = dp(16f) + widthM * 1.4f
            startPaint.color = if (reverse) 0xFFFFB74D.toInt() else 0xFFFF5252.toInt()
            canvas.drawLine(x - nx * half, y - ny * half, x + nx * half, y + ny * half, startPaint)
            val arrow = dp(7f)
            canvas.drawLine(
                x + tx * arrow, y + ty * arrow,
                x - tx * arrow * 0.4f + nx * arrow * 0.7f,
                y - ty * arrow * 0.4f + ny * arrow * 0.7f,
                startPaint,
            )
            canvas.drawLine(
                x + tx * arrow, y + ty * arrow,
                x - tx * arrow * 0.4f - nx * arrow * 0.7f,
                y - ty * arrow * 0.4f - ny * arrow * 0.7f,
                startPaint,
            )
        }
        marker?.let { m ->
            val p = map(m.point)
            markerPaint.color = m.color
            canvas.drawCircle(p.x, p.y, dp(5f), markerPaint)
            if (m.label.isNotEmpty()) {
                canvas.drawText(m.label, p.x + dp(8f), p.y - dp(6f), markerTextPaint)
            }
        }
    }

    private fun dp(v: Float): Float = v * resources.displayMetrics.density
}
