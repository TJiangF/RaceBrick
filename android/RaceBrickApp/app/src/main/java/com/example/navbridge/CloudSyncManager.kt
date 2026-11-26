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
        val dataVersion: Int,
    )

    private val api = CloudApi()

    fun sync(
        context: Context,
        base: String,
        uid: String,
        token: String,
        pullCloud: Boolean = true,
        progress: (String, Int) -> Unit = { _, _ -> },
    ): Result {
        progress("检查云端 Track 修改…", 3)
        val lastRev = CloudStore.lastRevision(context, uid)
        var applied = 0
        if (pullCloud && lastRev > 0) {
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
            progress("已应用云端 Track 修改 $applied 条", 12)
        }

        progress("读取设备数据…", 18)
        val snapshot = DeviceSnapshot.build(base) { msg, pct -> progress(msg, pct) }
        progress("上传 ${snapshot.bytes / 1024} KB 到云端…", 62)
        val r = api.sync(uid, snapshot.body, token)
        val revision = r.optInt("revision", 0)
        val hid = r.optInt("history_id", 0)
        val dv = r.optInt("data_version", 0)
        val now = r.optLong("synced_at", System.currentTimeMillis() / 1000L)
        progress("更新本地快照…", 86)
        LocalCache.saveFromSync(context, uid, snapshot.body, api.snapshot(uid, token), dv)
        CloudStore.saveRevision(context, uid, revision)
        CloudStore.saveLastSync(context, uid, now, snapshot.bytes.toLong())
        progress("云端同步完成", 100)
        return Result(
            revision = revision,
            historyId = hid,
            uploadBytes = snapshot.bytes,
            tracks = snapshot.tracks,
            sessions = snapshot.sessions,
            logs = snapshot.logs,
            appliedCloudTracks = applied,
            dataVersion = dv,
        )
    }


    fun loadCloudToCache(context: Context, uid: String, token: String): Int {
        val snap = api.snapshot(uid, token)
        val version = snap.optInt("data_version", 0)
        LocalCache.saveCloudSnapshot(context, uid, snap, version)
        return version
    }

    fun uploadCachedLocal(context: Context, uid: String, token: String): Int {
        val body = LocalCache.loadUpload(context)
            ?: throw IllegalStateException("本地快照不存在")
        val r = api.sync(uid, body, token)
        val dv = r.optInt("data_version", 0)
        val snap = api.snapshot(uid, token)
        LocalCache.saveFromSync(context, uid, body, snap, dv)
        return dv
    }

    fun applyCloudTracks(context: Context, base: String, uid: String, token: String) {
        val changes = api.changes(uid, 0, token)
        val localIds = RaceApi.tracks(base).map { it.id }.toHashSet()
        val arr = changes.optJSONArray("tracks") ?: return
        for (i in 0 until arr.length()) {
            val t = arr.optJSONObject(i) ?: continue
            val id = t.optInt("local_id", -1)
            if (id !in localIds) continue
            if (t.optBoolean("deleted", false)) {
                RaceApi.deleteTrack(base, id)
                continue
            }
            val p = t.optJSONObject("payload") ?: continue
            RaceApi.updateTrack(
                base, id, p.optString("name", ""),
                p.optDouble("start_f", 0.0), p.optBoolean("reverse", false),
                p.optDouble("start_width", p.optDouble("width", 5.0)),
                p.optDouble("start_offset", 0.0),
            )
        }
    }
}
