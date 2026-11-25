package com.example.navbridge

import android.content.Context
import java.security.MessageDigest

object SatelliteUploadCache {
    private const val PREFS = "satellite_uploads"

    fun fingerprint(sat: TrackSatellite): String {
        val md = MessageDigest.getInstance("SHA-256")
        md.update(sat.pal16)
        val bounds = "${sat.bounds.left},${sat.bounds.top},${sat.bounds.right},${sat.bounds.bottom}"
        md.update(bounds.toByteArray(Charsets.UTF_8))
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    fun needsUpload(context: Context, uid: String, trackId: Int, sat: TrackSatellite): Boolean {
        val saved = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(key(uid, trackId), null)
        return saved != fingerprint(sat)
    }

    fun markUploaded(context: Context, uid: String, trackId: Int, sat: TrackSatellite) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(key(uid, trackId), fingerprint(sat))
            .apply()
    }

    private fun key(uid: String, trackId: Int) = "$uid:$trackId"
}
