package com.example.navbridge

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.provider.Settings
import android.view.KeyEvent
import androidx.core.app.NotificationManagerCompat

/** Controls whichever media session is currently active on the phone. */
object MediaControl {
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

    private fun key(context: Context, code: Int): Boolean {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return false
        return runCatching {
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
            true
        }.getOrDefault(false)
    }
}
