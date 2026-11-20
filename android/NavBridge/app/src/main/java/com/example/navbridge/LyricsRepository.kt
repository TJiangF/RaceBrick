package com.example.navbridge

import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/** Best-effort synchronized lyrics (LRC) lookup. The system MediaSession does
 *  not expose lyrics, so this uses LRCLIB and falls back to plain lyrics. */
object LyricsRepository {
    data class Line(val timeMs: Long, val text: String)
    data class Result(val lines: List<Line>, val source: String)

    private const val BASE = "https://lrclib.net/api"
    private const val NETEASE = "https://music.163.com/api"

    /* mode: 0=AUTO, 1=LRCLIB exact, 2=NetEase, 3=notification, 4=title */
    fun fetch(title: String, artist: String, album: String, durationMs: Long,
              mode: Int = 0): Result {
        if (title.isBlank() || mode == 3 || mode == 4) {
            return Result(emptyList(), "NONE")
        }
        val enc = { s: String -> URLEncoder.encode(s, "UTF-8") }
        val duration = if (durationMs > 0) durationMs / 1000 else 0
        if (mode == 0 || mode == 1) {
            val get = "$BASE/get?track_name=${enc(title)}&artist_name=${enc(artist)}" +
                "&album_name=${enc(album)}&duration=$duration"
            parseJson(http(get))?.let { if (it.isNotEmpty()) return Result(it, "LRC-G") }
        }
        if (mode == 1) return Result(emptyList(), "NONE")

        // LRCLIB has a much smaller Chinese catalogue (e.g. 7-11 的爱恋 is
        // absent), so fall back to NetEase Cloud Music's public search/lyric
        // endpoints. This is only a plain GET, no account/cookie required.
        val netease = fetchNetease(title, artist, duration)
        if (netease.isNotEmpty()) return Result(netease, "NE")
        return Result(emptyList(), "NONE")
    }

    private fun norm(s: String): String = s.lowercase().filter { it.isLetterOrDigit() }

    private fun fetchNetease(title: String, artist: String, durationS: Long): List<Line> {
        val enc = { s: String -> URLEncoder.encode(s, "UTF-8") }
        val q = listOf(title, artist).filter { it.isNotBlank() }.joinToString(" ")
        val searchUrl = "$NETEASE/search/get/web?s=${enc(q)}&type=1&offset=0&total=true&limit=10"
        val text = http(searchUrl, "https://music.163.com/") ?: return emptyList()
        val root = runCatching { JSONObject(text) }.getOrNull() ?: return emptyList()
        val songs = root.optJSONObject("result")?.optJSONArray("songs") ?: return emptyList()
        val wantTitle = norm(title)
        val wantArtist = norm(artist)
        var bestId = 0L
        var bestScore = -1
        for (i in 0 until songs.length()) {
            val song = songs.optJSONObject(i) ?: continue
            val name = song.optString("name")
            var score = if (norm(name) == wantTitle) 4 else 0
            val artists = song.optJSONArray("artists")
            if (artists != null && wantArtist.isNotEmpty()) {
                for (j in 0 until artists.length()) {
                    val a = norm(artists.optJSONObject(j)?.optString("name").orEmpty())
                    if (a.isNotEmpty() && (a.contains(wantArtist) || wantArtist.contains(a))) {
                        score += 3
                        break
                    }
                }
            }
            if (durationS > 0) {
                val d = song.optLong("duration") / 1000
                if (d > 0) {
                    val diff = if (d > durationS) d - durationS else durationS - d
                    if (diff <= 3) score += 2 else if (diff > 15) score -= 2
                }
            }
            if (score > bestScore) {
                bestScore = score
                bestId = song.optLong("id")
            }
        }
        if (bestId <= 0L) return emptyList()
        val lyricUrl = "$NETEASE/song/lyric?id=$bestId&lv=1&kv=1&tv=-1"
        val lyricText = http(lyricUrl, "https://music.163.com/") ?: return emptyList()
        val lyricRoot = runCatching { JSONObject(lyricText) }.getOrNull() ?: return emptyList()
        val lrc = lyricRoot.optJSONObject("lrc")?.optString("lyric").orEmpty()
        if (lrc.isBlank() || lrc == "null") return emptyList()
        return parseLrc(lrc)
    }

    private fun http(url: String, referer: String? = null): String? {
        var c: HttpURLConnection? = null
        return runCatching {
            c = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 4500
                readTimeout = 4500
                requestMethod = "GET"
                setRequestProperty("User-Agent", "RaceBrick/0.9.30 (Android)")
                setRequestProperty("Accept", "application/json")
                if (referer != null) setRequestProperty("Referer", referer)
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
