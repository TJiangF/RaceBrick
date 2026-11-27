package com.example.navbridge

import android.content.pm.ActivityInfo
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.PointF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import java.util.Locale

class LapAnalysisActivity : LocalizedActivity() {
    private val palette = intArrayOf(
        0xFFFF3B30.toInt(), 0xFF2979FF.toInt(), 0xFFFFD60A.toInt(),
        0xFF34C759.toInt(), 0xFFFF2D95.toInt(), 0xFF32D4FF.toInt(),
        0xFFFF9F0A.toInt(), 0xFFAF52DE.toInt(),
    )

    private lateinit var chart: LapSpeedChartView
    private lateinit var map: TrackMapView
    private lateinit var cursorInfo: TextView
    private lateinit var legend: TextView
    private lateinit var lapRow: LinearLayout
    private lateinit var table: LinearLayout
    private lateinit var btnDistance: Button
    private lateinit var btnTime: Button
    private lateinit var payload: LapAnalysisStore.Payload

    private var axis = LapSpeedChartView.AXIS_DISTANCE
    private val selected = LinkedHashSet<Int>()
    private var fastest: Int? = null
    private val colors = HashMap<Int, Int>()
    private val checkViews = LinkedHashMap<Int, CheckBox>()
    private var seriesByLap: Map<Int, LapSpeedChartView.Series> = emptyMap()
    private var cursor: LapSpeedChartView.Cursor? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val data = LapAnalysisStore.payload
        if (data == null) {
            finish()
            return
        }
        payload = data
        setContentView(R.layout.activity_lap_analysis)
        chart = findViewById(R.id.speedChart)
        map = findViewById(R.id.analysisMap)
        cursorInfo = findViewById(R.id.tvAnalysisCursor)
        legend = findViewById(R.id.tvAnalysisLegend)
        lapRow = findViewById(R.id.llAnalysisLaps)
        table = findViewById(R.id.llAnalysisTable)
        btnDistance = findViewById(R.id.btnAxisDistance)
        btnTime = findViewById(R.id.btnAxisTime)
        findViewById<Button>(R.id.btnAnalysisBack).setOnClickListener { finish() }
        findViewById<TextView>(R.id.tvAnalysisTitle).text = Lang.tr("速度分析")
        findViewById<TextView>(R.id.tvAnalysisMeta).text =
            "${payload.session.trackName.ifBlank { "RaceBrick" }}  ·  ${payload.session.laps} 圈"

        payload.track?.let {
            map.startF = it.startF.toFloat()
            map.widthM = it.startWidthM.toFloat()
            map.trackLengthM = it.lengthM.toFloat()
            map.startOffsetM = it.startOffsetM.toFloat()
            map.reverse = it.reverse
        }
        payload.satellite?.let {
            map.setSatellite(TrackMapView.Satellite(it.bitmap, it.bounds))
        }

        val usable = payload.traces.filterValues { it.speeds.size >= 2 && it.times.size >= 2 }
        if (usable.isEmpty()) {
            Toast.makeText(this, Lang.tr("没有可用的速度数据"), Toast.LENGTH_SHORT).show()
            finish()
            return
        }
        val lapTime: (Int) -> Double = { lap ->
            payload.laps.firstOrNull { it.lap == lap && it.time > 0.0 }?.time
                ?: payload.session.lapTimes.getOrNull(lap - 1)
                ?: 0.0
        }
        fastest = usable.keys
            .filter { lapTime(it) > 0.0 }
            .minByOrNull { lapTime(it) }
            ?: payload.laps.filter { !it.deleted && it.time > 0.0 }.minByOrNull { it.time }?.lap
            ?: usable.keys.minOrNull()

        axis = savedInstanceState?.getInt("axis", LapSpeedChartView.AXIS_DISTANCE)
            ?: LapSpeedChartView.AXIS_DISTANCE
        val saved = savedInstanceState?.getIntArray("selected")
        if (saved != null) {
            selected.addAll(saved.filter { usable.containsKey(it) })
        }
        if (selected.isEmpty()) {
            fastest?.let { selected.add(it) }
        }

