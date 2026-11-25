package com.example.navbridge

import android.content.Context
import org.json.JSONObject

object CloudSyncManager {
    data class Result(
        val revision: Int,
        val historyId: Int,
        val uploadBytes: Int,
        val tracks: Int,
        val sessions: Int,
        val logs: Int,
        val appliedCloudTracks: Int,
    )

    private val api = CloudApi()

    fun sync(
        context: Context,
        base: String,
        uid: String,
        token: String,
        progress: (String) -> Unit = {},
    ): Result {
        val lastRev = CloudStore.lastRevision(context, uid)
        var applied = 0
        if (lastRev > 0) {
            progress("检查云端 Track 修改…")
            val changes = api.changes(uid, lastRev, token)
            val localIds = RaceApi.tracks(base).map { it.id }.toHashSet()
            val arr = changes.optJSONArray("tracks")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val t = arr.optJSONObject(i) ?: continue
                    val id = t.optInt("local_id", -1)
                    if (id !in localIds) continue
                    if (t.optBoolean("deleted", false)) {
                        RaceApi.deleteTrack(base, id)
                        applied++
                        continue
                    }
                    val p = t.optJSONObject("payload") ?: continue
                    val name = p.optString("name", "")
                    val sf = p.optDouble("start_f", 0.0)
                    val sw = p.optDouble("start_width", p.optDouble("width", 5.0))
                    val off = p.optDouble("start_offset", 0.0)
                    val rev = p.optBoolean("reverse", false)
                    RaceApi.updateTrack(base, id, name, sf, rev, sw, off)
                    applied++
                }
            }
            progress("已应用云端 Track 修改 $applied 条")
        }

        val snapshot = DeviceSnapshot.build(base, progress)
        progress("上传 ${snapshot.bytes / 1024} KB 到云端…")
        val r = api.sync(uid, snapshot.body, token)
        val revision = r.optInt("revision", 0)
        val hid = r.optInt("history_id", 0)
        val now = r.optLong("synced_at", System.currentTimeMillis() / 1000L)
        CloudStore.saveRevision(context, uid, revision)
        CloudStore.saveLastSync(context, uid, now, snapshot.bytes.toLong())
        return Result(
            revision = revision,
            historyId = hid,
            uploadBytes = snapshot.bytes,
            tracks = snapshot.tracks,
            sessions = snapshot.sessions,
            logs = snapshot.logs,
            appliedCloudTracks = applied,
        )
    }
}
