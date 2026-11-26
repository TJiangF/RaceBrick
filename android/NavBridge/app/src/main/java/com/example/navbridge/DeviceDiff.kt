package com.example.navbridge

import org.json.JSONObject

data class DeviceTrackRef(
    val slot: Int,
    val trackId: String,
    val name: String,
    val version: Int,
    val revisionId: String,
)

data class DeviceSessionRef(
    val slot: Int,
    val sessionId: String,
    val trackId: String,
    val trackName: String,
    val start: Long,
    val laps: Int,
    val rawAvailable: Boolean,
)

data class DeviceDiffItem(
    val type: String,
    val label: String,
    val track: DeviceTrackRef? = null,
    val session: DeviceSessionRef? = null,
    val conflict: Boolean = false,
)

object DeviceDiff {
    fun compare(device: JSONObject, local: JSONObject, dirtyTracks: Set<String>): List<DeviceDiffItem> {
        val out = ArrayList<DeviceDiffItem>()
        val localTracks = HashMap<String, JSONObject>()
        local.optJSONArray("tracks")?.let { arr ->
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let { o ->
                    localTracks[o.optString("track_id")] = o
                }
            }
        }
        device.optJSONArray("tracks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val ref = DeviceTrackRef(
                    slot = o.optInt("local_slot", -1),
                    trackId = o.optString("track_id", ""),
                    name = o.optString("name", ""),
                    version = o.optInt("version", 0),
                    revisionId = o.optString("revision_id", ""),
                )
                if (ref.trackId.isBlank()) continue
                val l = localTracks[ref.trackId]
                if (l == null) {
                    out += DeviceDiffItem(
                        type = "track",
                        label = "[Track] ${ref.name} · 设备新增 v${ref.version}",
                        track = ref,
                    )
                    continue
                }
                val lv = l.optInt("version", 0)
                val lr = l.optString("revision_id", "")
                if (lv == ref.version && lr == ref.revisionId) continue
                val dirty = ref.trackId in dirtyTracks
                val detail = if (lv > ref.version) {
                    "本地 v$lv 较新"
                } else {
                    "设备 v${ref.version} / 本地 v$lv"
                }
                out += DeviceDiffItem(
                    type = "track",
                    label = "[Track] ${ref.name} · $detail${if (dirty) " · 本地有修改" else ""}",
                    track = ref,
                    conflict = dirty || lv > ref.version,
                )
            }
        }
        val localSessions = HashSet<String>()
        local.optJSONArray("sessions")?.let { arr ->
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let {
                    localSessions += it.optString("session_id", "")
                }
            }
        }
        device.optJSONArray("sessions")?.let { arr ->
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val ref = DeviceSessionRef(
                    slot = o.optInt("local_slot", o.optInt("slot", -1)),
                    sessionId = o.optString("session_id", o.optString("session_uuid", "")),
                    trackId = o.optString("track_id", ""),
                    trackName = o.optString("track_name", ""),
                    start = o.optLong("start", 0L),
                    laps = o.optInt("laps", 0),
                    rawAvailable = o.optBoolean("telemetry_available", true),
                )
                if (ref.sessionId.isBlank() || ref.sessionId in localSessions) continue
                out += DeviceDiffItem(
                    type = "session",
                    label = "[Race Log] ${ref.trackName} · ${formatDate(ref.start)} · ${ref.laps} 圈" +
                        if (ref.rawAvailable) " · RAW" else "",
                    session = ref,
                )
            }
        }
        return out
    }

    private fun formatDate(epoch: Long): String =
        if (epoch <= 1_600_000_000L) "时间未知"
        else java.text.SimpleDateFormat("MM-dd HH:mm", java.util.Locale.getDefault())
            .format(java.util.Date(epoch * 1000L))
}
