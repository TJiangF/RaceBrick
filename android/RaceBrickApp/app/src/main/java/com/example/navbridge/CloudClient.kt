package com.example.navbridge

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

object CloudStore {
    private const val PREFS = "race_cloud"
    private const val KEY_TOKEN = "token"
    private const val KEY_USER = "user"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun token(context: Context): String? =
        prefs(context).getString(KEY_TOKEN, null)?.takeIf { it.isNotBlank() }

    fun username(context: Context): String =
        prefs(context).getString(KEY_USER, "").orEmpty()

    fun save(context: Context, token: String, username: String) {
        prefs(context).edit().putString(KEY_TOKEN, token).putString(KEY_USER, username).apply()
    }

    fun clear(context: Context) {
        prefs(context).edit().clear().apply()
    }

    fun lastRevision(context: Context, uid: String): Int =
        prefs(context).getInt("rev_$uid", 0)

    fun saveRevision(context: Context, uid: String, revision: Int) {
        prefs(context).edit().putInt("rev_$uid", revision).apply()
    }

    fun lastSyncAt(context: Context, uid: String): Long =
        prefs(context).getLong("sync_at_$uid", 0L)

    fun lastSyncBytes(context: Context, uid: String): Long =
        prefs(context).getLong("sync_bytes_$uid", 0L)

    fun saveLastSync(context: Context, uid: String, at: Long, bytes: Long) {
        prefs(context).edit()
            .putLong("sync_at_$uid", at)
            .putLong("sync_bytes_$uid", bytes)
            .apply()
    }

    fun lastReminder(context: Context, uid: String): Long =
        prefs(context).getLong("reminder_at_$uid", 0L)

    fun saveReminder(context: Context, uid: String, at: Long) {
        prefs(context).edit().putLong("reminder_at_$uid", at).apply()
    }
}

data class CloudAccount(val username: String, val token: String, val admin: Boolean)

data class CloudDeviceState(
    val revision: Int,
    val dataVersion: Int,
    val tracks: Int,
    val sessions: Int,
    val logs: Int,
    val history: JSONArray,
)

data class PublicTrackSummary(
    val publicId: String,
    val trackUuid: String,
    val name: String,
    val geo: Boolean,
    val lengthM: Double,
    val updatedAt: Long,
)

data class PublicTracksPage(val tracks: List<PublicTrackSummary>, val nextCursor: String?)

data class PublicTrack(
    val publicId: String,
    val trackUuid: String,
    val name: String,
    val payload: JSONObject,
)

data class PublicLeaderboardEntry(
    val rank: Int,
    val driver: String,
    val lapNumber: Int,
    val lapTimeMs: Long,
    val updatedAt: Long,
)

data class PublicLeaderboardPage(
    val entries: List<PublicLeaderboardEntry>,
    val nextCursor: String?,
)

class CloudApi {
    companion object {
        const val BASE_URL = "https://racebrick.tfworks.cn"
    }

    fun login(username: String, password: String): CloudAccount =
        auth("/api/platform/login", username, password)

    fun register(username: String, password: String): CloudAccount =
        auth("/api/platform/register", username, password)

    private fun auth(path: String, username: String, password: String): CloudAccount {
        val body = JSONObject().put("username", username).put("password", password)
        val o = request("POST", path, null, body.toString().toByteArray(StandardCharsets.UTF_8))
        val user = o.optJSONObject("user") ?: JSONObject()
        return CloudAccount(
            username = user.optString("username", username),
            token = o.optString("token", ""),
            admin = user.optString("role", "") == "admin",
        )
    }

    fun state(uid: String, token: String): CloudDeviceState {
        val o = request("GET", "/api/phone/v1/devices/${path(uid)}/state", token, null)
        return CloudDeviceState(
            revision = o.optInt("revision", 0),
            dataVersion = o.optInt("data_version", 0),
            tracks = o.optInt("tracks", 0),
            sessions = o.optInt("sessions", 0),
            logs = o.optInt("logs", 0),
            history = o.optJSONArray("history") ?: JSONArray(),
        )
    }

    fun snapshot(uid: String, token: String): JSONObject =
        request("GET", "/api/phone/v1/devices/${path(uid)}/snapshot", token, null)

    fun deviceManifest(uid: String, token: String): JSONObject =
        request("GET", "/api/v2/devices/${path(uid)}/manifest", token, null)

    fun changes(uid: String, since: Int, token: String): JSONObject =
        request("GET", "/api/phone/v1/devices/${path(uid)}/changes?since=$since", token, null)

    fun sync(uid: String, body: JSONObject, token: String): JSONObject =
        request("POST", "/api/phone/v1/devices/${path(uid)}/sync", token,
            body.toString().toByteArray(StandardCharsets.UTF_8), 180_000)

