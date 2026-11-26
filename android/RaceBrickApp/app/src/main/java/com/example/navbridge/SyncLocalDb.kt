package com.example.navbridge

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.UUID

class SyncLocalDb(context: Context) : SQLiteOpenHelper(context, "racebrick_sync.db", null, 4) {
    private val app = context.applicationContext

    override fun onCreate(db: SQLiteDatabase) {
        createMeta(db)
        createTypedTables(db)
    }

    override fun onOpen(db: SQLiteDatabase) {
        super.onOpen(db)
        if (!db.isReadOnly) {
            dedupeTracks(db, System.currentTimeMillis() / 1000L)
        }
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        if (oldVersion < 2) {
            ensureLegacyDirtyColumns(db)
        }
        createMeta(db)
        createTypedTables(db)
        if (oldVersion < 3) migrateLegacyEntities(db)
        if (oldVersion < 4) {
            if (tableExists(db, "sessions") && "device_hash" !in columns(db, "sessions")) {
                db.execSQL("ALTER TABLE sessions ADD COLUMN device_hash TEXT")
            }
        }
    }

    private fun createMeta(db: SQLiteDatabase) {
        db.execSQL("""CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )""")
    }

    private fun createTypedTables(db: SQLiteDatabase) {
        db.execSQL("""CREATE TABLE IF NOT EXISTS tracks (
            track_uuid TEXT PRIMARY KEY,
            local_slot INTEGER,
            name TEXT NOT NULL,
            version INTEGER NOT NULL DEFAULT 1,
            revision_id TEXT,
            parent_revision_id TEXT,
            payload TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            created_at INTEGER,
            updated_at INTEGER,
            deleted INTEGER NOT NULL DEFAULT 0,
            dirty_device INTEGER NOT NULL DEFAULT 0,
            dirty_cloud INTEGER NOT NULL DEFAULT 0,
            origin TEXT NOT NULL DEFAULT 'device'
        )""")
        db.execSQL("CREATE INDEX IF NOT EXISTS idx_tracks_slot ON tracks(local_slot, deleted)")
        db.execSQL("""CREATE TABLE IF NOT EXISTS track_revisions (
            track_uuid TEXT NOT NULL,
            revision_id TEXT NOT NULL,
            version INTEGER NOT NULL,
            parent_revision_id TEXT,
            payload TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            created_at INTEGER,
            created_by TEXT NOT NULL DEFAULT 'device',
            PRIMARY KEY(track_uuid, revision_id)
        )""")
        db.execSQL("""CREATE TABLE IF NOT EXISTS track_drafts (
            track_uuid TEXT PRIMARY KEY,
            base_revision_id TEXT,
            base_data TEXT NOT NULL,
            draft_data TEXT NOT NULL,
            modified_fields TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at INTEGER,
            updated_at INTEGER
        )""")
        db.execSQL("""CREATE TABLE IF NOT EXISTS sessions (
            session_uuid TEXT PRIMARY KEY,
            local_slot INTEGER,
            track_uuid TEXT,
            track_revision_id TEXT,
            track_version INTEGER,
            device_id TEXT,
            start_time INTEGER,
            end_time INTEGER,
            session_type TEXT NOT NULL DEFAULT 'race',
            total_laps INTEGER NOT NULL DEFAULT 0,
            best_lap_ms INTEGER,
            metadata TEXT NOT NULL DEFAULT '{}',
            raw_available INTEGER NOT NULL DEFAULT 0,
            raw_format INTEGER NOT NULL DEFAULT 16,
            sample_size INTEGER NOT NULL DEFAULT 16,
            payload TEXT NOT NULL,
            content_hash TEXT,
            laps_hash TEXT,
            telemetry_hash TEXT,
            device_hash TEXT,
            imported_at INTEGER,
            updated_at INTEGER,
            deleted INTEGER NOT NULL DEFAULT 0
        )""")
        db.execSQL("CREATE INDEX IF NOT EXISTS idx_sessions_track ON sessions(track_uuid, deleted)")
        db.execSQL("""CREATE TABLE IF NOT EXISTS lap_records (
            lap_id TEXT PRIMARY KEY,
            session_uuid TEXT NOT NULL,
            lap_number INTEGER NOT NULL,
            lap_time_ms INTEGER NOT NULL,
            sector_times_ms TEXT NOT NULL DEFAULT '[]',
            start_time INTEGER,
            end_time INTEGER,
            best_lap INTEGER NOT NULL DEFAULT 0,
            telemetry_reference TEXT NOT NULL DEFAULT '{}',
            content_hash TEXT,
            deleted INTEGER NOT NULL DEFAULT 0
        )""")
        db.execSQL("CREATE INDEX IF NOT EXISTS idx_laps_session ON lap_records(session_uuid, lap_number)")
        db.execSQL("""CREATE TABLE IF NOT EXISTS telemetry_chunks (
            chunk_id TEXT PRIMARY KEY,
            session_uuid TEXT NOT NULL,
            seq INTEGER NOT NULL,
            byte_offset INTEGER NOT NULL DEFAULT 0,
            size INTEGER NOT NULL DEFAULT 0,
            sha256 TEXT NOT NULL,
            local_path TEXT NOT NULL,
            uploaded_cloud INTEGER NOT NULL DEFAULT 0,
            imported_at INTEGER
        )""")
        db.execSQL("CREATE INDEX IF NOT EXISTS idx_chunks_session ON telemetry_chunks(session_uuid, seq)")
        db.execSQL("""CREATE TABLE IF NOT EXISTS sync_metadata (
            object_id TEXT NOT NULL,
            object_type TEXT NOT NULL,
            local_version INTEGER,
            device_version INTEGER,
            cloud_version INTEGER,
            local_hash TEXT,
            device_hash TEXT,
            cloud_hash TEXT,
            sync_status TEXT NOT NULL DEFAULT 'clean',
            last_synced_at INTEGER,
            PRIMARY KEY(object_id, object_type)
        )""")
        db.execSQL("""CREATE TABLE IF NOT EXISTS sync_tasks (
            task_id TEXT PRIMARY KEY,
            task_type TEXT NOT NULL,
            object_id TEXT,
            source TEXT NOT NULL,
            destination TEXT NOT NULL,
            status TEXT NOT NULL,
            retry_count INTEGER NOT NULL DEFAULT 0,
            idempotency_key TEXT,
            created_at INTEGER,
            updated_at INTEGER,
            error_message TEXT
        )""")
        db.execSQL("""CREATE TABLE IF NOT EXISTS cloud_sync_state (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )""")
    }

