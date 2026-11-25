package com.example.navbridge

import android.content.Context
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.charset.StandardCharsets
import java.util.zip.GZIPOutputStream

object LocalCache {
    private const val PREFS = "race_local_cache"
    private const val KEY_UID = "uid"
    private const val KEY_VERSION = "version"
    private const val KEY_LOCAL_VERSION = "local_version"
    private const val KEY_DIRTY = "dirty"
    private const val KEY_SAVED_AT = "saved_at"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun dir(context: Context): File =
        File(context.filesDir, "race_cache").apply { mkdirs() }

    private fun snapshotFile(context: Context, uid: String): File =
        File(dir(context), "$uid.snapshot.json")

    private fun uploadFile(context: Context, uid: String): File =
        File(dir(context), "$uid.upload.json")

    fun hasCache(context: Context): Boolean {
        val uid = prefs(context).getString(KEY_UID, null) ?: return false
        return snapshotFile(context, uid).isFile
    }

    fun uid(context: Context): String? = prefs(context).getString(KEY_UID, null)

    fun dataVersion(context: Context): Int = prefs(context).getInt(KEY_VERSION, 0)

    fun localVersion(context: Context): Int = prefs(context).getInt(KEY_LOCAL_VERSION, 0)

    fun dirty(context: Context): Boolean = prefs(context).getBoolean(KEY_DIRTY, false)

    fun savedAt(context: Context): Long = prefs(context).getLong(KEY_SAVED_AT, 0L)

    fun markDirty(context: Context, uid: String) {
        val p = prefs(context)
        if (p.getString(KEY_UID, null) != uid) return
        val next = maxOf(localVersion(context) + 1, dataVersion(context) + 1)
        p.edit().putInt(KEY_LOCAL_VERSION, next).putBoolean(KEY_DIRTY, true).apply()
    }

    fun saveFromSync(
        context: Context,
        uid: String,
        upload: JSONObject,
        snapshot: JSONObject,
        version: Int,
    ) {
        write(snapshotFile(context, uid), snapshot)
        write(uploadFile(context, uid), upload)
        saveMeta(context, uid, version, dirty = false)
    }

    fun saveCloudSnapshot(context: Context, uid: String, snapshot: JSONObject, version: Int) {
        val upload = buildUploadFromSnapshot(snapshot)
        write(snapshotFile(context, uid), snapshot)
        if (upload != null) write(uploadFile(context, uid), upload)
        saveMeta(context, uid, version, dirty = false)
    }

    private fun saveMeta(context: Context, uid: String, version: Int, dirty: Boolean) {
        prefs(context).edit()
            .putString(KEY_UID, uid)
            .putInt(KEY_VERSION, version)
            .putInt(KEY_LOCAL_VERSION, version)
            .putBoolean(KEY_DIRTY, dirty)
            .putLong(KEY_SAVED_AT, System.currentTimeMillis() / 1000L)
            .apply()
    }

    fun loadSnapshot(context: Context): JSONObject? {
        val uid = uid(context) ?: return null
        val f = snapshotFile(context, uid)
        if (!f.isFile) return null
        return runCatching { JSONObject(f.readText(StandardCharsets.UTF_8)) }.getOrNull()
    }

    fun loadUpload(context: Context): JSONObject? {
        val uid = uid(context) ?: return null
        val f = uploadFile(context, uid)
        if (!f.isFile) return null
        return runCatching { JSONObject(f.readText(StandardCharsets.UTF_8)) }.getOrNull()
    }

    fun applyCloudVersion(context: Context, uid: String, version: Int) {
        val p = prefs(context)
        if (p.getString(KEY_UID, null) != uid) return
        p.edit().putInt(KEY_VERSION, version).putInt(KEY_LOCAL_VERSION, version)
            .putBoolean(KEY_DIRTY, false).apply()
    }

    private fun write(file: File, obj: JSONObject) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(obj.toString(), StandardCharsets.UTF_8)
        if (!tmp.renameTo(file)) {
            file.delete()
            tmp.renameTo(file)
        }
    }

    private fun buildUploadFromSnapshot(snapshot: JSONObject): JSONObject? = runCatching {
        val body = JSONObject().apply {
            put("replace", true)
            put("since", 0)
            put("tracks", JSONArray())
            put("sessions", JSONArray())
            put("logs", JSONArray())
        }
        val tIn = snapshot.optJSONArray("tracks") ?: JSONArray()
        for (i in 0 until tIn.length()) {
            val t = tIn.getJSONObject(i)
            body.getJSONArray("tracks").put(JSONObject().apply {
                put("local_id", t.optInt("local_id"))
                put("payload", t.optJSONObject("payload") ?: JSONObject())
            })
        }
        val sIn = snapshot.optJSONArray("sessions") ?: JSONArray()
        for (i in 0 until sIn.length()) {
            val s = sIn.getJSONObject(i)
            val payload = s.optJSONObject("payload") ?: JSONObject()
            val item = JSONObject().apply {
                put("local_slot", s.optInt("local_slot"))
                put("payload", payload)
            }
            val csv = s.optString("csv", "")
            if (csv.isNotEmpty()) {
                val gz = ByteArrayOutputStream()
                GZIPOutputStream(gz).use { it.write(csv.toByteArray(StandardCharsets.UTF_8)) }
                item.put("csv_gzip_b64", Base64.encodeToString(gz.toByteArray(), Base64.NO_WRAP))
            }
            body.getJSONArray("sessions").put(item)
        }
        val lIn = snapshot.optJSONArray("logs") ?: JSONArray()
        for (i in 0 until lIn.length()) {
            val l = lIn.getJSONObject(i)
            body.getJSONArray("logs").put(JSONObject().apply {
                put("local_index", l.optInt("local_index"))
                put("payload", l.optJSONObject("payload") ?: JSONObject())
            })
        }
        body
    }.getOrNull()
}