        for (lap in usable.keys.sorted()) {
            lapRow.addView(CheckBox(this).apply {
                text = "L$lap"
                textSize = 12f
                isChecked = selected.contains(lap)
                setOnCheckedChangeListener { _, checked ->
                    if (checked) selected.add(lap) else selected.remove(lap)
                    cursor = null
                    refresh()
                }
                checkViews[lap] = this
            })
        }
        btnDistance.setOnClickListener {
            axis = LapSpeedChartView.AXIS_DISTANCE
            cursor = null
            refresh()
        }
        btnTime.setOnClickListener {
            axis = LapSpeedChartView.AXIS_TIME
            cursor = null
            refresh()
        }
        btnRotate = findViewById(R.id.btnAnalysisRotate)
        btnRotate.text = if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            Lang.tr("竖屏")
        } else {
            Lang.tr("横屏")
        }
        btnRotate.setOnClickListener {
            requestedOrientation =
                if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
                    ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                } else {
                    ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                }
        }
        chart.onCursor = { c ->
            cursor = c
            updateCursor(c)
            updateMarkers(c)
        }
        refresh()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt("axis", axis)
        outState.putIntArray("selected", selected.toIntArray())
    }

    override fun onDestroy() {
        if (isFinishing) LapAnalysisStore.payload = null
        super.onDestroy()
    }

    private fun colorsFor(sel: Set<Int>): Map<Int, Int> {
        val order = sel.sorted()
        val pal = when (order.size) {
            2 -> intArrayOf(0xFFFF3B30.toInt(), 0xFF2979FF.toInt())
            3 -> intArrayOf(0xFFFF3B30.toInt(), 0xFFFFD60A.toInt(), 0xFF2979FF.toInt())
            else -> palette
        }
        return order.mapIndexed { i, lap -> lap to pal[i % pal.size] }.toMap()
    }

    private fun textOn(color: Int): Int {
        val r = android.graphics.Color.red(color) / 255.0
        val g = android.graphics.Color.green(color) / 255.0
        val b = android.graphics.Color.blue(color) / 255.0
        return if (0.299 * r + 0.587 * g + 0.114 * b > 0.62) 0xFF101010.toInt()
        else 0xFFFFFFFF.toInt()
    }

    private fun refresh() {
        val cmap = colorsFor(selected)
        colors.clear()
        colors.putAll(cmap)
        val showChart = selected.size in 1..2
        val series = if (showChart) selected.sorted().mapNotNull { lap ->
            val data = payload.traces[lap] ?: return@mapNotNull null
            val detail = payload.laps.firstOrNull { it.lap == lap }
            buildSeries(lap, cmap[lap] ?: palette[0], data, detail)
        } else emptyList()
        seriesByLap = series.associateBy { it.lap }
        chart.visibility = if (showChart) View.VISIBLE else View.GONE
        val mapParams = map.layoutParams as? LinearLayout.LayoutParams
        if (mapParams != null && mapParams.height == 0) {
            mapParams.weight = if (showChart) 0.42f else 1f
            map.layoutParams = mapParams
        }
        table.visibility = View.VISIBLE
        btnDistance.visibility = if (showChart) View.VISIBLE else View.GONE
        btnTime.visibility = if (showChart) View.VISIBLE else View.GONE
        cursorInfo.visibility = if (showChart) View.VISIBLE else View.GONE
        legend.visibility = if (showChart) View.VISIBLE else View.GONE
        chart.setSeries(series, axis)
        btnDistance.setTextColor(if (axis == LapSpeedChartView.AXIS_DISTANCE) activeColor() else idleColor())
        btnTime.setTextColor(if (axis == LapSpeedChartView.AXIS_TIME) activeColor() else idleColor())
        for ((lap, cb) in checkViews) {
            val on = selected.contains(lap)
            val c = colors[lap] ?: 0xFF62727A.toInt()
            val fill = if (on) (c and 0x00FFFFFF) or 0x66000000 else 0x332F3A40
            cb.background = GradientDrawable().apply {
                cornerRadius = dp(5).toFloat()
                setColor(fill)
                if (lap == fastest) setStroke(dp(2), 0xFF8B5CF6.toInt())
            }
            cb.setTextColor(if (on) textOn(c) else 0xFF8A9A9A.toInt())
            cb.setTypeface(Typeface.DEFAULT, if (lap == fastest) Typeface.BOLD else Typeface.NORMAL)
            cb.buttonTintList = ColorStateList.valueOf(if (on) textOn(c) else 0xFF8A9A9A.toInt())
            cb.setPadding(dp(8), dp(3), dp(8), dp(3))
        }
        val layers = selected.sorted().mapNotNull { lap ->
            val data = payload.traces[lap] ?: return@mapNotNull null
            TrackMapView.Layer(
                data.points,
                colors[lap] ?: palette[0],
                "L$lap",
            )
        }
        map.setLayers(layers)
        map.setMarkers(emptyList())
        buildTable()
        if (showChart) {
            val sb = SpannableStringBuilder()
            for (item in series) {
                val color = colors[item.lap] ?: palette[0]
                val start = sb.length
                sb.append("L${item.lap}")
                sb.setSpan(ForegroundColorSpan(color), start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                if (item.lap == fastest) {
                    sb.setSpan(StyleSpan(Typeface.BOLD), start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                val t = payload.laps.firstOrNull { it.lap == item.lap }?.time
                    ?: payload.session.lapTimes.getOrNull(item.lap - 1) ?: 0.0
                val timeText = if (t > 0.0) formatLap(t) else "—"
                sb.append("  $timeText  MAX %.0f km/h\n".format(item.maxSpeed))
            }
            legend.text = sb
        }
        if (cursor != null && showChart) {
            updateMarkers(cursor)
        }
    }

    private fun buildTable() {
        table.removeAllViews()
        table.addView(TextView(this).apply {
            text = "圈    用时          最高速度"
            setTextColor(0xFF8FA6AE.toInt())
            textSize = 12f
            setPadding(0, dp(2), 0, dp(2))
        })
        for (lap in payload.traces.keys.sorted()) {
            val data = payload.traces[lap]
            val t = payload.laps.firstOrNull { it.lap == lap }?.time
                ?: payload.session.lapTimes.getOrNull(lap - 1) ?: 0.0
            val max = data?.speeds?.maxOrNull() ?: 0.0
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(4), 0, dp(4))
            }
            row.addView(View(this).apply {
                setBackgroundColor(colors[lap] ?: palette[0])
                layoutParams = LinearLayout.LayoutParams(dp(4), dp(20))
            })
            row.addView(TextView(this).apply {
                text = " L$lap   ${if (t > 0.0) formatLap(t) else "—"}   MAX %.0f km/h".format(max)
                setTextColor(if (lap == fastest) 0xFFFFD700.toInt() else 0xFFE8F3F0.toInt())
                textSize = 12f
                setTypeface(typeface, if (lap == fastest) Typeface.BOLD else Typeface.NORMAL)
                setPadding(dp(6), 0, 0, 0)
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            table.addView(row)
        }
    }

    private fun updateCursor(c: LapSpeedChartView.Cursor?) {
        if (c == null) {
            cursorInfo.text = Lang.tr("拖动图表查看各圈速度")
            return
        }
        val sb = SpannableStringBuilder()
        sb.append(chart.formatAxis(c.x))
        for ((lap, speed) in c.values) {
            sb.append("   ")
            val start = sb.length
            sb.append("L$lap %.0f km/h".format(speed))
            sb.setSpan(
                ForegroundColorSpan(colors[lap] ?: palette[0]),
                start, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
            )
        }
        cursorInfo.text = sb
    }

    private fun updateMarkers(c: LapSpeedChartView.Cursor?) {
        if (c == null || selected.size > 2) {
            map.setMarkers(emptyList())
            return
        }
        val markers = ArrayList<TrackMapView.Marker>()
        for ((lap, _) in c.values) {
            val series = seriesByLap[lap] ?: continue
            val data = payload.traces[lap] ?: continue
            val point = pointAt(data, series, c.x) ?: continue
            markers += TrackMapView.Marker(point, colors[lap] ?: palette[0], "L$lap")
        }
        map.setMarkers(markers)
    }

    private fun pointAt(data: TraceData, series: LapSpeedChartView.Series, x: Float): PointF? {
        val n = minOf(data.points.size, series.x.size)
        if (n < 2) return null
        if (x <= series.x[0]) return data.points[0]
        if (x >= series.x[n - 1]) return data.points[n - 1]
        var lo = 0
        var hi = n - 1
        while (lo + 1 < hi) {
            val mid = (lo + hi) / 2
            if (series.x[mid] <= x) lo = mid else hi = mid
        }
        val span = series.x[hi] - series.x[lo]
        val k = if (span > 0f) (x - series.x[lo]) / span else 0f
        return PointF(
            data.points[lo].x + (data.points[hi].x - data.points[lo].x) * k,
            data.points[lo].y + (data.points[hi].y - data.points[lo].y) * k,
        )
    }

    private lateinit var btnRotate: Button

    private fun buildSeries(
        lap: Int,
        color: Int,
        data: TraceData,
        detail: LapDetail?,
    ): LapSpeedChartView.Series? {
        val n = minOf(data.speeds.size, data.times.size)
        if (n < 2) return null
        val x = FloatArray(n)
        if (axis == LapSpeedChartView.AXIS_TIME) {
            val t0 = data.times[0]
            for (i in 0 until n) x[i] = (data.times[i] - t0).coerceAtLeast(0L) / 1000f
            if (x[n - 1] <= 0f) {
                val t = detail?.time ?: 0.0
                for (i in 0 until n) x[i] = (t * i / (n - 1)).toFloat()
            }
        } else {
            var dist = 0.0
            for (i in 1 until n) {
                val dt = (data.times[i] - data.times[i - 1]).coerceAtLeast(0L) / 1000.0
                if (dt <= 0.0 || dt > 10.0) continue
                dist += (data.speeds[i] + data.speeds[i - 1]) * 0.5 / 3.6 * dt
                x[i] = dist.toFloat()
            }
            if (dist <= 0.0 && data.points.size >= 2) {
                val m = minOf(data.points.size, n)
                var d = 0.0
                for (i in 1 until m) {
                    val dx = data.points[i].x - data.points[i - 1].x
                    val dy = data.points[i].y - data.points[i - 1].y
                    d += kotlin.math.hypot(dx.toDouble(), dy.toDouble())
                    x[i] = d.toFloat()
                }
                for (i in m until n) x[i] = x[m - 1]
            }
            if (x[n - 1] <= 0f) return null
        }
        var maxIndex = 0
        var maxSpeed = data.speeds[0]
        for (i in 1 until n) {
            if (data.speeds[i] > maxSpeed) {
                maxSpeed = data.speeds[i]
                maxIndex = i
            }
        }
        if (maxSpeed <= 0.0) return null
        return LapSpeedChartView.Series(
            lap, color, x,
            FloatArray(n) { data.speeds[it].toFloat() },
            maxIndex, maxSpeed.toFloat(),
        )
    }

    private fun activeColor() = 0xFF66E0D2.toInt()
    private fun idleColor() = 0xFF8A9A9A.toInt()
    private fun formatLap(seconds: Double): String {
        val total = (seconds * 1000.0).toLong().coerceAtLeast(0L)
        val m = total / 60000
        val s = (total % 60000) / 1000
        val ms = total % 1000
        return "%d:%02d.%03d".format(Locale.US, m, s, ms)
    }
    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
