package com.example.navbridge

import android.content.Context
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.GZIPOutputStream

object DeviceSyncManager {
    data class SessionPick(
        val ref: DeviceSessionRef,
        val includeRaw: Boolean,
    )

    data class Selection(
        val tracks: List<DeviceTrackRef>,
        val sessions: List<SessionPick>,
    )

    data class ImportResult(
        val summary: SyncLocalDb.Summary,
        val tracks: Int,
        val sessions: Int,
        val failures: List<String>,
    )

    fun importSelected(
        context: Context,
        base: String,
        uid: String,
        db: SyncLocalDb,
        selection: Selection,
        progress: (String) -> Unit = {},
    ): ImportResult {
        val cleanup = ArrayList<File>()
        val failures = ArrayList<String>()
        var trackCount = 0
        var sessionCount = 0
        val tracksOut = JSONArray()
        for ((i, ref) in selection.tracks.withIndex()) {
            progress("读取 Track ${i + 1}/${selection.tracks.size}")
            try {
                val d = RaceApi.track(base, ref.slot)
                tracksOut.put(JSONObject().apply {
                    put("local_id", ref.slot)
                    put("payload", trackPayload(ref, d))
                })
                trackCount++
            } catch (e: Exception) {
                failures += "Track ${ref.name}: ${e.message}"
            }
        }

        val sessionsOut = JSONArray()
        for ((i, pick) in selection.sessions.withIndex()) {
            val ref = pick.ref
            progress("读取 Race Log ${i + 1}/${selection.sessions.size}")
            try {
                val s = RaceApi.session(base, ref.slot)
                val csv = if (pick.includeRaw && ref.rawAvailable) {
                    downloadCsvChunked(context, base, s, cleanup) { message -> progress(message) }
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
                sessionCount++
            } catch (e: Exception) {
                failures += "Race Log ${ref.trackName}: ${e.message}"
            }
        }

        val body = JSONObject().apply {
            put("tracks", tracksOut)
            put("sessions", sessionsOut)
            put("logs", JSONArray())
        }
        progress("写入 Local App")
        val summary = db.importDeviceSnapshot(uid, body, replace = false)
        for (f in cleanup) runCatching { f.delete() }
        return ImportResult(summary, trackCount, sessionCount, failures)
    }

    private fun downloadCsvChunked(
        context: Context,
        base: String,
        s: RaceSession,
        cleanup: MutableList<File>,
        progress: (String) -> Unit,
    ): ByteArray {
        if (s.samples <= 0) return RaceApi.sessionCsv(base, s.slot)
        val safe = s.uuid.ifBlank { "slot-${s.slot}" }
        val dir = File(context.filesDir, "sync_transfers").apply { mkdirs() }
        val part = File(dir, "$safe.csv.part")
        val stateFile = File(dir, "$safe.csv.state")
        val chunkSize = 2000
        val total = s.samples
        var nextSeq = 0
        if (part.isFile && stateFile.isFile) {
            val state = runCatching { JSONObject(stateFile.readText()) }.getOrNull()
            if (state != null &&
                state.optString("session") == safe &&
                state.optLong("total", -1) == total &&
                state.optInt("chunk", -1) == chunkSize
            ) {
                nextSeq = state.optInt("next", 0)
                val expected = state.optLong("bytes", -1L)
                if (expected < 0L || part.length() < expected) {
                    nextSeq = 0
                } else if (part.length() > expected) {
                    java.io.RandomAccessFile(part, "rw").use { it.setLength(expected) }
                }
            } else {
                part.delete()
                stateFile.delete()
            }
        } else {
            part.delete()
            stateFile.delete()
        }
        if (nextSeq == 0) {
            part.writeText(csvHeader(s))
        }
        cleanup += part
        cleanup += stateFile
        val chunks = ((total + chunkSize - 1) / chunkSize).toInt()
        for (seq in nextSeq until chunks) {
            var last: Exception? = null
            var bytes: ByteArray? = null
            for (attempt in 1..3) {
                try {
                    val chunk = RaceApi.sessionChunk(base, s.slot, seq, chunkSize)
                    if (chunk.sha256.isNotBlank() &&
                        SyncLocalDb.sha256(chunk.bytes) != chunk.sha256) {
                        throw IllegalStateException("chunk SHA-256 mismatch")
                    }
                    bytes = chunk.bytes
                    break
                } catch (e: Exception) {
                    last = e
                    if (attempt < 3) Thread.sleep(400L * attempt)
                }
            }
            if (bytes == null) throw last ?: IllegalStateException("chunk failed")
            part.appendBytes(bytes)
            stateFile.writeText(JSONObject()
                .put("session", safe)
                .put("total", total)
                .put("chunk", chunkSize)
                .put("next", seq + 1)
                .put("bytes", part.length())
                .toString())
            progress("Raw Telemetry ${seq + 1}/$chunks")
        }
        return part.readBytes()
    }

    private fun csvHeader(s: RaceSession): String {
        val v2 = s.sampleSize == 20
        val name = s.trackName.replace("\"", "\"\"")
        return buildString {
            append("Format,3\n")
            append("Session title,\"$name\"\n")
            append("Session type,Lap timing\n")
            append("Track name,\"$name\"\n")
            append("timestamp,fragment_id,lap_number,elapsed_time,latitude,longitude,speed")
            if (v2) append(",gps_accuracy_m,satellites,fix_type")
            append("\n")
            append("unix time,,,s,deg,deg,m/s")
            if (v2) append(",m,,count")
            append("\n")
            append(",,,,100: gps,100: gps,100: gps")
            if (v2) append(",100: gps,100: gps,100: gps")
            append("\n")
        }
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
