package com.example.navbridge

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class DeviceDiffTest {
    @Test
    fun reportsNewModifiedConflictingTracksAndNewSessions() {
        val device = JSONObject()
        device.put("tracks", JSONArray()
            .put(JSONObject()
                .put("track_id", "t-new").put("local_slot", 1)
                .put("name", "New").put("version", 1).put("revision_id", "r1"))
            .put(JSONObject()
                .put("track_id", "t-mod").put("local_slot", 2)
                .put("name", "Modified").put("version", 4).put("revision_id", "r4"))
            .put(JSONObject()
                .put("track_id", "t-dirty").put("local_slot", 3)
                .put("name", "Dirty").put("version", 3).put("revision_id", "r3")))
        device.put("sessions", JSONArray()
            .put(JSONObject()
                .put("session_id", "s-new").put("local_slot", 0)
                .put("track_id", "t-new").put("track_name", "New")
                .put("start", 1795000000).put("laps", 3)
                .put("telemetry_available", true)))
        val local = JSONObject()
        local.put("tracks", JSONArray()
            .put(JSONObject()
                .put("track_id", "t-mod").put("version", 3).put("revision_id", "r3"))
            .put(JSONObject()
                .put("track_id", "t-dirty").put("version", 2).put("revision_id", "r2")))
        local.put("sessions", JSONArray())
        val diff = DeviceDiff.compare(device, local, setOf("t-dirty"))
        assertEquals(4, diff.size)
        val byId = diff.associateBy { it.track?.trackId ?: it.session?.sessionId }
        assertFalse(byId["t-new"]!!.conflict)
        assertFalse(byId["t-mod"]!!.conflict)
        assertTrue(byId["t-dirty"]!!.conflict)
        assertEquals("session", byId["s-new"]!!.type)
    }

    @Test
    fun identicalManifestsProduceNoDiff() {
        val manifest = JSONObject()
        manifest.put("tracks", JSONArray().put(JSONObject()
            .put("track_id", "t1").put("version", 2).put("revision_id", "r2")))
        manifest.put("sessions", JSONArray().put(JSONObject()
            .put("session_id", "s1").put("start", 1795000000)
            .put("telemetry", JSONObject().put("available", true))))
        assertEquals(0, DeviceDiff.compare(manifest, manifest, emptySet()).size)
    }
}
