package com.example.navbridge

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.ceil
import kotlin.math.max

class LapSpeedChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {
    companion object {
        const val AXIS_DISTANCE = 0
        const val AXIS_TIME = 1
    }

    data class Series(
        val lap: Int,
        val color: Int,
        val x: FloatArray,
        val speed: FloatArray,
        val maxIndex: Int,
        val maxSpeed: Float,
    )

    data class Cursor(
        val x: Float,
        val values: List<Pair<Int, Float>>,
    )

    var axis: Int = AXIS_DISTANCE
        private set
    var onCursor: ((Cursor?) -> Unit)? = null
    private var series: List<Series> = emptyList()
    private var cursorX: Float? = null
    private val plot = RectF()

    private val density = resources.displayMetrics.density
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF243038.toInt()
        strokeWidth = 1f
        style = Paint.Style.STROKE
    }
    private val axisPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF60717A.toInt()
        strokeWidth = 1f
        style = Paint.Style.STROKE
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF8FA6AE.toInt()
        textSize = 10f * density
    }
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val cursorPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFE8F3F0.toInt()
        strokeWidth = 1f * density
        style = Paint.Style.STROKE
    }
    private val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val path = Path()

    init {
        isClickable = true
        isFocusable = true
    }

    fun setSeries(value: List<Series>, axis: Int) {
        series = value
        this.axis = axis
        cursorX = null
        invalidate()
        onCursor?.invoke(null)
    }

    fun clearCursor() {
        cursorX = null
        invalidate()
        onCursor?.invoke(null)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(0xFF0B1116.toInt())
        val left = 44f * density
        val right = width - 12f * density
        val top = 14f * density
        val bottom = height - 30f * density
        plot.set(left, top, right, bottom)
        if (series.isEmpty() || right <= left || bottom <= top) {
            textPaint.textAlign = Paint.Align.CENTER
            canvas.drawText("NO SPEED DATA", width / 2f, height / 2f, textPaint)
            return
        }

        val maxX = series.maxOf { it.x.lastOrNull()?.toDouble() ?: 0.0 }.toFloat().coerceAtLeast(1f)
        val maxSpeed = series.maxOf { it.maxSpeed }.coerceAtLeast(10f)
        val maxY = (ceil(maxSpeed / 20f) * 20f).coerceAtLeast(60f)

        val rows = 4
        for (i in 0..rows) {
            val y = plot.bottom - plot.height() * i / rows
            canvas.drawLine(plot.left, y, plot.right, y, gridPaint)
            val v = maxY * i / rows
            textPaint.textAlign = Paint.Align.RIGHT
            canvas.drawText("%.0f".format(v), plot.left - 6f * density, y + 3f * density, textPaint)
        }
        val cols = 4
        for (i in 0..cols) {
            val x = plot.left + plot.width() * i / cols
            canvas.drawLine(x, plot.top, x, plot.bottom, gridPaint)
            textPaint.textAlign = Paint.Align.CENTER
            val v = maxX * i / cols
            canvas.drawText(formatAxis(v), x, plot.bottom + 15f * density, textPaint)
        }
        canvas.drawRect(plot, axisPaint)

        for (s in series) {
            path.reset()
            var started = false
            for (i in s.x.indices) {
                if (i >= s.speed.size) break
                val px = xToPixel(s.x[i], maxX)
                val py = speedToPixel(s.speed[i], maxY)
                if (!started) {
                    path.moveTo(px, py)
                    started = true
                } else {
                    path.lineTo(px, py)
                }
            }
            if (started) {
                linePaint.color = s.color
                canvas.drawPath(path, linePaint)
            }
            if (s.maxIndex in s.x.indices && s.maxIndex in s.speed.indices) {
                dotPaint.color = s.color
                canvas.drawCircle(
                    xToPixel(s.x[s.maxIndex], maxX),
                    speedToPixel(s.speed[s.maxIndex], maxY),
                    3f * density,
                    dotPaint,
                )
            }
        }

        cursorX?.let { cx ->
            val px = xToPixel(cx, maxX)
            canvas.drawLine(px, plot.top, px, plot.bottom, cursorPaint)
            for (s in series) {
                val value = valueAt(s, cx) ?: continue
                dotPaint.color = s.color
                canvas.drawCircle(px, speedToPixel(value, maxY), 3.5f * density, dotPaint)
            }
        }
    }

    private fun xToPixel(value: Float, maxX: Float): Float =
        plot.left + (value / maxX).coerceIn(0f, 1f) * plot.width()

    private fun speedToPixel(value: Float, maxY: Float): Float =
        plot.bottom - (value / maxY).coerceIn(0f, 1f) * plot.height()

    private fun valueAt(s: Series, x: Float): Float? {
        if (s.x.isEmpty() || s.speed.isEmpty()) return null
        if (x <= s.x.first()) return s.speed.first()
        if (x >= s.x.last()) return s.speed.last()
        var lo = 0
        var hi = s.x.size - 1
        while (lo + 1 < hi) {
            val mid = (lo + hi) / 2
            if (s.x[mid] <= x) lo = mid else hi = mid
        }
        val span = s.x[hi] - s.x[lo]
        if (span <= 0f) return s.speed[lo]
        val k = (x - s.x[lo]) / span
        val speedIndexLo = lo.coerceAtMost(s.speed.size - 1)
        val speedIndexHi = hi.coerceAtMost(s.speed.size - 1)
        return s.speed[speedIndexLo] + (s.speed[speedIndexHi] - s.speed[speedIndexLo]) * k
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (series.isEmpty()) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                val x = ((event.x - plot.left) / plot.width()).coerceIn(0f, 1f)
                val maxX = series.maxOf { it.x.lastOrNull()?.toDouble() ?: 0.0 }.toFloat().coerceAtLeast(1f)
                cursorX = x * maxX
                val values = series.mapNotNull { s ->
                    valueAt(s, cursorX!!)?.let { s.lap to it }
                }
                onCursor?.invoke(Cursor(cursorX!!, values))
                invalidate()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    fun formatAxis(value: Float): String =
        if (axis == AXIS_TIME) {
            val total = value.toInt().coerceAtLeast(0)
            val m = total / 60
            val s = total % 60
            "%d:%02d".format(m, s)
        } else {
            if (value >= 1000f) "%.1fkm".format(value / 1000f)
            else "%.0fm".format(value)
        }
}
