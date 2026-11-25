package com.example.navbridge

import android.graphics.Color
import android.graphics.PointF
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.GZIPInputStream
import kotlin.math.cos
import kotlin.math.max

class LocalRecordsActivity : AppCompatActivity() {
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val db by lazy { SyncLocalDb(this) }
    private lateinit var list: LinearLayout
    private lateinit var status: TextView
    private var tab = "track"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF080C11.toInt())
            setPadding(dp(14), dp(24), dp(14), dp(14))
        }
        val head = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        head.addView(TextView(this).apply {
            text = "LOCAL DATA"
            setTextColor(0xFF66E0D2.toInt())
            textSize = 21f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        head.addView(Button(this).apply { text = "BACK"; setOnClickListener { finish() } })
        root.addView(head)
        status = TextView(this).apply {
            text = "本地 SQLite"
            setTextColor(0xFF8FA2B5.toInt())
            textSize = 12f
            setPadding(0, dp(8), 0, dp(8))
        }
        root.addView(status)
        val tabs = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun tabButton(label: String, key: String) {
            tabs.addView(Button(this).apply {
                text = label
                textSize = 12f
                setOnClickListener { tab = key; render() }
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            })
        }
        tabButton("TRACKS", "track")
        tabButton("RACE LOGS", "session")
        tabButton("OLD LOGS", "race_log")
        root.addView(tabs)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(list) }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
        ))
        setContentView(root)
        render()
    }

    private fun render() {
        list.removeAllViews()
        worker.execute {
            val entities = db.entities(tab)
            main.post {
                status.text = "${entities.size} local records · ${if (tab == "track") "Track" else if (tab == "session") "Race Log" else "Legacy Log"}"
                if (entities.isEmpty()) {
                    list.addView(text("No local records. Use Load RaceBrick -> Local App first.", 0xFF8FA2B5.toInt()))
                    return@post
                }
                for (e in entities) {
                    val card = LinearLayout(this).apply {
                        orientation = LinearLayout.VERTICAL
                        setBackgroundColor(0xFF101923.toInt())
                        setPadding(dp(10), dp(10), dp(10), dp(10))
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                        ).apply { bottomMargin = dp(10) }
                    }
                    card.addView(text(e.name.ifBlank { if (tab == "track") "Track ${e.localKey}" else "Record ${e.localKey}" }, 0xFFF0F6F4.toInt(), 16f))
                    val map = when (tab) {
                        "track" -> trackMap(e.payload)
                        "session" -> sessionMap(e)
                        else -> logMap(e.payload)
                    }
                    if (map != null) card.addView(map)
                    else card.addView(text("No trace data", 0xFFE0A040.toInt()))
                    if (tab == "session") {
                        val times = e.payload.optJSONArray("lap_times") ?: JSONArray()
                        card.addView(text("${e.payload.optInt("laps")} laps · ${fmtDate(e.payload.optLong("start"))} · ${times.length()} lap times", 0xFFA9C4BE.toInt(), 12f))
                    }
                    list.addView(card)
                }
            }
        }
    }

    private fun trackMap(p: JSONObject): TrackMapView? {
        val pts = pairs(p.optJSONArray("points") ?: return null)
        if (pts.size < 2) return null
        return TrackMapView(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(150)).apply { topMargin = dp(6) }
            setLayers(listOf(TrackMapView.Layer(pts, 0xFF66E0D2.toInt())))
            startF = p.optDouble("start_f", 0.0).toFloat()
            widthM = p.optDouble("start_width", p.optDouble("width", 5.0)).toFloat()
            trackLengthM = p.optDouble("length_m", 0.0).toFloat()
            startOffsetM = p.optDouble("start_offset", 0.0).toFloat()
            reverse = p.optBoolean("reverse", false)
        }
    }

    private fun logMap(p: JSONObject): TrackMapView? {
        val pts = pairs(p.optJSONArray("trace") ?: return null)
        if (pts.size < 2) return null
        return TrackMapView(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(150)).apply { topMargin = dp(6) }
            setLayers(listOf(TrackMapView.Layer(pts, 0xFFFFD700.toInt())))
        }
    }

    private fun sessionMap(e: SyncLocalDb.LocalEntity): TrackMapView? {
        val file = e.blobPath?.let { File(it) } ?: return null
        if (!file.isFile) return null
        val track = db.trackByLocalKey(e.payload.optInt("track", -1))
        val trackP = track?.payload
        val csv = runCatching {
            GZIPInputStream(file.inputStream()).bufferedReader().use { it.readText() }
        }.getOrNull() ?: return null
        val lapTimes = e.payload.optJSONArray("lap_times") ?: JSONArray()
        val groups = LinkedHashMap<Int, MutableList<PointF>>()
        var lat0 = Double.NaN
        var lon0 = 0.0
        var lastElapsedLap1 = 0.0
        val rows = ArrayList<Array<String>>()
        for (line in csv.lineSequence()) {
            val p = line.split(',')
            if (p.size < 7) continue
            val lap = p[2].trim().toIntOrNull() ?: continue
            if (lap <= 0) continue
            rows += p.toTypedArray()
            if (lap == 1) p[3].trim().toDoubleOrNull()?.let { lastElapsedLap1 = max(lastElapsedLap1, it) }
        }
        val cut = if (lapTimes.length() > 0) max(0.0, lastElapsedLap1 - lapTimes.optDouble(0, 0.0)) else 0.0
        for (p in rows) {
            val lap = p[2].trim().toIntOrNull() ?: continue
            val elapsed = p[3].trim().toDoubleOrNull() ?: continue
            if (lap == 1 && cut > 0 && elapsed < cut) continue
            val lat = p[4].trim().toDoubleOrNull() ?: continue
            val lon = p[5].trim().toDoubleOrNull() ?: continue
            if (lat == 0.0 && lon == 0.0) continue
            if (lat0.isNaN()) { lat0 = lat; lon0 = lon }
            val q = geoNorm(trackP, lat, lon, lat0, lon0)
            groups.getOrPut(lap) { ArrayList() } += q
        }
        if (groups.isEmpty()) return null
        val layers = ArrayList<TrackMapView.Layer>()
        val colors = intArrayOf(0xFF66E0D2.toInt(), 0xFFFFB74D.toInt(), 0xFFa78bfa.toInt(), 0xFF5ED6A3.toInt(), 0xFFFF6B9A.toInt(), 0xFF42A5F5.toInt())
        for ((i, entry) in groups.entries.withIndex()) {
            val pts = entry.value
            if (pts.size < 2) continue
            val step = max(1, pts.size / 96)
            val reduced = ArrayList<PointF>()
            var k = 0
            while (k < pts.size) { reduced += pts[k]; k += step }
            if (reduced.last() !== pts.last()) reduced += pts.last()
            layers += TrackMapView.Layer(reduced, colors[i % colors.size], "L${entry.key}")
        }
        if (layers.isEmpty()) return null
        return TrackMapView(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(170)).apply { topMargin = dp(6) }
            setLayers(layers)
            if (trackP != null) {
                startF = trackP.optDouble("start_f", 0.0).toFloat()
                widthM = trackP.optDouble("start_width", trackP.optDouble("width", 5.0)).toFloat()
                trackLengthM = trackP.optDouble("length_m", 0.0).toFloat()
                startOffsetM = trackP.optDouble("start_offset", 0.0).toFloat()
                reverse = trackP.optBoolean("reverse", false)
            }
        }
    }

    private fun geoNorm(track: JSONObject?, lat: Double, lon: Double, lat0: Double, lon0: Double): PointF {
        if (track != null && track.optBoolean("geo", false)) {
            val ns = track.optDouble("nscale", 0.0)
            if (ns > 1e-9) {
                val tl0 = track.optDouble("glat0", 0.0)
                val to0 = track.optDouble("glon0", 0.0)
                val mx = (lon - to0) * 111320.0 * cos(Math.toRadians((lat + tl0) / 2.0))
                val my = (lat - tl0) * 111320.0
                return PointF((mx * ns + track.optDouble("nox", 0.0)).toFloat(),
                    (my * ns + track.optDouble("noy", 0.0)).toFloat())
            }
        }
        val x = (lon - lon0) * 111320.0 * cos(Math.toRadians(lat0))
        val y = (lat - lat0) * 111320.0
        return PointF(x.toFloat(), y.toFloat())
    }

    private fun pairs(arr: JSONArray?): List<PointF> {
        if (arr == null) return emptyList()
        val out = ArrayList<PointF>()
        var i = 0
        if (arr.length() > 0 && arr.opt(0) is JSONArray) {
            for (j in 0 until arr.length()) {
                val p = arr.optJSONArray(j) ?: continue
                if (p.length() >= 2) out += PointF(p.optDouble(0).toFloat(), p.optDouble(1).toFloat())
            }
        } else {
            while (i + 1 < arr.length()) {
                out += PointF(arr.optDouble(i).toFloat(), arr.optDouble(i + 1).toFloat()); i += 2
            }
        }
        return out
    }

    private fun text(s: String, color: Int, size: Float = 13f) = TextView(this).apply {
        text = s; setTextColor(color); textSize = size; setPadding(0, dp(3), 0, 0)
    }
    private fun fmtDate(ts: Long): String = if (ts <= 0) "time unknown" else SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(ts * 1000L))
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    override fun onDestroy() { worker.shutdownNow(); super.onDestroy() }
}
