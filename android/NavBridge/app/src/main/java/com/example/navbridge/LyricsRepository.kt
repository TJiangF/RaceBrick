package com.example.navbridge

import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/** Best-effort synchronized lyrics (LRC) lookup. The system MediaSession does
 *  not expose lyrics, so this uses LRCLIB and falls back to plain lyrics. */
object LyricsRepository {
    data class Line(val timeMs: Long, val text: String)

    private const val BASE = "https://lrclib.net/api"

    fun fetch(title: String, artist: String, album: String, durationMs: Long): List<Line> {
        if (title.isBlank()) return emptyList()
        val enc = { s: String -> URLEncoder.encode(s, "UTF-8") }
        val duration = if (durationMs > 0) durationMs / 1000 else 0
        val get = "$BASE/get?track_name=${enc(title)}&artist_name=${enc(artist)}" +
            "&album_name=${enc(album)}&duration=$duration"
        parseJson(http(get))?.let { if (it.isNotEmpty()) return it }

        val search = "$BASE/search?q=${enc("$artist $title")}"
        val arr = http(search)?.let { runCatching { JSONArray(it) }.getOrNull() }
        if (arr != null) {
            for (i in 0 until arr.length()) {
                val obj = arr.optJSONObject(i) ?: continue
                parseObject(obj)?.let { if (it.isNotEmpty()) return it }
            }
        }
        return emptyList()
    }

    private fun http(url: String): String? {
        var c: HttpURLConnection? = null
        return runCatching {
            c = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 4500
                readTimeout = 4500
                requestMethod = "GET"
                setRequestProperty("User-Agent", "RaceBrick/0.9.13 (Android)")
                setRequestProperty("Accept", "application/json")
            }
            if (c!!.responseCode !in 200..299) return null
            c!!.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
        }.getOrNull().also { runCatching { c?.disconnect() } }
    }

    private fun parseJson(text: String?): List<Line>? {
        if (text.isNullOrBlank()) return null
        return runCatching { parseObject(JSONObject(text)) }.getOrNull()
    }

    private fun parseObject(obj: JSONObject): List<Line>? {
        val synced = obj.optString("syncedLyrics").takeUnless { it == "null" }.orEmpty()
        if (synced.isNotBlank()) {
            val timed = parseLrc(synced)
            if (timed.isNotEmpty()) return timed
        }
        val plain = obj.optString("plainLyrics").takeUnless { it == "null" }.orEmpty()
        if (plain.isBlank()) return null
        val lines = plain.lines().map { it.trim() }.filter { it.isNotEmpty() }
        if (lines.isEmpty()) return null
        return lines.mapIndexed { i, text -> Line(i * 4000L, text) }
    }

    private fun parseLrc(lrc: String): List<Line> {
        val stamp = Regex("""\[(\d{1,2}):(\d{2})(?:[.:](\d{1,3}))?\]""")
        val out = ArrayList<Line>()
        for (raw in lrc.lines()) {
            val marks = stamp.findAll(raw).toList()
            if (marks.isEmpty()) continue
            val text = raw.substring(marks.last().range.last + 1).trim()
            if (text.isEmpty()) continue
            for (m in marks) {
                val min = m.groupValues[1].toLongOrNull() ?: continue
                val sec = m.groupValues[2].toLongOrNull() ?: continue
                val fracRaw = m.groupValues[3]
                val frac = when (fracRaw.length) {
                    0 -> 0L
                    1 -> fracRaw.toLong() * 100
                    2 -> fracRaw.toLong() * 10
                    else -> fracRaw.take(3).toLong()
                }
                out.add(Line((min * 60 + sec) * 1000 + frac, text))
            }
        }
        out.sortBy { it.timeMs }
        return out
    }
}
