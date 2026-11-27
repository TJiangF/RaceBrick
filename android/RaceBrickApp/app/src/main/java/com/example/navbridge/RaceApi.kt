package com.example.navbridge

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URLEncoder
import java.net.URL
import java.nio.charset.StandardCharsets
import java.util.Locale

data class RaceTrack(
    val id: Int,
    val name: String,
    val geo: Boolean,
    val lengthM: Double,
    val sections: Int,
    val widthM: Double = 5.0,
    val startWidthM: Double = 5.0,
    val startF: Double = 0.0,
    val startOffsetM: Double = 0.0,
    val reverse: Boolean = false,
    val uuid: String = "",
    val revision: Int = 0,
    val revisionId: String = "",
)

data class SessionChunk(
    val bytes: ByteArray,
    val sha256: String,
)

data class TrackUpdate(
    val id: Int,
    val uuid: String,
    val revision: Int,
    val revisionId: String,
)

class TrackConflictException(
    val currentVersion: Int,
    val currentRevisionId: String,
) : IllegalStateException("设备 Track 已被更新")

data class TrackGeoRef(
    val lat0: Double,
    val lon0: Double,
    val nscale: Double,
    val nox: Double,
    val noy: Double,
)

data class TrackDetail(
    val name: String,
    val lengthM: Double,
    val widthM: Double,
    val startWidthM: Double,
    val startF: Double,
    val startOffsetM: Double,
    val reverse: Boolean,
    val points: List<TrackMapView.Layer>,
    val geoRef: TrackGeoRef? = null,
    val sections: List<Double> = emptyList(),
)

data class RaceLogEntry(
    val index: Int,
    val trackName: String,
    val date: String,
    val mode: Int,
    val durationS: Double,
    val laps: Int,
    val bestS: Double,
    val bestIndex: Int,
    val hasBest: Boolean,
    val vehicle: Int,
    val lapTimes: List<Double>,
)

data class RaceLogDetail(
    val entry: RaceLogEntry,
    val trace: List<android.graphics.PointF>,
)

data class LapDetail(
    val lap: Int,
    val time: Double,
    val samples: Long,
    val distanceM: Double,
    val avgKmh: Double,
    val maxKmh: Double,
    val deleted: Boolean,
)

data class TraceData(
    val points: List<android.graphics.PointF>,
    val speeds: List<Double> = emptyList(),
    val times: List<Long> = emptyList(),
)

data class RaceSession(
    val slot: Int,
    val id: Long,
    val trackSlot: Int,
    val trackName: String,
    val startEpoch: Long,
    val durationMs: Long,
    val samples: Long,
    val laps: Int,
    val deletedLapMask: Int = 0,
    val lapTimes: List<Double> = emptyList(),
    val uuid: String = "",
    val trackUuid: String = "",
    val trackRevisionId: String = "",
    val trackVersion: Int = 0,
    val endEpoch: Long = 0L,
    val sampleSize: Int = 16,
    val lapIds: List<String> = emptyList(),
    val contentHash: String = "",
)

object RaceApi {
    const val PREFS = "racebridge"
    const val KEY_BASE_URL = "race_base_url"
    const val DEFAULT_BASE_URL = "http://192.168.4.1"

