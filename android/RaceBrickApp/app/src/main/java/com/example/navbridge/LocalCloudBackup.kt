package com.example.navbridge

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.charset.StandardCharsets
import java.util.zip.GZIPOutputStream

object LocalCloudBackup {
    data class Result(
        val revision: Int,
        val dataVersion: Int,
        val tracks: Int,
        val sessions: Int,
        val bytes: Int,
    )

    data class RestoreResult(
        val tracks: Int,
        val sessions: Int,
    )

    fun restore(db: SyncLocalDb, uid: String, token: String): RestoreResult {
        val snap = CloudApi().snapshot(uid, token)
        val tracksOut = JSONArray()
        var tracks = 0
        snap.optJSONArray("tracks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val t = arr.optJSONObject(i) ?: continue
                val payload = t.optJSONObject("payload") ?: continue
                if (!payload.has("uuid")) payload.put("uuid", t.optString("track_uuid", ""))
                if (!payload.has("version")) payload.put("version", t.optInt("version", 1))
                if (!payload.has("revision_id")) payload.put("revision_id", t.optString("revision_id", ""))
                tracksOut.put(JSONObject()
                    .put("local_id", t.optInt("local_id", -1))
                    .put("payload", payload))
                tracks++
            }
        }
        val sessionsOut = JSONArray()
        var sessions = 0
        snap.optJSONArray("sessions")?.let { arr ->
            for (i in 0 until arr.length()) {
                val s = arr.optJSONObject(i) ?: continue
                val payload = s.optJSONObject("payload") ?: continue
                val item = JSONObject()
                    .put("local_slot", s.optInt("local_slot", payload.optInt("slot", -1)))
                    .put("payload", payload)
                val csv = s.optString("csv", "")
                if (csv.isNotBlank()) {
                    val compressed = ByteArrayOutputStream()
                    GZIPOutputStream(compressed).use { it.write(csv.toByteArray(StandardCharsets.UTF_8)) }
                    item.put("csv_gzip_b64",
                        Base64.encodeToString(compressed.toByteArray(), Base64.NO_WRAP))
                }
                sessionsOut.put(item)
                sessions++
            }
        }
        val body = JSONObject()
            .put("tracks", tracksOut)
            .put("sessions", sessionsOut)
            .put("logs", JSONArray())
        db.importDeviceSnapshot(uid, body, replace = false)
        return RestoreResult(tracks, sessions)
    }

    fun restoreSnapshot(db: SyncLocalDb, uid: String, snapshot: JSONObject): RestoreResult {
        val tracksOut = JSONArray()
        var tracks = 0
        snapshot.optJSONArray("tracks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val t = arr.optJSONObject(i) ?: continue
                val payload = t.optJSONObject("payload") ?: continue
                tracksOut.put(JSONObject()
                    .put("local_id", t.optInt("local_id", -1))
                    .put("payload", payload))
                tracks++
            }
        }
        val sessionsOut = JSONArray()
        var sessions = 0
        snapshot.optJSONArray("sessions")?.let { arr ->
            for (i in 0 until arr.length()) {
                val s = arr.optJSONObject(i) ?: continue
                val payload = s.optJSONObject("payload") ?: continue
                val item = JSONObject()
                    .put("local_slot", s.optInt("local_slot", payload.optInt("slot", -1)))
                    .put("payload", payload)
                val gz = s.optString("csv_gzip_b64", "")
                if (gz.isNotBlank()) {
                    item.put("csv_gzip_b64", gz)
                } else {
                    val csv = s.optString("csv", "")
                    if (csv.isNotBlank()) {
                        val compressed = ByteArrayOutputStream()
                        GZIPOutputStream(compressed).use { it.write(csv.toByteArray(StandardCharsets.UTF_8)) }
                        item.put("csv_gzip_b64",
                            Base64.encodeToString(compressed.toByteArray(), Base64.NO_WRAP))
                    }
                }
                sessionsOut.put(item)
                sessions++
            }
        }
        db.importDeviceSnapshot(uid, JSONObject()
            .put("tracks", tracksOut)
            .put("sessions", sessionsOut)
            .put("logs", JSONArray()), replace = false)
        return RestoreResult(tracks, sessions)
    }

    fun backup(db: SyncLocalDb, uid: String, token: String): Result {
        val api = CloudApi()
        val cloud = api.deviceManifest(uid, token)
        val cloudTracks = HashMap<String, JSONObject>()
        cloud.optJSONArray("tracks")?.let { arr ->
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let {
                    cloudTracks[it.optString("track_id", "")] = it
                }
            }
        }
        val cloudSessions = HashSet<String>()
        cloud.optJSONArray("sessions")?.let { arr ->
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let {
                    cloudSessions += it.optString("session_id", "")
                }
            }
        }
        val tracksOut = JSONArray()
        var trackCount = 0
        for (t in db.tracks()) {
            val p = JSONObject(t.payload.toString())
            val uuid = t.uuid
            p.put("uuid", uuid)
            p.put("version", t.version)
            p.put("revision_id", t.revisionId)
            if (!p.has("id")) p.put("id", t.localKey.toIntOrNull() ?: -1)
            val c = cloudTracks[uuid]
            if (c != null &&
                c.optInt("version", -1) == p.optInt("version", 1) &&
                c.optString("revision_id", "") == p.optString("revision_id", "")
            ) continue
            tracksOut.put(JSONObject()
                .put("local_id", t.localKey.toIntOrNull() ?: -1)
                .put("payload", p))
            trackCount++
        }
        val sessionsOut = JSONArray()
        var sessionCount = 0
        for (s in db.sessions()) {
            val p = JSONObject(s.payload.toString())
            val sessionId = p.optString("session_uuid", s.uuid)
            if (sessionId in cloudSessions) continue
            if (!p.has("slot")) p.put("slot", s.localKey.toIntOrNull() ?: -1)
            val item = JSONObject()
                .put("local_slot", s.localKey.toIntOrNull() ?: -1)
                .put("payload", p)
            val blob = s.blobPath?.let { File(it) }
            if (blob?.isFile == true) {
                item.put("csv_gzip_b64",
                    Base64.encodeToString(blob.readBytes(), Base64.NO_WRAP))
            }
            sessionsOut.put(item)
            sessionCount++
        }
        if (trackCount == 0 && sessionCount == 0) {
            return Result(
                revision = cloud.optInt("revision", 0),
                dataVersion = cloud.optInt("data_version", 0),
                tracks = 0,
                sessions = 0,
                bytes = 0,
            )
        }
        val body = JSONObject()
            .put("replace", false)
            .put("tracks", tracksOut)
            .put("sessions", sessionsOut)
            .put("logs", JSONArray())
        val text = body.toString()
        val r = api.sync(uid, body, token)
        val revision = r.optInt("revision", 0)
        val dataVersion = r.optInt("data_version", 0)
        return Result(
            revision = revision,
            dataVersion = dataVersion,
            tracks = trackCount,
            sessions = sessionCount,
            bytes = text.toByteArray(Charsets.UTF_8).size,
        )
    }
}
