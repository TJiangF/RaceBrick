package com.example.navbridge

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets

data class RaceTrack(
    val id: Int,
    val name: String,
    val geo: Boolean,
    val lengthM: Double,
    val sections: Int,
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

    fun deleteTrack(base: String, id: Int) {
        delete(base, "/api/track?id=$id")
    }

    fun deleteSession(base: String, slot: Int) {
        delete(base, "/api/session?id=$slot")
    }

    fun deleteLap(base: String, slot: Int, lap: Int) {
        delete(base, "/api/session/lap?id=$slot&lap=$lap")
    }

    fun sessionCsv(base: String, slot: Int): ByteArray =
        get(base, "/api/session/file?id=$slot")

    private fun parseSession(o: JSONObject): RaceSession {
        val times = ArrayList<Double>()
        val arr = o.optJSONArray("lap_times")
        if (arr != null) {
            for (i in 0 until arr.length()) times += arr.optDouble(i, 0.0)
        }
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
        )
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
