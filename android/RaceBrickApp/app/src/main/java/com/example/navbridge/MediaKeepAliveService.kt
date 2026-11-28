package com.example.navbridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.core.content.ContextCompat

/** Keeps the process and BLE bridge alive while the UI is in the background,
 *  so hardware volume/media commands still run. */
class MediaKeepAliveService : Service() {
    private val main = Handler(Looper.getMainLooper())
    private val mediaListener = object : BleSession.Listener {
        override fun onStatus(message: String) = Unit
        override fun onSessionChanged(active: Boolean) = Unit
        override fun onReady() = Unit
        override fun onLine(line: String) {
            MediaBridge.handleLine(this@MediaKeepAliveService, line)
        }
    }
    private val pump = object : Runnable {
        override fun run() {
            MediaBridge.sendState(this@MediaKeepAliveService)
            main.postDelayed(this, 500L)
        }
    }

    companion object {
        private const val CHANNEL = "racebrick_media"
        private const val NOTIFICATION_ID = 1002

        fun start(context: Context) {
            runCatching {
                ContextCompat.startForegroundService(
                    context, Intent(context, MediaKeepAliveService::class.java)
                )
            }
        }

        fun stop(context: Context) {
            runCatching { context.stopService(Intent(context, MediaKeepAliveService::class.java)) }
        }
    }

    override fun onCreate() {
        super.onCreate()
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL, "RaceBrick 蓝牙后台", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "保持蓝牙媒体控制和音量同步"
                    setShowBadge(false)
                }
            )
        }
        BleSession.bind(mediaListener)
        main.removeCallbacks(pump)
        main.post(pump)
    }

    override fun onDestroy() {
        main.removeCallbacks(pump)
        BleSession.unbind(mediaListener)
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val content = Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }
        val pi = PendingIntent.getActivity(
            this, 0, content,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val n: Notification = Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_mylocation)
            .setContentTitle("RaceBrick 媒体控制")
            .setContentText("蓝牙媒体按键和音量在后台保持可用")
            .setContentIntent(pi)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
