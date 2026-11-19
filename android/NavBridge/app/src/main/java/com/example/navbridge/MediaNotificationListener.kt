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
        val bmp = notificationBitmap(n) ?: return
        MediaNotificationArt.bitmap = bmp
        MediaNotificationArt.packageName = sbn.packageName
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
