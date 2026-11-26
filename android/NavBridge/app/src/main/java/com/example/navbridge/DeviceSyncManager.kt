package com.example.navbridge

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.zip.GZIPOutputStream

object DeviceSyncManager {
    data class Selection(
        val tracks: List<DeviceTrackRef>,
        val sessions: List<DeviceSessionRef>,
        val includeRaw: Boolean,
    )

    fun importSelected(
        base: String,
        uid: String,
        db: SyncLocalDb,
        selection: Selection,
        progress: (String) -> Unit = {},
    ): SyncLocalDb.Summary {
        val tracksOut = JSONArray()
        for ((i, ref) in selection.tracks.withIndex()) {
            progress("读取 Track ${i + 1}/${selection.tracks.size}")
            val d = RaceApi.track(base, ref.slot)
            tracksOut.put(JSONObject().apply {
                put("local_id", ref.slot)
                put("payload", trackPayload(ref, d))
            })
        }

        val sessionsOut = JSONArray()
        for ((i, ref) in selection.sessions.withIndex()) {
            progress("读取 Race Log ${i + 1}/${selection.sessions.size}")
            val s = RaceApi.session(base, ref.slot)
            val csv = if (selection.includeRaw && ref.rawAvailable) {
                RaceApi.sessionCsv(base, ref.slot)
            } else null
            val csvB64 = if (csv != null) {
                val compressed = ByteArrayOutputStream()
                GZIPOutputStream(compressed).use { it.write(csv) }
                Base64.encodeToString(compressed.toByteArray(), Base64.NO_WRAP)
            } else ""
            sessionsOut.put(JSONObject().apply {
                put("local_slot", s.slot)
                put("payload", sessionPayload(s))
                put("csv_gzip_b64", csvB64)
            })
        }

        val body = JSONObject().apply {
            put("tracks", tracksOut)
            put("sessions", sessionsOut)
            put("logs", JSONArray())
        }
        progress("写入 Local App")
        return db.importDeviceSnapshot(uid, body, replace = false)
    }

    private fun trackPayload(ref: DeviceTrackRef, d: TrackDetail): JSONObject = JSONObject().apply {
        put("id", ref.slot)
        put("uuid", ref.trackId)
        put("version", ref.version)
        put("revision_id", ref.revisionId)
        put("name", d.name)
        put("geo", d.geoRef != null)
        put("length_m", d.lengthM)
        put("width", d.widthM)
        put("start_width", d.startWidthM)
        put("start_f", d.startF)
        put("reverse", d.reverse)
        put("start_offset", d.startOffsetM)
        d.geoRef?.let {
            put("glat0", it.lat0)
            put("glon0", it.lon0)
            put("nscale", it.nscale)
            put("nox", it.nox)
            put("noy", it.noy)
        }
        put("sections", JSONArray().also { a -> for (v in d.sections) a.put(v) })
        put("points", JSONArray().also { a ->
            for (p in d.points.firstOrNull()?.points.orEmpty()) {
                a.put(JSONArray().put(p.x.toDouble()).put(p.y.toDouble()))
            }
        })
    }

    private fun sessionPayload(s: RaceSession): JSONObject = JSONObject().apply {
        put("slot", s.slot)
        put("id", s.id)
        put("session_uuid", s.uuid)
        put("track", s.trackSlot)
        put("track_uuid", s.trackUuid)
        put("track_revision_id", s.trackRevisionId)
        put("track_version", s.trackVersion)
        put("track_name", s.trackName)
        put("start", s.startEpoch)
        put("end", s.endEpoch)
        put("duration_ms", s.durationMs)
        put("samples", s.samples)
        put("sample_size", s.sampleSize)
        put("laps", s.laps)
        put("deleted_laps", s.deletedLapMask)
        put("lap_times", JSONArray().also { a -> for (v in s.lapTimes) a.put(v) })
        put("lap_ids", JSONArray().also { a -> for (v in s.lapIds) a.put(v) })
    }
}