    fun history(uid: String, token: String): JSONArray =
        request("GET", "/api/phone/v1/devices/${path(uid)}/history", token, null)
            .optJSONArray("history") ?: JSONArray()

    fun historyDetail(uid: String, historyId: Int, token: String): JSONObject =
        request("GET", "/api/platform/devices/${path(uid)}/sync-history/$historyId", token, null)

    fun publicTracks(limit: Int = 20, cursor: String? = null): PublicTracksPage {
        val q = buildString {
            append("/api/public/v1/tracks?limit=").append(limit.coerceIn(1, 50))
            if (!cursor.isNullOrBlank()) append("&cursor=").append(URLEncoder.encode(cursor, "UTF-8"))
        }
        val o = request("GET", q, null, null)
        val arr = o.optJSONArray("tracks") ?: JSONArray()
        val out = ArrayList<PublicTrackSummary>(arr.length())
        for (i in 0 until arr.length()) {
            val t = arr.optJSONObject(i) ?: continue
            out += PublicTrackSummary(
                publicId = t.optString("public_id", ""),
                trackUuid = t.optString("track_uuid", ""),
                name = t.optString("name", ""),
                geo = t.optBoolean("geo", false),
                lengthM = t.optDouble("length_m", 0.0),
                updatedAt = t.optLong("updated_at", 0L),
            )
        }
        return PublicTracksPage(out, o.optString("next_cursor", "").ifBlank { null })
    }

    fun publicTrack(publicId: String): PublicTrack {
        val path = "/api/public/v1/tracks/" + URLEncoder.encode(publicId, "UTF-8")
        val o = request("GET", path, null, null)
        val t = o.optJSONObject("track") ?: throw IllegalStateException("公开赛道响应为空")
        val payload = JSONObject()
        for (k in arrayOf(
            "name", "geo", "length_m", "width", "start_width", "start_f",
            "start_offset", "reverse", "sections", "points", "glat0", "glon0",
            "nscale", "nox", "noy",
        )) {
            if (t.has(k)) payload.put(k, t.opt(k))
        }
        val uuid = t.optString("track_uuid", "")
        payload.put("uuid", uuid)
        payload.put("id", 0)
        payload.put("version", 1)
        return PublicTrack(
            publicId = t.optString("public_id", publicId),
            trackUuid = uuid,
            name = t.optString("name", ""),
            payload = payload,
        )
    }

    fun publicLeaderboard(
        publicId: String,
        limit: Int = 20,
        cursor: String? = null,
    ): PublicLeaderboardPage {
        val q = buildString {
            append("/api/public/v1/tracks/")
            append(URLEncoder.encode(publicId, "UTF-8"))
            append("/leaderboard?limit=").append(limit.coerceIn(1, 50))
            if (!cursor.isNullOrBlank()) append("&cursor=").append(URLEncoder.encode(cursor, "UTF-8"))
        }
        val o = request("GET", q, null, null)
        val arr = o.optJSONArray("entries") ?: JSONArray()
        val out = ArrayList<PublicLeaderboardEntry>(arr.length())
        for (i in 0 until arr.length()) {
            val e = arr.optJSONObject(i) ?: continue
            out += PublicLeaderboardEntry(
                rank = e.optInt("rank", i + 1),
                driver = e.optString("driver", "匿名车手"),
                lapNumber = e.optInt("lap_number", 0),
                lapTimeMs = e.optLong("lap_time_ms", 0L),
                updatedAt = e.optLong("updated_at", 0L),
            )
        }
        return PublicLeaderboardPage(out, o.optString("next_cursor", "").ifBlank { null })
    }

    private fun path(uid: String): String =
        URLEncoder.encode(uid, StandardCharsets.UTF_8.name())

    private fun request(
        method: String,
        path: String,
        token: String?,
        body: ByteArray?,
        readTimeoutMs: Int = 45_000,
    ): JSONObject {
        val conn = (URL(BASE_URL + path).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 12_000
            readTimeout = readTimeoutMs
            useCaches = false
            setRequestProperty("Accept", "application/json")
            if (token != null) setRequestProperty("Authorization", "Bearer $token")
            if (body != null) {
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
                setFixedLengthStreamingMode(body.size)
            }
        }
        try {
            if (body != null) conn.outputStream.use { it.write(body) }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.let { input ->
                BufferedInputStream(input).use { i ->
                    val out = ByteArrayOutputStream()
                    val buf = ByteArray(8192)
                    while (true) {
                        val n = i.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                    }
                    out.toByteArray().toString(StandardCharsets.UTF_8)
                }
            }.orEmpty()
            if (code !in 200..299) {
                val detail = runCatching { JSONObject(text).optString("detail") }
                    .getOrNull().orEmpty().ifBlank { text.take(300) }
                throw IllegalStateException("HTTP $code: $detail")
            }
            return if (text.isBlank()) JSONObject() else JSONObject(text)
        } finally {
            conn.disconnect()
        }
    }
}
