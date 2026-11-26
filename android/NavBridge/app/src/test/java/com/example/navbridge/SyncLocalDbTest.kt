package com.example.navbridge

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.util.zip.GZIPOutputStream

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class SyncLocalDbTest {
    private lateinit var context: Context

    @Before
    fun setUp() {
        context = ApplicationProvider.getApplicationContext()
        context.deleteDatabase("racebrick_sync.db")
    }

    @After
    fun tearDown() {
        context.deleteDatabase("racebrick_sync.db")
    }

    @Test
    fun migratesLegacyEntitiesToTypedTables() {
        val db = context.openOrCreateDatabase("racebrick_sync.db", Context.MODE_PRIVATE, null)
        db.execSQL("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
        db.execSQL(
            """CREATE TABLE entities (
                uuid TEXT PRIMARY KEY, type TEXT NOT NULL, local_key TEXT NOT NULL,
                parent_uuid TEXT, name TEXT NOT NULL, revision INTEGER NOT NULL,
                hash TEXT NOT NULL, created_at INTEGER, updated_at INTEGER,
                deleted INTEGER NOT NULL DEFAULT 0,
                dirty_device INTEGER NOT NULL DEFAULT 0,
                dirty_cloud INTEGER NOT NULL DEFAULT 0,
                origin TEXT NOT NULL, payload TEXT NOT NULL, blob_path TEXT,
                UNIQUE(type, local_key)
            )"""
        )
        db.execSQL("INSERT INTO meta(key,value) VALUES('device_uid','esp32s3-test')")
        val trackId = "11111111-1111-4111-8111-111111111111"
        db.execSQL(
            "INSERT INTO entities(uuid,type,local_key,name,revision,hash,origin,payload) VALUES(?,?,?,?,?,?,?,?)",
            arrayOf(trackId, "track", "3", "Old Track", 2, "legacy-hash", "device",
                """{"id":3,"name":"Old Track","points":[[0.1,0.1],[0.9,0.1],[0.9,0.9]]}""")
        )
        val sessionId = "22222222-2222-4222-8222-222222222222"
        db.execSQL(
            "INSERT INTO entities(uuid,type,local_key,parent_uuid,name,revision,hash,origin,payload) VALUES(?,?,?,?,?,?,?,?,?)",
            arrayOf(sessionId, "session", "0", trackId, "Old Track", 1, "legacy-session-hash", "device",
                """{"track":3,"track_name":"Old Track","start":1795000000,"duration_ms":10000,"laps":2,"lap_times":[2.0,3.0]}""")
        )
        db.version = 2
        db.close()

        val local = SyncLocalDb(context)
        val summary = local.summary()
        assertEquals(1, summary.tracks)
        assertEquals(1, summary.sessions)
        assertEquals(0, summary.logs)
        assertEquals("esp32s3-test", summary.uid)
        val track = local.trackByLocalKey(3)
        assertNotNull(track)
        assertEquals(trackId, track!!.uuid)
        assertEquals("Old Track", track.name)
        assertEquals(1, local.sessions().size)
        val laps = local.readableDatabase.rawQuery(
            "SELECT lap_number,lap_time_ms,best_lap FROM lap_records WHERE session_uuid=? ORDER BY lap_number",
            arrayOf(sessionId),
        ).use { c ->
            val out = ArrayList<Triple<Int, Long, Int>>()
            while (c.moveToNext()) out += Triple(c.getInt(0), c.getLong(1), c.getInt(2))
            out
        }
        assertEquals(listOf(1, 2), laps.map { it.first })
        assertEquals(listOf(2000L, 3000L), laps.map { it.second })
        assertEquals(1, laps.count { it.third != 0 })
    }

    @Test
    fun importsStableTrackAndSessionIdentity() {
        val trackId = "33333333-3333-4333-8333-333333333333"
        val revisionId = "44444444-4444-4444-8444-444444444444"
        val sessionId = "55555555-5555-4555-8555-555555555555"
        val trackPayload = JSONObject()
            .put("id", 7)
            .put("name", "Imported Track")
            .put("uuid", trackId)
            .put("version", 5)
            .put("revision_id", revisionId)
            .put("points", JSONArray().put(JSONArray().put(0.1).put(0.1)))
        val track = JSONObject().put("local_id", 7).put("payload", trackPayload)

        val csv = "Format,3\nSession title,\"Imported Track\"\n" +
            (0 until 6).joinToString("") { i ->
                "179500000$i,$i,1,${i / 10.0},31.0,121.0,20.0\n"
            }
        val compressed = ByteArrayOutputStream()
        GZIPOutputStream(compressed).use { it.write(csv.toByteArray()) }
        val sessionPayload = JSONObject()
            .put("slot", 0)
            .put("track", 7)
            .put("track_name", "Imported Track")
            .put("start", 1795000000)
            .put("end", 1795000010)
            .put("duration_ms", 10000)
            .put("samples", 6)
            .put("sample_size", 20)
            .put("laps", 1)
            .put("lap_times", JSONArray().put(2.0))
            .put("session_uuid", sessionId)
            .put("track_uuid", trackId)
            .put("track_revision_id", revisionId)
            .put("track_version", 5)
        val session = JSONObject()
            .put("local_slot", 0)
            .put("payload", sessionPayload)
            .put("csv_gzip_b64", android.util.Base64.encodeToString(compressed.toByteArray(), android.util.Base64.NO_WRAP))

        val body = JSONObject()
            .put("tracks", JSONArray().put(track))
            .put("sessions", JSONArray().put(session))
            .put("logs", JSONArray())
        val local = SyncLocalDb(context)
        val summary = local.importDeviceSnapshot("esp32s3-test", body)
        assertEquals(1, summary.tracks)
        assertEquals(1, summary.sessions)
        val imported = local.trackByUuid(trackId)
        assertNotNull(imported)
        assertEquals("5", imported!!.payload.optString("version"))
        val sessionEntity = local.sessions().first()
        assertEquals(sessionId, sessionEntity.uuid)
        assertEquals(1, local.readableDatabase.rawQuery(
            "SELECT COUNT(*) FROM telemetry_chunks WHERE session_uuid=?", arrayOf(sessionId),
        ).use { if (it.moveToFirst()) it.getInt(0) else 0 })
        assertTrue(local.summary().updatedAt > 0)
    }
}
