package com.example.navbridge

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets
import java.util.zip.GZIPOutputStream

object DeviceSnapshot {
    data class Result(
        val body: JSONObject,
        val bytes: Int,
        val tracks: Int,
        val sessions: Int,
        val logs: Int,
    )

    fun build(base: String, progress: (String, Int) -> Unit = { _, _ -> }): Result {
        val body = JSONObject().apply {
            put("replace", true)
            put("since", 0)
            put("tracks", JSONArray())
            put("sessions", JSONArray())
            put("logs", JSONArray())
        }
        val tracksOut = body.getJSONArray("tracks")

        val expectedTracks = runCatching { RaceApi.status(base).optInt("tracks", -1) }.getOrDefault(-1)
        val tracks = RaceApi.tracks(base)
        if (expectedTracks >= 0 && tracks.size < expectedTracks) {
            throw IllegalStateException("设备 Track 列表不完整（${tracks.size}/$expectedTracks），已取消上传")
        }
        for ((i, t) in tracks.withIndex()) {
            progress("读取赛道 ${i + 1}/${tracks.size}", 22 + 15 * (i + 1) / tracks.size.coerceAtLeast(1))
            val d = RaceApi.track(base, t.id)
            tracksOut.put(JSONObject().apply {
                put("local_id", t.id)
                put("payload", JSONObject().apply {
                    put("id", t.id)
                    put("name", d.name)
                    put("geo", d.geoRef != null)
                    put("length_m", d.lengthM)
                    put("width", d.widthM)
                    put("start_width", d.startWidthM)
                    put("start_f", d.startF)
                    put("reverse", d.reverse)
                    put("start_offset", d.startOffsetM)
                    put("uuid", t.uuid)
                    put("version", t.revision)
                    put("revision_id", t.revisionId)
                    val ref = d.geoRef
                    if (ref != null) {
                        put("glat0", ref.lat0)
                        put("glon0", ref.lon0)
                        put("nscale", ref.nscale)
                        put("nox", ref.nox)
                        put("noy", ref.noy)
                    }
                    put("sections", JSONArray().also { a ->
                        for (v in d.sections) a.put(v)
                    })
                    put("points", JSONArray().also { a ->
                        for (p in d.points.firstOrNull()?.points.orEmpty()) {
                            a.put(JSONArray().put(p.x.toDouble()).put(p.y.toDouble()))
                        }
                    })
                })
            })
        }

        val logsOut = body.getJSONArray("logs")
        val logs = RaceApi.logs(base)
        for ((i, l) in logs.withIndex()) {
            progress("读取 Race Log ${i + 1}/${logs.size}", 37 + 8 * (i + 1) / logs.size.coerceAtLeast(1))
            val d = RaceApi.logDetail(base, l.index)
            val e = d.entry
            logsOut.put(JSONObject().apply {
                put("local_index", e.index)
                put("payload", JSONObject().apply {
                    put("index", e.index)
                    put("track_name", e.trackName)
                    put("date", e.date)
                    put("mode", e.mode)
                    put("duration", e.durationS)
                    put("laps", e.laps)
                    put("best", e.bestS)
                    put("best_index", e.bestIndex)
                    put("has_best", if (e.hasBest) 1 else 0)
                    put("vehicle", e.vehicle)
                    put("lap_times", JSONArray().also { a -> for (v in e.lapTimes) a.put(v) })
                    put("trace", JSONArray().also { a ->
                        for (p in d.trace) a.put(JSONArray().put(p.x.toDouble()).put(p.y.toDouble()))
                    })
                })
            })
        }

        val sessionsOut = body.getJSONArray("sessions")
        val sessions = RaceApi.sessions(base)
        for ((i, s) in sessions.withIndex()) {
            progress("读取 Session CSV ${i + 1}/${sessions.size}", 45 + 15 * (i + 1) / sessions.size.coerceAtLeast(1))
            val csv = RaceApi.sessionCsv(base, s.slot)
            val compressed = ByteArrayOutputStream()
            GZIPOutputStream(compressed).use { it.write(csv) }
            val csvB64 = Base64.encodeToString(compressed.toByteArray(), Base64.NO_WRAP)
            sessionsOut.put(JSONObject().apply {
                put("local_slot", s.slot)
                put("payload", JSONObject().apply {
                    put("slot", s.slot)
                    put("id", s.id)
                    put("track", s.trackSlot)
                    put("track_name", s.trackName)
                    put("start", s.startEpoch)
                    put("end", s.endEpoch)
                    put("duration_ms", s.durationMs)
                    put("samples", s.samples)
                    put("sample_size", s.sampleSize)
                    put("content_hash", s.contentHash)
                    put("session_uuid", s.uuid)
                    put("track_uuid", s.trackUuid)
                    put("track_revision_id", s.trackRevisionId)
                    put("track_version", s.trackVersion)
                    put("lap_ids", JSONArray().also { a -> for (v in s.lapIds) a.put(v) })
                    put("laps", s.laps)
                    put("deleted_laps", s.deletedLapMask)
                    put("lap_times", JSONArray().also { a -> for (v in s.lapTimes) a.put(v) })
                })
                put("csv_gzip_b64", csvB64)
            })
        }

        val text = body.toString()
        val bytes = text.toByteArray(StandardCharsets.UTF_8)
        return Result(body, bytes.size, tracks.size, sessions.size, logs.size)
    }
}
