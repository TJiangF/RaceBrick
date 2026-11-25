package com.example.navbridge

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.UUID

class SyncLocalDb(context: Context) : SQLiteOpenHelper(context, "racebrick_sync.db", null, 2) {
    private val app = context.applicationContext

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL("""CREATE TABLE meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )""")
        db.execSQL("""CREATE TABLE entities (
            uuid TEXT PRIMARY KEY,
            type TEXT NOT NULL,
            local_key TEXT NOT NULL,
            parent_uuid TEXT,
            name TEXT NOT NULL,
            revision INTEGER NOT NULL,
            hash TEXT NOT NULL,
            created_at INTEGER,
            updated_at INTEGER,
            deleted INTEGER NOT NULL DEFAULT 0,
            dirty_device INTEGER NOT NULL DEFAULT 0,
            dirty_cloud INTEGER NOT NULL DEFAULT 0,
            origin TEXT NOT NULL,
            payload TEXT NOT NULL,
            blob_path TEXT,
            UNIQUE(type, local_key)
        )""")
        db.execSQL("CREATE INDEX idx_entities_type ON entities(type, deleted)")
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        if (oldVersion < 2) {
            db.execSQL("ALTER TABLE entities ADD COLUMN dirty_device INTEGER NOT NULL DEFAULT 0")
            db.execSQL("ALTER TABLE entities ADD COLUMN dirty_cloud INTEGER NOT NULL DEFAULT 0")
        }
    }

    data class Summary(
        val tracks: Int,
        val sessions: Int,
        val logs: Int,
        val dirtyDevice: Boolean,
        val dirtyCloud: Boolean,
        val datasetRevision: Int,
        val updatedAt: Long,
        val uid: String?,
    )

    private fun meta(db: SQLiteDatabase, key: String): String? =
        db.rawQuery("SELECT value FROM meta WHERE key=?", arrayOf(key)).use {
            if (it.moveToFirst()) it.getString(0) else null
        }

    private fun setMeta(db: SQLiteDatabase, key: String, value: String) {
        db.insertWithOnConflict("meta", null, ContentValues().apply {
            put("key", key); put("value", value)
        }, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun summary(): Summary {
        val db = readableDatabase
        fun count(type: String): Int =
            db.rawQuery("SELECT COUNT(*) FROM entities WHERE type=? AND deleted=0", arrayOf(type)).use {
                if (it.moveToFirst()) it.getInt(0) else 0
            }
        return Summary(
            tracks = count("track"),
            sessions = count("session"),
            logs = count("race_log"),
            dirtyDevice = meta(db, "dirty_device") == "1",
            dirtyCloud = meta(db, "dirty_cloud") == "1",
            datasetRevision = meta(db, "dataset_revision")?.toIntOrNull() ?: 0,
            updatedAt = meta(db, "updated_at")?.toLongOrNull() ?: 0L,
            uid = meta(db, "device_uid"),
        )
    }

    fun importDeviceSnapshot(uid: String, body: JSONObject): Summary {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        val blobDir = File(app.filesDir, "sync_blobs").apply { mkdirs() }
        db.beginTransaction()
        try {
            val seen = HashSet<String>()
            fun upsert(type: String, key: String, name: String, payload: JSONObject,
                       parent: String?, blob: File? = null) {
                val identity = "$type:$key"
                seen += identity
                val payloadText = payload.toString()
                val digest = sha256(payloadText.toByteArray() + (blob?.readBytes() ?: ByteArray(0)))
                var oldUuid: String? = null
                var oldHash: String? = null
                var oldRevision = 0
                var oldCreated = now
                db.rawQuery(
                    "SELECT uuid,hash,revision,created_at FROM entities WHERE type=? AND local_key=?",
                    arrayOf(type, key),
                ).use {
                    if (it.moveToFirst()) {
                        oldUuid = it.getString(0)
                        oldHash = it.getString(1)
                        oldRevision = it.getInt(2)
                        oldCreated = it.getLong(3)
                    }
                }
                val uuid = oldUuid ?: UUID.randomUUID().toString()
                val revision = if (oldUuid == null || oldHash != digest) oldRevision + 1 else oldRevision
                val created = oldCreated
                val values = ContentValues().apply {
                    put("uuid", uuid); put("type", type); put("local_key", key)
                    if (parent != null) put("parent_uuid", parent) else putNull("parent_uuid")
                    put("name", name)
                    put("revision", revision); put("hash", digest)
                    put("created_at", created); put("updated_at", now)
                    put("deleted", 0); put("origin", "device")
                    put("dirty_device", 0); put("dirty_cloud", 0)
                    put("payload", payloadText)
                    if (blob != null) put("blob_path", blob.absolutePath) else putNull("blob_path")
                }
                db.insertWithOnConflict("entities", null, values, SQLiteDatabase.CONFLICT_REPLACE)
            }

            val trackIds = HashMap<Int, String>()
            val tracks = body.optJSONArray("tracks") ?: JSONArray()
            for (i in 0 until tracks.length()) {
                val item = tracks.optJSONObject(i) ?: continue
                val key = item.optInt("local_id").toString()
                val payload = item.optJSONObject("payload") ?: JSONObject()
                upsert("track", key, payload.optString("name"), payload, null)
                db.rawQuery("SELECT uuid FROM entities WHERE type='track' AND local_key=?", arrayOf(key)).use {
                    if (it.moveToFirst()) trackIds[item.optInt("local_id")] = it.getString(0)
                }
            }

            val sessions = body.optJSONArray("sessions") ?: JSONArray()
            for (i in 0 until sessions.length()) {
                val item = sessions.optJSONObject(i) ?: continue
                val key = item.optInt("local_slot").toString()
                val payload = item.optJSONObject("payload") ?: JSONObject()
                val b64 = item.optString("csv_gzip_b64", "")
                var blob: File? = null
                if (b64.isNotEmpty()) {
                    blob = File(blobDir, "session-$key-${System.nanoTime()}.gz")
                    blob.writeBytes(android.util.Base64.decode(b64, android.util.Base64.NO_WRAP))
                }
                upsert("session", key, payload.optString("track_name"), payload,
                    trackIds[payload.optInt("track", -1)], blob)
            }

            val logs = body.optJSONArray("logs") ?: JSONArray()
            for (i in 0 until logs.length()) {
                val item = logs.optJSONObject(i) ?: continue
                val key = item.optInt("local_index").toString()
                val payload = item.optJSONObject("payload") ?: JSONObject()
                upsert("race_log", key, payload.optString("track_name"), payload, null)
            }

            val old = db.rawQuery("SELECT uuid,type,local_key FROM entities WHERE deleted=0", null).use {
                val list = ArrayList<Triple<String, String, String>>()
                while (it.moveToNext()) list += Triple(it.getString(0), it.getString(1), it.getString(2))
                list
            }
            for ((uuid, type, key) in old) {
                if ("$type:$key" !in seen) {
                    db.execSQL("UPDATE entities SET deleted=1,revision=revision+1,updated_at=? WHERE uuid=?",
                        arrayOf(now, uuid))
                }
            }

            val version = (meta(db, "dataset_revision")?.toIntOrNull() ?: 0) + 1
            setMeta(db, "device_uid", uid)
            setMeta(db, "dataset_revision", version.toString())
            setMeta(db, "updated_at", now.toString())
            setMeta(db, "dirty_device", "0")
            setMeta(db, "dirty_cloud", "1")
            setMeta(db, "baseline_device_hash", sha256(body.toString().toByteArray()))
            setMeta(db, "baseline_device_at", now.toString())
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
        return summary()
    }


    data class LocalEntity(
        val uuid: String,
        val localKey: String,
        val name: String,
        val payload: JSONObject,
        val blobPath: String?,
        val dirtyDevice: Boolean,
        val dirtyCloud: Boolean,
    )

    fun entities(type: String): List<LocalEntity> {
        val out = ArrayList<LocalEntity>()
        readableDatabase.rawQuery(
            "SELECT uuid,local_key,name,payload,blob_path,dirty_device,dirty_cloud FROM entities WHERE type=? AND deleted=0 ORDER BY CAST(local_key AS INTEGER)",
            arrayOf(type),
        ).use { c ->
            while (c.moveToNext()) {
                out += LocalEntity(
                    uuid = c.getString(0),
                    localKey = c.getString(1),
                    name = c.getString(2),
                    payload = runCatching { JSONObject(c.getString(3)) }.getOrElse { JSONObject() },
                    blobPath = c.getString(4),
                    dirtyDevice = c.getInt(5) != 0,
                    dirtyCloud = c.getInt(6) != 0,
                )
            }
        }
        return out
    }

    fun trackByLocalKey(localKey: Int): LocalEntity? =
        entities("track").firstOrNull { it.localKey == localKey.toString() }



    fun updateTrack(uuid: String, name: String, startF: Double, startWidth: Double,
                    startOffset: Double, reverse: Boolean): Boolean {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        val row = db.rawQuery("SELECT payload,revision FROM entities WHERE uuid=? AND type='track'", arrayOf(uuid)).use {
            if (it.moveToFirst()) it.getString(0) to it.getInt(1) else null
        } ?: return false
        val payload = runCatching { JSONObject(row.first) }.getOrDefault(JSONObject()).apply {
            put("name", name.take(48))
            put("start_f", startF.coerceIn(0.0, 0.9999))
            put("start_width", startWidth.coerceIn(1.0, 50.0))
            put("start_offset", startOffset.coerceIn(-50.0, 50.0))
            put("reverse", reverse)
        }
        val text = payload.toString()
        val hash = sha256(text.toByteArray())
        val values = ContentValues().apply {
            put("name", name.take(48)); put("payload", text); put("hash", hash)
            put("revision", row.second + 1); put("updated_at", now)
            put("dirty_device", 1); put("dirty_cloud", 1)
        }
        db.update("entities", values, "uuid=?", arrayOf(uuid))
        setMeta(db, "dirty_device", "1")
        setMeta(db, "dirty_cloud", "1")
        setMeta(db, "updated_at", now.toString())
        setMeta(db, "dataset_revision", ((meta(db, "dataset_revision")?.toIntOrNull() ?: 0) + 1).toString())
        return true
    }

    fun dirtyTracks(): List<LocalEntity> = entities("track").filter { it.dirtyDevice }

    fun markTrackDeviceClean(uuid: String) {
        val db = writableDatabase
        db.execSQL("UPDATE entities SET dirty_device=0 WHERE uuid=?", arrayOf(uuid))
        val left = db.rawQuery("SELECT COUNT(*) FROM entities WHERE dirty_device=1 AND deleted=0", null).use {
            if (it.moveToFirst()) it.getInt(0) else 0
        }
        setMeta(db, "dirty_device", if (left > 0) "1" else "0")
    }

    fun markAllCloudClean() {
        val db = writableDatabase
        db.execSQL("UPDATE entities SET dirty_cloud=0")
        setMeta(db, "dirty_cloud", "0")
    }

    companion object {
        fun sha256(bytes: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(bytes)
                .joinToString("") { "%02x".format(it) }
    }
}