    fun baseUrl(context: Context): String {
        val saved = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY_BASE_URL, DEFAULT_BASE_URL)
            .orEmpty()
        return normalizeBase(saved.ifBlank { DEFAULT_BASE_URL })
    }

    fun saveBaseUrl(context: Context, value: String): String {
        val base = normalizeBase(value)
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_BASE_URL, base)
            .apply()
        return base
    }

    fun normalizeBase(value: String): String {
        var v = value.trim()
        if (v.isEmpty()) return DEFAULT_BASE_URL
        if (!v.startsWith("http://") && !v.startsWith("https://")) v = "http://$v"
        return v.trimEnd('/')
    }

    fun tracks(base: String): List<RaceTrack> {
        val root = JSONObject(get(base, "/api/tracks").toString(StandardCharsets.UTF_8))
        val arr = root.optJSONArray("tracks") ?: JSONArray()
        val out = ArrayList<RaceTrack>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            out += RaceTrack(
                id = o.optInt("id"),
                name = o.optString("name", "未命名赛道"),
                geo = o.optInt("geo", 0) != 0,
                lengthM = o.optDouble("m", 0.0),
                sections = o.optInt("sections", 0),
                widthM = o.optDouble("width", 5.0),
                startWidthM = o.optDouble("start_width", o.optDouble("width", 5.0)),
                startF = o.optDouble("start_f", 0.0),
                startOffsetM = o.optDouble("offset", 0.0),
                reverse = o.optInt("reverse", 0) != 0,
                uuid = o.optString("uuid", ""),
                revision = o.optInt("version", 0),
                revisionId = o.optString("revision_id", ""),
            )
        }
        return out
    }

    fun track(base: String, id: Int): TrackDetail {
        val o = JSONObject(get(base, "/api/track?id=$id").toString(StandardCharsets.UTF_8))
        val pts = ArrayList<android.graphics.PointF>()
        val arr = o.optJSONArray("points")
        if (arr != null) {
            var i = 0
            while (i + 1 < arr.length()) {
                pts += android.graphics.PointF(arr.optDouble(i).toFloat(), arr.optDouble(i + 1).toFloat())
                i += 2
            }
        }
        val nscale = o.optDouble("nscale", 0.0)
        val geoRef = if (o.optInt("geo", 0) != 0 && nscale > 1e-9) TrackGeoRef(
            lat0 = o.optDouble("glat0", 0.0),
            lon0 = o.optDouble("glon0", 0.0),
            nscale = nscale,
            nox = o.optDouble("nox", 0.0),
            noy = o.optDouble("noy", 0.0),
        ) else null
        val sections = ArrayList<Double>()
        val sa = o.optJSONArray("sections")
        if (sa != null) for (j in 0 until sa.length()) sections += sa.optDouble(j, 0.0)
        return TrackDetail(
            name = o.optString("name", ""),
            lengthM = o.optDouble("length_m", 0.0),
            widthM = o.optDouble("width", 5.0),
            startWidthM = o.optDouble("start_width", o.optDouble("width", 5.0)),
            startF = o.optDouble("start_f", 0.0),
            startOffsetM = o.optDouble("start_offset", 0.0),
            reverse = o.optInt("reverse", 0) != 0,
            points = listOf(TrackMapView.Layer(pts, 0xFF66E0D2.toInt())),
            geoRef = geoRef,
            sections = sections,
        )
    }

    fun renameLogs(base: String, from: String, to: String) {
        val a = URLEncoder.encode(from, StandardCharsets.UTF_8.name())
        val b = URLEncoder.encode(to, StandardCharsets.UTF_8.name())
        post(base, "/api/log/rename?from=$a&to=$b")
    }

    fun logDetail(base: String, index: Int): RaceLogDetail {
        val o = JSONObject(get(base, "/api/log?id=$index").toString(StandardCharsets.UTF_8))
        val times = ArrayList<Double>()
        val ta = o.optJSONArray("lap_times")
        if (ta != null) for (j in 0 until ta.length()) times += ta.optDouble(j, 0.0)
        val entry = RaceLogEntry(
            index = o.optInt("index"),
            trackName = o.optString("track_name", ""),
            date = o.optString("date", ""),
            mode = o.optInt("mode", 0),
            durationS = o.optDouble("duration", 0.0),
            laps = o.optInt("laps", 0),
            bestS = o.optDouble("best", 0.0),
            bestIndex = o.optInt("best_index", 0),
            hasBest = o.optInt("has_best", 0) != 0,
            vehicle = o.optInt("vehicle", 0),
            lapTimes = times,
        )
        val trace = ArrayList<android.graphics.PointF>()
        val tr = o.optJSONArray("trace")
        if (tr != null) {
            var i = 0
            while (i + 1 < tr.length()) {
                trace += android.graphics.PointF(tr.optDouble(i).toFloat(), tr.optDouble(i + 1).toFloat())
                i += 2
            }
        }
        return RaceLogDetail(entry, trace)
    }

    fun logs(base: String): List<RaceLogEntry> {
        val root = JSONObject(get(base, "/api/logs").toString(StandardCharsets.UTF_8))
        val arr = root.optJSONArray("logs") ?: JSONArray()
        val out = ArrayList<RaceLogEntry>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            val times = ArrayList<Double>()
            val ta = o.optJSONArray("lap_times")
            if (ta != null) for (j in 0 until ta.length()) times += ta.optDouble(j, 0.0)
            out += RaceLogEntry(
                index = o.optInt("index"),
                trackName = o.optString("track_name", ""),
                date = o.optString("date", ""),
                mode = o.optInt("mode", 0),
                durationS = o.optDouble("duration", 0.0),
                laps = o.optInt("laps", 0),
                bestS = o.optDouble("best", 0.0),
                bestIndex = o.optInt("best_index", 0),
                hasBest = o.optInt("has_best", 0) != 0,
                vehicle = o.optInt("vehicle", 0),
                lapTimes = times,
            )
        }
        return out
    }

    fun sessions(base: String): List<RaceSession> {
        val root = JSONObject(get(base, "/api/sessions").toString(StandardCharsets.UTF_8))
        val arr = root.optJSONArray("sessions") ?: JSONArray()
        val out = ArrayList<RaceSession>(arr.length())
        for (i in 0 until arr.length()) out += parseSession(arr.getJSONObject(i))
        return out
    }

    fun session(base: String, slot: Int): RaceSession {
        val o = JSONObject(get(base, "/api/session?id=$slot").toString(StandardCharsets.UTF_8))
        return parseSession(o)
    }

    fun updateTrack(
        base: String, id: Int, name: String,
        startF: Double, reverse: Boolean, startWidthM: Double, offsetM: Double,
        baseRevisionId: String = "",
    ): TrackUpdate {
        val encoded = URLEncoder.encode(name, StandardCharsets.UTF_8.name())
        val sf = String.format(Locale.US, "%.5f", startF)
        val sw = String.format(Locale.US, "%.2f", startWidthM)
        val off = String.format(Locale.US, "%.2f", offsetM)
        val baseRev = if (baseRevisionId.isBlank()) "" else
            "&base_rev=" + URLEncoder.encode(baseRevisionId, StandardCharsets.UTF_8.name())
        val path = "/api/track?id=$id&name=$encoded&start_f=$sf&reverse=${if (reverse) 1 else 0}" +
            "&start_width=$sw&offset=$off$baseRev"
        val conn = open(base, path, "POST")
        val body: ByteArray
        val code: Int
        try {
            code = conn.responseCode
            body = drain(conn)
        } finally {
            conn.disconnect()
        }
        if (code == 409) {
            val o = runCatching { JSONObject(body.toString(StandardCharsets.UTF_8)) }.getOrDefault(JSONObject())
            throw TrackConflictException(
                currentVersion = o.optInt("current_version", 0),
                currentRevisionId = o.optString("current_revision_id", ""),
            )
        }
        if (code !in 200..299) throw IllegalStateException("HTTP $code")
        val o = JSONObject(body.toString(StandardCharsets.UTF_8))
        if (o.optInt("ok", 0) == 0) throw IllegalStateException("设备拒绝了保存")
        return TrackUpdate(
            id = o.optInt("id", -1),
            uuid = o.optString("uuid", ""),
            revision = o.optInt("version", 0),
            revisionId = o.optString("revision_id", ""),
        )
    }

    fun renameTrack(base: String, id: Int, name: String) {
        val encoded = URLEncoder.encode(name, StandardCharsets.UTF_8.name())
        post(base, "/api/track?id=$id&name=$encoded")
    }

    fun deleteTrack(base: String, id: Int) {
        delete(base, "/api/track?id=$id")
    }

    fun deleteSession(base: String, slot: Int) {
        delete(base, "/api/session?id=$slot")
    }

    fun deleteLap(base: String, slot: Int, lap: Int) {
        delete(base, "/api/session/lap?id=$slot&lap=$lap")
    }

    fun sessionLaps(base: String, slot: Int): List<LapDetail> {
        val root = JSONObject(get(base, "/api/session/laps?id=$slot").toString(StandardCharsets.UTF_8))
        val arr = root.optJSONArray("laps") ?: JSONArray()
        val out = ArrayList<LapDetail>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            out += LapDetail(
                lap = o.optInt("lap"),
                time = o.optDouble("time", 0.0),
                samples = o.optLong("samples", 0),
                distanceM = o.optDouble("distance_m", 0.0),
                avgKmh = o.optDouble("avg_kmh", 0.0),
                maxKmh = o.optDouble("max_kmh", 0.0),
                deleted = o.optInt("deleted", 0) != 0,
            )
        }
        return out
    }

    fun sessionTraces(base: String, slot: Int): Map<Int, TraceData> {
        val root = JSONObject(get(base, "/api/session/traces?id=$slot").toString(StandardCharsets.UTF_8))
        val arr = root.optJSONArray("laps") ?: JSONArray()
        val out = LinkedHashMap<Int, TraceData>()
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            val lap = o.optInt("lap")
            val pts = ArrayList<android.graphics.PointF>()
            val pa = o.optJSONArray("points")
            if (pa != null) {
                var j = 0
                while (j + 1 < pa.length()) {
                    pts += android.graphics.PointF(pa.optDouble(j).toFloat(), pa.optDouble(j + 1).toFloat())
                    j += 2
                }
            }
            val speeds = ArrayList<Double>()
            val sa = o.optJSONArray("speeds")
            if (sa != null) for (j in 0 until sa.length()) speeds += sa.optDouble(j, 0.0)
            val times = ArrayList<Long>()
            val ta = o.optJSONArray("times")
            if (ta != null) for (j in 0 until ta.length()) times += ta.optLong(j, 0L)
            if (pts.size >= 2) out[lap] = TraceData(pts, speeds, times)
        }
        return out
    }

    fun sessionCsv(base: String, slot: Int): ByteArray =
        get(base, "/api/session/file?id=$slot")

    fun sessionChunk(base: String, slot: Int, seq: Int, size: Int): SessionChunk {
        val conn = open(base, "/api/session/chunk?id=$slot&seq=$seq&size=$size", "GET")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            return SessionChunk(
                bytes = drain(conn),
                sha256 = conn.getHeaderField("X-Chunk-SHA256").orEmpty(),
            )
        } finally {
            conn.disconnect()
        }
    }

    fun status(base: String): JSONObject =
        JSONObject(get(base, "/api/status").toString(StandardCharsets.UTF_8))

    fun manifest(base: String): JSONObject =
        JSONObject(get(base, "/api/v2/manifest").toString(StandardCharsets.UTF_8))

    fun transferBusy(base: String, active: Boolean) {
        runCatching {
            post(base, "/api/transfer?state=${if (active) "begin" else "end"}")
        }
    }

    private fun parseSession(o: JSONObject): RaceSession {
        val times = ArrayList<Double>()
        val arr = o.optJSONArray("lap_times")
        if (arr != null) {
            for (i in 0 until arr.length()) times += arr.optDouble(i, 0.0)
        }
        val lapIds = ArrayList<String>()
        val ids = o.optJSONArray("lap_ids")
        if (ids != null) for (i in 0 until ids.length()) lapIds += ids.optString(i, "")
        return RaceSession(
            slot = o.optInt("slot", -1),
            id = o.optLong("id", 0L),
            trackSlot = o.optInt("track", -1),
            trackName = o.optString("track_name", ""),
            startEpoch = o.optLong("start", 0L),
            durationMs = o.optLong("duration_ms", 0L),
            samples = o.optLong("samples", 0L),
            laps = o.optInt("laps", 0),
            deletedLapMask = o.optInt("deleted_laps", 0),
            lapTimes = times,
            uuid = o.optString("session_uuid", ""),
            trackUuid = o.optString("track_uuid", ""),
            trackRevisionId = o.optString("track_revision_id", ""),
            trackVersion = o.optInt("track_version", 0),
            endEpoch = o.optLong("end", 0L),
            sampleSize = o.optInt("sample_size", 16),
            lapIds = lapIds,
            contentHash = o.optString("content_hash", ""),
        )
    }

    fun importTrack(base: String, slot: Int, payload: JSONObject): TrackUpdate {
        val points = ArrayList<android.graphics.PointF>()
        val pa = payload.optJSONArray("points") ?: JSONArray()
        if (pa.length() > 0 && pa.opt(0) is JSONArray) {
            for (i in 0 until pa.length()) {
                val q = pa.optJSONArray(i) ?: continue
                if (q.length() >= 2) points += android.graphics.PointF(
                    q.optDouble(0).toFloat(), q.optDouble(1).toFloat(),
                )
            }
        } else {
            var i = 0
            while (i + 1 < pa.length()) {
                points += android.graphics.PointF(
                    pa.optDouble(i).toFloat(), pa.optDouble(i + 1).toFloat(),
                )
                i += 2
            }
        }
        if (points.size < 2) throw IllegalArgumentException("Track 没有轨迹点")
        val target = 360
        val sampled = if (points.size == target) points else {
            val out = ArrayList<android.graphics.PointF>(target)
            for (i in 0 until target) {
                val f = i.toDouble() * points.size / target
                val i0 = f.toInt() % points.size
                val i1 = (i0 + 1) % points.size
                val t = (f - kotlin.math.floor(f)).toFloat()
                out += android.graphics.PointF(
                    points[i0].x + (points[i1].x - points[i0].x) * t,
                    points[i0].y + (points[i1].y - points[i0].y) * t,
                )
            }
            out
        }
        val sections = ArrayList<Float>()
        payload.optJSONArray("sections")?.let { a ->
            for (i in 0 until minOf(a.length(), 16)) sections += a.optDouble(i, 0.0).toFloat()
        }
        val body = java.nio.ByteBuffer.allocate(target * 2 * 4 + sections.size * 4)
            .order(java.nio.ByteOrder.LITTLE_ENDIAN)
        for (p in sampled) {
            body.putFloat(p.x)
            body.putFloat(p.y)
        }
        for (v in sections) body.putFloat(v)
        val name = URLEncoder.encode(payload.optString("name", "RESTORE"), StandardCharsets.UTF_8.name())
        val geo = payload.optBoolean("geo", false)
        val path = buildString {
            append("/api/track/import?id=$slot&name=$name")
            append("&geo=").append(if (geo) 1 else 0)
            append("&length_m=").append(String.format(Locale.US, "%.2f", payload.optDouble("length_m", 1.0)))
            append("&width=").append(String.format(Locale.US, "%.2f", payload.optDouble("width", 5.0)))
            append("&start_width=").append(String.format(Locale.US, "%.2f", payload.optDouble("start_width", 10.0)))
            append("&start_f=").append(String.format(Locale.US, "%.5f", payload.optDouble("start_f", 0.0)))
            append("&offset=").append(String.format(Locale.US, "%.2f", payload.optDouble("start_offset", 0.0)))
            append("&reverse=").append(if (payload.optBoolean("reverse", false)) 1 else 0)
            append("&vehicle=").append(payload.optInt("vehicle", 0))
            append("&revision=").append(payload.optInt("version", 1).coerceAtLeast(1))
            append("&nsec=").append(sections.size)
            val uuid = payload.optString("uuid", "")
            if (uuid.isNotBlank()) append("&uuid=").append(URLEncoder.encode(uuid, StandardCharsets.UTF_8.name()))
            val rev = payload.optString("revision_id", "")
            if (rev.isNotBlank()) append("&revision_id=").append(URLEncoder.encode(rev, StandardCharsets.UTF_8.name()))
            if (geo) {
                append("&glat0=").append(String.format(Locale.US, "%.9f", payload.optDouble("glat0", 0.0)))
                append("&glon0=").append(String.format(Locale.US, "%.9f", payload.optDouble("glon0", 0.0)))
                append("&nscale=").append(String.format(Locale.US, "%.12f", payload.optDouble("nscale", 0.0)))
                append("&nox=").append(String.format(Locale.US, "%.8f", payload.optDouble("nox", 0.0)))
                append("&noy=").append(String.format(Locale.US, "%.8f", payload.optDouble("noy", 0.0)))
            }
        }
        val conn = URL(normalizeBase(base) + path).openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = 5000
            conn.readTimeout = 20000
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/octet-stream")
            conn.setRequestProperty("Accept", "application/json")
            conn.setFixedLengthStreamingMode(body.array().size)
            conn.outputStream.use { it.write(body.array()) }
            val code = conn.responseCode
            val bytes = drain(conn)
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            val o = JSONObject(bytes.toString(StandardCharsets.UTF_8))
            if (o.optInt("ok", 0) == 0) throw IllegalStateException("设备未保存 Track")
            return TrackUpdate(
                id = o.optInt("id", slot),
                uuid = o.optString("uuid", ""),
                revision = o.optInt("version", 0),
                revisionId = o.optString("revision_id", ""),
            )
        } finally {
            conn.disconnect()
        }
    }

    fun uploadTrackImage(
        base: String, trackSlot: Int, pal16: ByteArray,
        x0: Float, y0: Float, x1: Float, y1: Float,
    ): Boolean {
        val b = String.format(
            Locale.US,
            "/api/track/image?id=%d&x0=%.6f&y0=%.6f&x1=%.6f&y1=%.6f",
            trackSlot, x0, y0, x1, y1,
        )
        val conn = URL(normalizeBase(base) + b).openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = 5000
            conn.readTimeout = 30000
            conn.useCaches = false
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/octet-stream")
            conn.setRequestProperty("Accept", "application/json")
            conn.setFixedLengthStreamingMode(pal16.size)
            conn.outputStream.use { it.write(pal16) }
            val code = conn.responseCode
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            val body = drain(conn)
            val ok = runCatching {
                JSONObject(body.toString(StandardCharsets.UTF_8)).optInt("ok", 0) != 0
            }.getOrDefault(false)
            if (!ok) throw IllegalStateException("设备未保存卫星图")
            return true
        } finally {
            conn.disconnect()
        }
    }

    private fun post(base: String, path: String): ByteArray {
        val conn = open(base, path, "POST")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            return drain(conn)
        } finally {
            conn.disconnect()
        }
    }

    private fun delete(base: String, path: String) {
        val conn = open(base, path, "DELETE")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            drain(conn)
        } finally {
            conn.disconnect()
        }
    }

    private fun get(base: String, path: String): ByteArray {
        val conn = open(base, path, "GET")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IllegalStateException("HTTP $code")
            return drain(conn)
        } finally {
            conn.disconnect()
        }
    }

    private fun open(base: String, path: String, method: String): HttpURLConnection {
        val url = URL(normalizeBase(base) + path)
        val conn = url.openConnection() as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = 5000
        conn.readTimeout = 30000
        conn.useCaches = false
        conn.setRequestProperty("Accept", "application/json, text/csv")
        return conn
    }

    private fun drain(conn: HttpURLConnection): ByteArray {
        val input = BufferedInputStream(conn.inputStream)
        input.use {
            val out = ByteArrayOutputStream()
            val buf = ByteArray(8192)
            while (true) {
                val n = it.read(buf)
                if (n <= 0) break
                out.write(buf, 0, n)
            }
            return out.toByteArray()
        }
    }
}
