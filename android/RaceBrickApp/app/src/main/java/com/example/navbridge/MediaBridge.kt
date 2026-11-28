package com.example.navbridge

import android.content.Context
import android.net.Uri

/**
 * Media handling that survives leaving MainActivity. When MainActivity is
 * visible it owns the richer media/lyrics pipeline; otherwise the foreground
 * service uses this bridge for basic playback state and transport controls.
 */
object MediaBridge {
    @Volatile var mainActive: Boolean = false
    private var lastTitle: String? = null
    private var lastArtist: String? = null
    private var lastAlbum: String? = null

    fun setUiActive(active: Boolean) {
        mainActive = active
    }

    fun handleLine(context: Context, line: String) {
        if (mainActive || !line.startsWith("M,")) return
        val cmd = line.removePrefix("M,").trim()
        when (cmd) {
            "TOGGLE" -> MediaControl.toggle(context)
            "NEXT" -> MediaControl.next(context)
            "PREV" -> MediaControl.prev(context)
            "VOLUP" -> MediaControl.volumeUp(context)
            "VOLDOWN" -> MediaControl.volumeDown(context)
        }
    }

    fun sendState(context: Context) {
        if (mainActive || !BleSession.verified) return
        val playing = MediaControl.isPlaying(context) ?: false
        BleSession.send("M,STATE,${if (playing) 1 else 0}")
        MediaControl.progress(context)?.let {
            BleSession.send("M,POS,${it.positionMs},${it.durationMs}")
        }
        MediaControl.volume(context)?.let {
            BleSession.send("M,VOL,${it.first},${it.second}")
        }
        val meta = MediaControl.metadata(context)
        if (meta == null) {
            if (lastTitle != null || lastArtist != null || lastAlbum != null) {
                lastTitle = null
                lastArtist = null
                lastAlbum = null
                BleSession.send("M,TITLE,")
                BleSession.send("M,ARTIST,")
                BleSession.send("M,ALBUM,")
            }
            return
        }
        val title = meta.title.trim()
        val artist = meta.artist.trim()
        val album = meta.album.trim()
        if (title != lastTitle) {
            lastTitle = title
            BleSession.send("M,TITLE,${Uri.encode(clip(title, 96))}")
        }
        if (artist != lastArtist) {
            lastArtist = artist
            BleSession.send("M,ARTIST,${Uri.encode(clip(artist, 64))}")
        }
        if (album != lastAlbum) {
            lastAlbum = album
            BleSession.send("M,ALBUM,${Uri.encode(clip(album, 64))}")
        }
    }

    private fun clip(value: String, max: Int): String =
        if (value.length <= max) value else value.substring(0, max)
}
