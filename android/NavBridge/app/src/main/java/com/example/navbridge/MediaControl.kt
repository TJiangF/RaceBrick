package com.example.navbridge

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.SystemClock
import android.provider.Settings
import android.view.KeyEvent
import androidx.core.app.NotificationManagerCompat

/** Controls whichever media session is currently active on the phone. */
object MediaControl {
    data class Progress(val positionMs: Long, val durationMs: Long, val playing: Boolean)
    data class Meta(val title: String, val artist: String, val album: String, val durationMs: Long)
    fun hasAccess(context: Context): Boolean =
        NotificationManagerCompat.getEnabledListenerPackages(context).contains(context.packageName)

    fun openSettings(context: Context) {
        runCatching {
            context.startActivity(
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
    }

    private fun controller(context: Context): MediaController? {
        if (!hasAccess(context)) return null
        val msm = context.getSystemService(Context.MEDIA_SESSION_SERVICE) as? MediaSessionManager
            ?: return null
        val component = ComponentName(context, MediaNotificationListener::class.java)
        val sessions = runCatching { msm.getActiveSessions(component) }.getOrNull() ?: return null
        val candidates = sessions.filter { it.packageName != context.packageName }
        return candidates.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: candidates.firstOrNull { it.playbackState?.state == PlaybackState.STATE_BUFFERING }
            ?: candidates.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PAUSED }
            ?: candidates.firstOrNull()
    }

    fun isPlaying(context: Context): Boolean? {
        val c = controller(context) ?: return null
        return c.playbackState?.state == PlaybackState.STATE_PLAYING
    }

    fun metadata(context: Context): Meta? {
        val c = controller(context) ?: return null
        val m = c.metadata ?: return null
        val title = m.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE)
            ?.takeIf { it.isNotBlank() }
            ?: m.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty()
        val artist = m.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: m.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST).orEmpty()
        val album = m.getString(MediaMetadata.METADATA_KEY_ALBUM).orEmpty()
        val duration = m.getLong(MediaMetadata.METADATA_KEY_DURATION)
        return Meta(title, artist, album, duration)
    }

    fun albumArt(context: Context): android.graphics.Bitmap? {
        val c = controller(context)
        val m = c?.metadata
        return m?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: m?.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: MediaNotificationArt.bitmap
    }

    fun progress(context: Context): Progress? {
        val c = controller(context) ?: return null
        val ps = c.playbackState ?: return null
        val duration = c.metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        if (duration <= 0L) return null
        var pos = ps.position.coerceAtLeast(0L)
        val playing = ps.state == PlaybackState.STATE_PLAYING
        if (playing && ps.lastPositionUpdateTime > 0L) {
            val elapsed = (SystemClock.elapsedRealtime() - ps.lastPositionUpdateTime).coerceAtLeast(0L)
            pos += (elapsed * ps.playbackSpeed).toLong().coerceAtLeast(0L)
        }
        return Progress(pos.coerceIn(0L, duration), duration, playing)
    }

    fun toggle(context: Context): Boolean {
        val c = controller(context)
        if (c != null) {
            return runCatching {
                if (c.playbackState?.state == PlaybackState.STATE_PLAYING) {
                    c.transportControls.pause()
                } else {
                    c.transportControls.play()
                }
                true
            }.getOrDefault(false)
        }
        return key(context, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
    }

    fun next(context: Context): Boolean {
        val c = controller(context)
        if (c != null) {
            return runCatching { c.transportControls.skipToNext(); true }.getOrDefault(false)
        }
        return key(context, KeyEvent.KEYCODE_MEDIA_NEXT)
    }

    fun prev(context: Context): Boolean {
        val c = controller(context)
        if (c != null) {
            return runCatching { c.transportControls.skipToPrevious(); true }.getOrDefault(false)
        }
        return key(context, KeyEvent.KEYCODE_MEDIA_PREVIOUS)
    }

    /** Current music-stream volume, independent of notification-listener access. */
    fun volume(context: Context): Pair<Int, Int>? {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return null
        return runCatching {
            val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val cur = am.getStreamVolume(AudioManager.STREAM_MUSIC)
            if (max > 0) cur.coerceIn(0, max) to max else null
        }.getOrNull()
    }

    fun volumeUp(context: Context): Boolean {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
        return runCatching {
            am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE,
                AudioManager.FLAG_SHOW_UI)
            true
        }.getOrElse {
            runCatching {
                am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP))
                am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_VOLUME_UP))
                true
            }.getOrDefault(false)
        }
    }

    fun volumeDown(context: Context): Boolean {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
        return runCatching {
            am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER,
                AudioManager.FLAG_SHOW_UI)
            true
        }.getOrElse {
            runCatching {
                am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_DOWN))
                am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_VOLUME_DOWN))
                true
            }.getOrDefault(false)
        }
    }

    private fun key(context: Context, code: Int): Boolean {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
        return runCatching {
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
            true
        }.getOrDefault(false)
    }
}