    private fun ensureLegacyDirtyColumns(db: SQLiteDatabase) {
        if (!tableExists(db, "entities")) return
        val cols = columns(db, "entities")
        if ("dirty_device" !in cols) {
            db.execSQL("ALTER TABLE entities ADD COLUMN dirty_device INTEGER NOT NULL DEFAULT 0")
        }
        if ("dirty_cloud" !in cols) {
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

    data class LocalEntity(
        val uuid: String,
        val localKey: String,
        val name: String,
        val payload: JSONObject,
        val blobPath: String?,
        val dirtyDevice: Boolean,
        val dirtyCloud: Boolean,
        val version: Int = 1,
        val revisionId: String = "",
    )

    private fun meta(db: SQLiteDatabase, key: String): String? =
        db.rawQuery("SELECT value FROM meta WHERE key=?", arrayOf(key)).use {
            if (it.moveToFirst()) it.getString(0) else null
        }

    private fun setMeta(db: SQLiteDatabase, key: String, value: String) {
        db.insertWithOnConflict("meta", null, ContentValues().apply {
            put("key", key)
            put("value", value)
        }, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun summary(): Summary {
        val db = readableDatabase
        fun count(table: String): Int =
            db.rawQuery("SELECT COUNT(*) FROM $table WHERE deleted=0", null).use {
                if (it.moveToFirst()) it.getInt(0) else 0
            }
        return Summary(
            tracks = count("tracks"),
            sessions = count("sessions"),
            logs = 0,
            dirtyDevice = meta(db, "dirty_device") == "1",
            dirtyCloud = meta(db, "dirty_cloud") == "1",
            datasetRevision = meta(db, "dataset_revision")?.toIntOrNull() ?: 0,
            updatedAt = meta(db, "updated_at")?.toLongOrNull() ?: 0L,
            uid = meta(db, "device_uid"),
        )
    }

    fun importDeviceSnapshot(uid: String, body: JSONObject, replace: Boolean = true): Summary {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        val blobDir = File(app.filesDir, "sync_blobs").apply { mkdirs() }
        db.beginTransaction()
        try {
            val seenTracks = HashSet<String>()
            val seenSessions = HashSet<String>()

            val tracks = body.optJSONArray("tracks") ?: JSONArray()
            for (i in 0 until tracks.length()) {
                val item = tracks.optJSONObject(i) ?: continue
                val localId = item.optInt("local_id", -1)
                val payload = item.optJSONObject("payload") ?: JSONObject()
                val existingBySlot = trackUuidBySlot(db, localId)
                val trackUuid = payload.optString("uuid", "")
                    .ifBlank { existingBySlot ?: stableUuid("track:$uid:$localId") }
                seenTracks += trackUuid
                val version = payload.optInt("version", 1).coerceAtLeast(1)
                val revisionId = payload.optString("revision_id", "")
                    .ifBlank { stableUuid("trev:$trackUuid:$version") }
                val parent = payload.optString("parent_revision_id", "").ifBlank { null }
                val name = payload.optString("name", "")
                val hash = sha256(payload.toString().toByteArray())
                val existing = queryTrack(db, trackUuid)
                if (existing != null && existing.dirtyDevice && existing.hash != hash) {
                    /* Preserve a local working edit. Phase 3 conflict UI will resolve it. */
                    continue
                }
                val values = ContentValues().apply {
                    put("track_uuid", trackUuid)
                    put("local_slot", localId)
                    put("name", name)
                    put("version", version)
                    put("revision_id", revisionId)
                    if (parent != null) put("parent_revision_id", parent) else putNull("parent_revision_id")
                    put("payload", payload.toString())
                    put("content_hash", hash)
                    put("created_at", existing?.createdAt ?: now)
                    put("updated_at", now)
                    put("deleted", 0)
                    put("dirty_device", 0)
                    put("dirty_cloud", 0)
                    put("origin", "device")
                }
                db.insertWithOnConflict("tracks", null, values, SQLiteDatabase.CONFLICT_REPLACE)
                val revValues = ContentValues().apply {
                    put("track_uuid", trackUuid)
                    put("revision_id", revisionId)
                    put("version", version)
                    if (parent != null) put("parent_revision_id", parent) else putNull("parent_revision_id")
                    put("payload", payload.toString())
                    put("content_hash", hash)
                    put("created_at", now)
                    put("created_by", "device")
                }
                db.insertWithOnConflict("track_revisions", null, revValues, SQLiteDatabase.CONFLICT_IGNORE)
            }

            val sessions = body.optJSONArray("sessions") ?: JSONArray()
            for (i in 0 until sessions.length()) {
                val item = sessions.optJSONObject(i) ?: continue
                val localSlot = item.optInt("local_slot", -1)
                val payload = item.optJSONObject("payload") ?: JSONObject()
                val b64 = item.optString("csv_gzip_b64", "")
                var blob: File? = null
                var blobBytes: ByteArray? = null
                if (b64.isNotEmpty()) {
                    val bytes = Base64.decode(b64, Base64.NO_WRAP)
                    blobBytes = bytes
                    blob = File(blobDir, "session-$localSlot-${System.nanoTime()}.gz")
                    blob.writeBytes(bytes)
                }
                val sessionUuid = payload.optString("session_uuid", "")
                    .ifBlank { stableUuid("session:$uid:$localSlot:${sha256(payload.toString().toByteArray())}") }
                seenSessions += sessionUuid
                val trackSlot = payload.optInt("track", -1)
                val trackUuid = payload.optString("track_uuid", "")
                    .ifBlank { trackUuidBySlot(db, trackSlot) }
                val trackRevisionId = payload.optString("track_revision_id", "")
                    .ifBlank { trackRevisionId(db, trackUuid) }
                val sampleSize = payload.optInt("sample_size", 16).coerceAtLeast(16)
                val start = payload.optLong("start", 0L)
                val durationMs = payload.optLong("duration_ms", 0L)
                val end = payload.optLong("end", if (start > 0) start + durationMs / 1000L else 0L)
                val lapTimes = payload.optJSONArray("lap_times") ?: JSONArray()
                val totalLaps = payload.optInt("laps", lapTimes.length())
                val bestMs = (0 until lapTimes.length())
                    .mapNotNull { idx -> lapTimes.optDouble(idx, 0.0).takeIf { it > 0.0 } }
                    .minOrNull()?.times(1000.0)?.toLong()
                val hashInput = payload.toString().toByteArray() + (blobBytes ?: ByteArray(0))
                val contentHash = sha256(hashInput)
                val lapsHash = sha256(lapTimes.toString().toByteArray())
                val telemetryHash = blobBytes?.let { sha256(it) }
                val metaText = payload.optJSONObject("metadata")?.toString() ?: "{}"
                val sessionValues = ContentValues().apply {
                    put("session_uuid", sessionUuid)
                    put("local_slot", localSlot)
                    if (trackUuid != null) put("track_uuid", trackUuid) else putNull("track_uuid")
                    if (trackRevisionId != null) put("track_revision_id", trackRevisionId) else putNull("track_revision_id")
                    put("track_version", payload.optInt("track_version", 0))
                    put("device_id", uid)
                    put("start_time", start)
                    put("end_time", end)
                    put("session_type", payload.optString("session_type", "race"))
                    put("total_laps", totalLaps)
                    if (bestMs != null) put("best_lap_ms", bestMs) else putNull("best_lap_ms")
                    put("metadata", metaText)
                    put("raw_available", if (blob != null) 1 else 0)
                    put("raw_format", sampleSize)
                    put("sample_size", sampleSize)
                    put("payload", payload.toString())
                    put("content_hash", contentHash)
                    put("laps_hash", lapsHash)
                    if (telemetryHash != null) put("telemetry_hash", telemetryHash) else putNull("telemetry_hash")
                    put("device_hash", payload.optString("content_hash", ""))
                    put("imported_at", now)
                    put("deleted", 0)
                }
                db.insertWithOnConflict("sessions", null, sessionValues, SQLiteDatabase.CONFLICT_REPLACE)

                db.delete("lap_records", "session_uuid=?", arrayOf(sessionUuid))
                val suppliedLapIds = payload.optJSONArray("lap_ids")
                for (idx in 0 until lapTimes.length()) {
                    val lapMs = (lapTimes.optDouble(idx, 0.0) * 1000.0).toLong()
                    if (lapMs <= 0) continue
                    val suppliedId = suppliedLapIds?.optString(idx, "").orEmpty()
                    val lapId = suppliedId.ifBlank { stableUuid("lap:$sessionUuid:${idx + 1}") }
                    val isBest = bestMs != null && lapMs == bestMs
                    db.insertWithOnConflict("lap_records", null, ContentValues().apply {
                        put("lap_id", lapId)
                        put("session_uuid", sessionUuid)
                        put("lap_number", idx + 1)
                        put("lap_time_ms", lapMs)
                        put("sector_times_ms", "[]")
                        put("best_lap", if (isBest) 1 else 0)
                        put("telemetry_reference", "{}")
                        put("content_hash", sha256("$sessionUuid:$lapId:$lapMs".toByteArray()))
                        put("deleted", 0)
                    }, SQLiteDatabase.CONFLICT_REPLACE)
                }

                if (blob != null && blobBytes != null) {
                    db.delete("telemetry_chunks", "session_uuid=?", arrayOf(sessionUuid))
                    val chunkId = stableUuid("chunk:$sessionUuid:0")
                    db.insertWithOnConflict("telemetry_chunks", null, ContentValues().apply {
                        put("chunk_id", chunkId)
                        put("session_uuid", sessionUuid)
                        put("seq", 0)
                        put("byte_offset", 0)
                        put("size", blobBytes.size)
                        put("sha256", sha256(blobBytes))
                        put("local_path", blob.absolutePath)
                        put("uploaded_cloud", 0)
                        put("imported_at", now)
                    }, SQLiteDatabase.CONFLICT_REPLACE)
                }
            }

            if (replace) {
                markMissing(db, "tracks", "track_uuid", seenTracks, now)
                markMissing(db, "sessions", "session_uuid", seenSessions, now)
            }
            dedupeTracks(db, now)

            val version = (meta(db, "dataset_revision")?.toIntOrNull() ?: 0) + 1
            setMeta(db, "device_uid", uid)
            setMeta(db, "dataset_revision", version.toString())
            setMeta(db, "updated_at", now.toString())
            if (replace) {
                setMeta(db, "dirty_device", "0")
                setMeta(db, "baseline_device_hash", sha256(body.toString().toByteArray()))
                setMeta(db, "baseline_device_at", now.toString())
            }
            setMeta(db, "dirty_cloud", "1")
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
        return summary()
    }

    private data class TrackRow(
        val hash: String,
        val createdAt: Long,
        val dirtyDevice: Boolean,
    )

    private fun queryTrack(db: SQLiteDatabase, uuid: String): TrackRow? =
        db.rawQuery(
            "SELECT content_hash,created_at,dirty_device FROM tracks WHERE track_uuid=?",
            arrayOf(uuid),
        ).use {
            if (it.moveToFirst()) TrackRow(
                hash = it.getString(0),
                createdAt = it.getLong(1),
                dirtyDevice = it.getInt(2) != 0,
            ) else null
        }

    private fun trackUuidBySlot(db: SQLiteDatabase, slot: Int): String? =
        db.rawQuery(
            "SELECT track_uuid FROM tracks WHERE local_slot=? AND deleted=0 ORDER BY updated_at DESC LIMIT 1",
            arrayOf(slot.toString()),
        ).use { if (it.moveToFirst()) it.getString(0) else null }

    private fun trackRevisionId(db: SQLiteDatabase, trackUuid: String?): String? {
        if (trackUuid.isNullOrBlank()) return null
        return db.rawQuery(
            "SELECT revision_id FROM tracks WHERE track_uuid=? AND deleted=0",
            arrayOf(trackUuid),
        ).use { if (it.moveToFirst()) it.getString(0) else null }
    }

    private fun trackFingerprint(payload: JSONObject): String {
        val pts = payload.optJSONArray("points")?.toString().orEmpty()
        val sec = payload.optJSONArray("sections")?.toString().orEmpty()
        return listOf(
            payload.optString("name", ""),
            payload.optDouble("length_m", 0.0).toString(),
            payload.optDouble("width", 0.0).toString(),
            payload.optDouble("start_width", 0.0).toString(),
            payload.optDouble("start_f", 0.0).toString(),
            payload.optDouble("start_offset", 0.0).toString(),
            payload.optBoolean("reverse", false).toString(),
            payload.optBoolean("geo", false).toString(),
            payload.optDouble("glat0", 0.0).toString(),
            payload.optDouble("glon0", 0.0).toString(),
            payload.optDouble("nscale", 0.0).toString(),
            payload.optDouble("nox", 0.0).toString(),
            payload.optDouble("noy", 0.0).toString(),
            sec, pts,
        ).joinToString("|")
    }

    private fun dedupeTracks(db: SQLiteDatabase, now: Long) {
        val rows = db.rawQuery(
            """SELECT track_uuid,local_slot,payload,version,revision_id,updated_at
               FROM tracks WHERE deleted=0 ORDER BY updated_at DESC""",
            null,
        ).use { c ->
            val out = ArrayList<Array<Any>>()
            while (c.moveToNext()) {
                out += arrayOf(
                    c.getString(0), c.getInt(1), c.getString(2), c.getInt(3),
                    c.getString(4).orEmpty(), c.getLong(5),
                )
            }
            out
        }
        if (rows.size < 2) return
        val groups = HashMap<String, MutableList<Array<Any>>>()
        for (r in rows) {
            val p = runCatching { JSONObject(r[2] as String) }.getOrNull() ?: continue
            groups.getOrPut(trackFingerprint(p)) { ArrayList() }.add(r)
        }
        for (group in groups.values) {
            if (group.size < 2) continue
            val keep = group.maxWithOrNull(
                compareBy<Array<Any>> { it[3] as Int }.thenBy { it[5] as Long }
            ) ?: continue
            val keepUuid = keep[0] as String
            val keepSlot = keep[1] as Int
            val keepRev = keep[4] as String
            for (r in group) {
                val uuid = r[0] as String
                if (uuid == keepUuid) continue
                db.execSQL(
                    "UPDATE tracks SET deleted=1,dirty_device=0,dirty_cloud=1,updated_at=? WHERE track_uuid=?",
                    arrayOf(now, uuid),
                )
                val sessionRows = db.rawQuery(
                    "SELECT session_uuid,payload FROM sessions WHERE track_uuid=?",
                    arrayOf(uuid),
                ).use { c ->
                    val out = ArrayList<Pair<String, String>>()
                    while (c.moveToNext()) out += c.getString(0) to c.getString(1)
                    out
                }
                for ((sessionId, payloadText) in sessionRows) {
                    val sp = runCatching { JSONObject(payloadText) }.getOrDefault(JSONObject()).apply {
                        put("track", keepSlot)
                        if (keepUuid.isNotBlank()) put("track_uuid", keepUuid)
                        if (keepRev.isNotBlank()) put("track_revision_id", keepRev)
                    }
                    val text = sp.toString()
                    db.update("sessions", ContentValues().apply {
                        put("track_uuid", keepUuid)
                        put("track_revision_id", keepRev)
                        put("payload", text)
                        put("content_hash", sha256(text.toByteArray()))
                        put("updated_at", now)
                    }, "session_uuid=?", arrayOf(sessionId))
                }
            }
        }
    }

    private fun markMissing(
        db: SQLiteDatabase,
        table: String,
        key: String,
        present: Set<String>,
        now: Long,
    ) {
        if (present.isEmpty()) {
            db.execSQL("UPDATE $table SET deleted=1, updated_at=? WHERE deleted=0",
                arrayOf<Any?>(now))
            return
        }
        val placeholders = present.joinToString(",") { "?" }
        val args = ArrayList<Any?>()
        args.add(now)
        args.addAll(present)
        db.execSQL(
            "UPDATE $table SET deleted=1, updated_at=? WHERE deleted=0 AND $key NOT IN ($placeholders)",
            args.toTypedArray(),
        )
    }

    fun entities(type: String): List<LocalEntity> {
        if (type == "track") return tracks()
        if (type == "session") return sessions()
        return emptyList()
    }

    fun tracks(): List<LocalEntity> {
        val out = ArrayList<LocalEntity>()
        readableDatabase.rawQuery(
            """SELECT track_uuid,local_slot,name,payload,dirty_device,dirty_cloud,
                      version,revision_id FROM tracks WHERE deleted=0 ORDER BY name, track_uuid""",
            null,
        ).use { c ->
            while (c.moveToNext()) {
                out += LocalEntity(
                    uuid = c.getString(0),
                    localKey = c.getInt(1).toString(),
                    name = c.getString(2),
                    payload = runCatching { JSONObject(c.getString(3)) }.getOrElse { JSONObject() },
                    blobPath = null,
                    dirtyDevice = c.getInt(4) != 0,
                    dirtyCloud = c.getInt(5) != 0,
                    version = c.getInt(6),
                    revisionId = c.getString(7).orEmpty(),
                )
            }
        }
        return out
    }

    fun sessions(): List<LocalEntity> {
        val out = ArrayList<LocalEntity>()
        readableDatabase.rawQuery(
            """SELECT s.session_uuid,s.local_slot,s.payload,
                      (SELECT local_path FROM telemetry_chunks c
                       WHERE c.session_uuid=s.session_uuid ORDER BY c.seq LIMIT 1)
               FROM sessions s WHERE s.deleted=0 ORDER BY s.start_time DESC""",
            null,
        ).use { c ->
            while (c.moveToNext()) {
                out += LocalEntity(
                    uuid = c.getString(0),
                    localKey = c.getInt(1).toString(),
                    name = runCatching { JSONObject(c.getString(2)).optString("track_name") }.getOrElse { "" },
                    payload = runCatching { JSONObject(c.getString(2)) }.getOrElse { JSONObject() },
                    blobPath = c.getString(3),
                    dirtyDevice = false,
                    dirtyCloud = false,
                )
            }
        }
        return out
    }

    fun trackByLocalKey(localKey: Int): LocalEntity? =
        tracks().firstOrNull { it.localKey == localKey.toString() }

    fun trackByUuid(uuid: String): LocalEntity? = tracks().firstOrNull { it.uuid == uuid }

    fun updateTrack(uuid: String, name: String, startF: Double, startWidth: Double,
                    startOffset: Double, reverse: Boolean): Boolean {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        val row = db.rawQuery(
            "SELECT payload FROM tracks WHERE track_uuid=? AND deleted=0",
            arrayOf(uuid),
        ).use { if (it.moveToFirst()) it.getString(0) else null } ?: return false
        val payload = runCatching { JSONObject(row) }.getOrDefault(JSONObject()).apply {
            put("name", name.take(48))
            put("start_f", startF.coerceIn(0.0, 0.9999))
            put("start_width", startWidth.coerceIn(1.0, 50.0))
            put("start_offset", startOffset.coerceIn(-50.0, 50.0))
            put("reverse", reverse)
        }
        val text = payload.toString()
        val hash = sha256(text.toByteArray())
        db.update("tracks", ContentValues().apply {
            put("name", name.take(48))
            put("payload", text)
            put("content_hash", hash)
            put("updated_at", now)
            put("dirty_device", 1)
            put("dirty_cloud", 1)
        }, "track_uuid=?", arrayOf(uuid))
        setMeta(db, "dirty_device", "1")
        setMeta(db, "dirty_cloud", "1")
        setMeta(db, "updated_at", now.toString())
        setMeta(db, "dataset_revision",
            ((meta(db, "dataset_revision")?.toIntOrNull() ?: 0) + 1).toString())
        return true
    }

    fun manifest(): JSONObject {
        val db = readableDatabase
        val root = JSONObject()
            .put("protocol", 2)
            .put("device_id", meta(db, "device_uid") ?: "")
            .put("generated_at", System.currentTimeMillis() / 1000L)
        val tracksOut = JSONArray()
        for (t in tracks()) {
            tracksOut.put(JSONObject()
                .put("track_id", t.uuid)
                .put("local_slot", t.localKey.toIntOrNull() ?: -1)
                .put("name", t.name)
                .put("version", t.payload.optInt("version", 1))
                .put("revision_id", t.payload.optString("revision_id", ""))
                .put("parent_revision_id", t.payload.optString("parent_revision_id", ""))
                .put("content_hash", sha256(t.payload.toString().toByteArray())))
        }
        val sessionsOut = JSONArray()
        for (s in sessions()) {
            val p = s.payload
            val sessionUuid = s.uuid
            val chunk = db.rawQuery(
                "SELECT size,sha256 FROM telemetry_chunks WHERE session_uuid=? ORDER BY seq LIMIT 1",
                arrayOf(sessionUuid),
            ).use { c ->
                if (c.moveToFirst()) (c.getLong(0) to c.getString(1)) else null
            }
            sessionsOut.put(JSONObject()
                .put("session_id", sessionUuid)
                .put("local_slot", s.localKey.toIntOrNull() ?: -1)
                .put("track_id", p.optString("track_uuid", ""))
                .put("track_revision_id", p.optString("track_revision_id", ""))
                .put("track_version", p.optInt("track_version", 0))
                .put("track_name", p.optString("track_name", ""))
                .put("start", p.optLong("start", 0L))
                .put("end", p.optLong("end", 0L))
                .put("laps", p.optInt("laps", 0))
                .put("samples", p.optLong("samples", 0L))
                .put("sample_size", p.optInt("sample_size", 16))
                .put("content_hash", p.optString("content_hash", ""))
                .put("telemetry", JSONObject()
                    .put("available", s.blobPath != null)
                    .put("size_bytes", chunk?.first ?: 0L)
                    .put("sha256", chunk?.second ?: "")))
        }
        root.put("tracks", tracksOut)
        root.put("sessions", sessionsOut)
        return root
    }

    fun dirtyTracks(): List<LocalEntity> = tracks().filter { it.dirtyDevice }

    fun markTrackDeviceClean(uuid: String) {
        val db = writableDatabase
        db.execSQL("UPDATE tracks SET dirty_device=0 WHERE track_uuid=?", arrayOf(uuid))
        val left = db.rawQuery("SELECT COUNT(*) FROM tracks WHERE dirty_device=1 AND deleted=0", null).use {
            if (it.moveToFirst()) it.getInt(0) else 0
        }
        setMeta(db, "dirty_device", if (left > 0) "1" else "0")
    }

    fun applyTrackCommit(
        uuid: String,
        newSlot: Int,
        name: String,
        startF: Double,
        startWidth: Double,
        startOffset: Double,
        reverse: Boolean,
        version: Int,
        revisionId: String,
    ): Boolean {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        val row = db.rawQuery(
            "SELECT payload FROM tracks WHERE track_uuid=? AND deleted=0",
            arrayOf(uuid),
        ).use { if (it.moveToFirst()) it.getString(0) else null } ?: return false
        val payload = runCatching { JSONObject(row) }.getOrDefault(JSONObject()).apply {
            put("name", name.take(48))
            put("start_f", startF.coerceIn(0.0, 0.9999))
            put("start_width", startWidth.coerceIn(1.0, 50.0))
            put("start_offset", startOffset.coerceIn(-50.0, 50.0))
            put("reverse", reverse)
            put("version", version)
            put("revision_id", revisionId)
        }
        val text = payload.toString()
        val hash = sha256(text.toByteArray())
        db.update("tracks", ContentValues().apply {
            put("local_slot", newSlot)
            put("name", name.take(48))
            put("version", version)
            put("revision_id", revisionId)
            put("payload", text)
            put("content_hash", hash)
            put("updated_at", now)
            put("dirty_device", 0)
            put("dirty_cloud", 1)
        }, "track_uuid=?", arrayOf(uuid))
        db.insertWithOnConflict("track_revisions", null, ContentValues().apply {
            put("track_uuid", uuid)
            put("revision_id", revisionId)
            put("version", version)
            putNull("parent_revision_id")
            put("payload", text)
            put("content_hash", hash)
            put("created_at", now)
            put("created_by", "device")
        }, SQLiteDatabase.CONFLICT_IGNORE)
        setMeta(db, "updated_at", now.toString())
        setMeta(db, "dirty_cloud", "1")
        val left = db.rawQuery("SELECT COUNT(*) FROM tracks WHERE dirty_device=1 AND deleted=0", null).use {
            if (it.moveToFirst()) it.getInt(0) else 0
        }
        setMeta(db, "dirty_device", if (left > 0) "1" else "0")
        return true
    }

    fun markTrackDeleted(uuid: String) {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        db.execSQL("UPDATE tracks SET deleted=1,updated_at=?,dirty_device=0,dirty_cloud=1 WHERE track_uuid=?",
            arrayOf(now, uuid))
        setMeta(db, "updated_at", now.toString())
        setMeta(db, "dirty_cloud", "1")
    }

    fun markSessionDeleted(uuid: String) {
        val db = writableDatabase
        val now = System.currentTimeMillis() / 1000L
        db.execSQL("UPDATE sessions SET deleted=1,updated_at=? WHERE session_uuid=?",
            arrayOf(now, uuid))
        db.execSQL("UPDATE lap_records SET deleted=1 WHERE session_uuid=?", arrayOf(uuid))
        setMeta(db, "updated_at", now.toString())
        setMeta(db, "dirty_cloud", "1")
    }

    fun markAllCloudClean() {
        val db = writableDatabase
        db.execSQL("UPDATE tracks SET dirty_cloud=0")
        db.execSQL("UPDATE telemetry_chunks SET uploaded_cloud=1")
        setMeta(db, "dirty_cloud", "0")
    }

    private fun migrateLegacyEntities(db: SQLiteDatabase) {
        if (!tableExists(db, "entities")) return
        val cols = columns(db, "entities")
        val dirtyDevice = if ("dirty_device" in cols) "dirty_device" else "0"
        val dirtyCloud = if ("dirty_cloud" in cols) "dirty_cloud" else "0"
        val now = System.currentTimeMillis() / 1000L
        db.rawQuery(
            """SELECT uuid,type,local_key,parent_uuid,name,revision,hash,created_at,
                      updated_at,origin,payload,blob_path,$dirtyDevice,$dirtyCloud
               FROM entities WHERE deleted=0""",
            null,
        ).use { c ->
            while (c.moveToNext()) {
                val uuid = c.getString(0)
                val type = c.getString(1)
                val localKey = c.getInt(2)
                val parentUuid = c.getString(3)
                val name = c.getString(4)
                val revision = c.getInt(5)
                val hash = c.getString(6)
                val created = c.getLong(7)
                val updated = c.getLong(8)
                val origin = c.getString(9)
                val payloadText = c.getString(10)
                val blobPath = c.getString(11)
                val dirtyDev = c.getInt(12) != 0
                val dirtyCloud = c.getInt(13) != 0
                if (type == "track") {
                    val version = revision.coerceAtLeast(1)
                    val revisionId = stableUuid("trev:$uuid:$version")
                    db.insertWithOnConflict("tracks", null, ContentValues().apply {
                        put("track_uuid", uuid)
                        put("local_slot", localKey)
                        put("name", name)
                        put("version", version)
                        put("revision_id", revisionId)
                        putNull("parent_revision_id")
                        put("payload", payloadText)
                        put("content_hash", hash)
                        put("created_at", created)
                        put("updated_at", updated)
                        put("deleted", 0)
                        put("dirty_device", if (dirtyDev) 1 else 0)
                        put("dirty_cloud", if (dirtyCloud) 1 else 0)
                        put("origin", origin)
                    }, SQLiteDatabase.CONFLICT_IGNORE)
                    db.insertWithOnConflict("track_revisions", null, ContentValues().apply {
                        put("track_uuid", uuid)
                        put("revision_id", revisionId)
                        put("version", version)
                        putNull("parent_revision_id")
                        put("payload", payloadText)
                        put("content_hash", hash)
                        put("created_at", created)
                        put("created_by", origin)
                    }, SQLiteDatabase.CONFLICT_IGNORE)
                } else if (type == "session") {
                    val payload = runCatching { JSONObject(payloadText) }.getOrElse { JSONObject() }
                    val start = payload.optLong("start", 0L)
                    val duration = payload.optLong("duration_ms", 0L)
                    val lapTimes = payload.optJSONArray("lap_times") ?: JSONArray()
                    val bestMs = (0 until lapTimes.length())
                        .mapNotNull { idx -> lapTimes.optDouble(idx, 0.0).takeIf { it > 0.0 } }
                        .minOrNull()?.times(1000.0)?.toLong()
                    val blob = blobPath?.let { File(it) }
                    val telemetryHash = blob?.takeIf { it.isFile }?.readBytes()?.let { sha256(it) }
                    db.insertWithOnConflict("sessions", null, ContentValues().apply {
                        put("session_uuid", uuid)
                        put("local_slot", localKey)
                        if (parentUuid != null) put("track_uuid", parentUuid) else putNull("track_uuid")
                        put("track_version", payload.optInt("track_version", 0))
                        put("start_time", start)
                        put("end_time", payload.optLong("end", start + duration / 1000L))
                        put("session_type", payload.optString("session_type", "race"))
                        put("total_laps", payload.optInt("laps", lapTimes.length()))
                        if (bestMs != null) put("best_lap_ms", bestMs) else putNull("best_lap_ms")
                        put("metadata", "{}")
                        put("raw_available", if (blob?.isFile == true) 1 else 0)
                        put("raw_format", 16)
                        put("sample_size", 16)
                        put("payload", payloadText)
                        put("content_hash", hash)
                        put("laps_hash", sha256(lapTimes.toString().toByteArray()))
                        if (telemetryHash != null) put("telemetry_hash", telemetryHash) else putNull("telemetry_hash")
                        put("device_hash", payload.optString("content_hash", ""))
                        put("imported_at", now)
                        put("deleted", 0)
                    }, SQLiteDatabase.CONFLICT_IGNORE)
                    db.delete("lap_records", "session_uuid=?", arrayOf(uuid))
                    for (idx in 0 until lapTimes.length()) {
                        val lapMs = (lapTimes.optDouble(idx, 0.0) * 1000.0).toLong()
                        if (lapMs <= 0) continue
                        val lapId = stableUuid("lap:$uuid:${idx + 1}")
                        db.insertWithOnConflict("lap_records", null, ContentValues().apply {
                            put("lap_id", lapId)
                            put("session_uuid", uuid)
                            put("lap_number", idx + 1)
                            put("lap_time_ms", lapMs)
                            put("sector_times_ms", "[]")
                            put("best_lap", if (bestMs != null && lapMs == bestMs) 1 else 0)
                            put("telemetry_reference", "{}")
                            put("content_hash", sha256("$uuid:$lapId:$lapMs".toByteArray()))
                        }, SQLiteDatabase.CONFLICT_REPLACE)
                    }
                    if (blob?.isFile == true) {
                        val chunkId = stableUuid("chunk:$uuid:0")
                        val bytes = blob.readBytes()
                        db.insertWithOnConflict("telemetry_chunks", null, ContentValues().apply {
                            put("chunk_id", chunkId)
                            put("session_uuid", uuid)
                            put("seq", 0)
                            put("byte_offset", 0)
                            put("size", bytes.size)
                            put("sha256", sha256(bytes))
                            put("local_path", blob.absolutePath)
                            put("imported_at", now)
                        }, SQLiteDatabase.CONFLICT_IGNORE)
                    }
                }
            }
        }
        setMeta(db, "dirty_device", if (meta(db, "dirty_device") == "1") "1" else "0")
        setMeta(db, "dirty_cloud", if (meta(db, "dirty_cloud") == "1") "1" else "0")
    }

    private fun tableExists(db: SQLiteDatabase, name: String): Boolean =
        db.rawQuery(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            arrayOf(name),
        ).use { it.moveToFirst() }

    private fun columns(db: SQLiteDatabase, table: String): Set<String> {
        val out = HashSet<String>()
        db.rawQuery("PRAGMA table_info($table)", null).use { c ->
            while (c.moveToNext()) out += c.getString(1)
        }
        return out
    }

    companion object {
        fun sha256(bytes: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(bytes)
                .joinToString("") { "%02x".format(it) }

        private fun stableUuid(text: String): String =
            UUID.nameUUIDFromBytes(text.toByteArray()).toString()
    }
}
