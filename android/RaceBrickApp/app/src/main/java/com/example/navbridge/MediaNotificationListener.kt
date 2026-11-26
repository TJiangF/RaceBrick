package com.example.navbridge

import android.app.Notification
import android.graphics.Bitmap
import android.os.Build
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

/** Permission handle for MediaSessionManager, plus a fallback cover source:
 *  some music apps put album art only in the MediaStyle notification. */
object MediaNotificationArt {
    @Volatile var bitmap: Bitmap? = null
    @Volatile var packageName: String = ""
    @Volatile var title: String = ""
    @Volatile var text: String = ""
    @Volatile var subText: String = ""
    @Volatile var bigText: String = ""
    /** Best-effort current lyric line exposed by the notification. */
    @Volatile var lyric: String = ""
}

class MediaNotificationListener : NotificationListenerService() {
    override fun onListenerConnected() {
        activeNotifications?.forEach { onNotificationPosted(it) }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val n = sbn.notification ?: return
        val isMedia = n.category == Notification.CATEGORY_TRANSPORT ||
            n.extras?.containsKey(Notification.EXTRA_MEDIA_SESSION) == true
        if (!isMedia) return
        val e = n.extras
        val title = e?.getCharSequence(Notification.EXTRA_TITLE)?.toString()?.trim().orEmpty()
        val text = e?.getCharSequence(Notification.EXTRA_TEXT)?.toString()?.trim().orEmpty()
        val sub = e?.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString()?.trim().orEmpty()
        val big = e?.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()?.trim().orEmpty()
        MediaNotificationArt.title = title
        MediaNotificationArt.text = text
        MediaNotificationArt.subText = sub
        MediaNotificationArt.bigText = big
        // NetEase/QQ Music often put the current lyric in subText or bigText
        // when "Bluetooth lyrics" is enabled. Keep only a changed candidate;
        // MainActivity filters out title/artist/album before displaying it.
        MediaNotificationArt.lyric = when {
            sub.isNotBlank() && sub != title && sub != text -> sub
            big.isNotBlank() && big != title && big != text -> big
            text.isNotBlank() && text != title -> text
            else -> ""
        }
        MediaNotificationArt.packageName = sbn.packageName
        val bmp = notificationBitmap(n) ?: return
        MediaNotificationArt.bitmap = bmp
    }

    private fun notificationBitmap(n: Notification): Bitmap? {
        val e = n.extras ?: return null
        return runCatching {
            if (Build.VERSION.SDK_INT >= 33) {
                e.getParcelable(Notification.EXTRA_LARGE_ICON, Bitmap::class.java)
                    ?: e.getParcelable(Notification.EXTRA_PICTURE, Bitmap::class.java)
            } else {
                @Suppress("DEPRECATION")
                (e.getParcelable(Notification.EXTRA_LARGE_ICON) as? Bitmap)
                    ?: @Suppress("DEPRECATION") (e.getParcelable(Notification.EXTRA_PICTURE) as? Bitmap)
            }
        }.getOrNull()
    }
}
