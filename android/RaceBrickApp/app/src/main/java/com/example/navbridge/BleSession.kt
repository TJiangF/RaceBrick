package com.example.navbridge

import android.content.Context
import android.os.Handler
import android.os.Looper

/**
 * Process-wide BLE connection. The client is owned here, not by an Activity,
 * so navigating Home/NAV/WIFI does not tear the connection down.
 */
object BleSession {
    interface Listener {
        fun onStatus(message: String)
        fun onSessionChanged(active: Boolean)
        fun onReady()
        fun onLine(line: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private var appContext: Context? = null
    private var client: BleClient? = null
    private val listeners = LinkedHashSet<Listener>()

    @Volatile private var lastStatus: String = "未连接 RaceBrick"

    val initialized: Boolean get() = client != null
    val connected: Boolean get() = client?.connected == true
    val verified: Boolean get() = client?.verified == true
    val status: String get() = lastStatus

    fun ensure(context: Context): BleClient {
        val existing = client
        if (existing != null) return existing
        val app = context.applicationContext
        appContext = app
        val created = BleClient(
            app,
            onStatus = { msg ->
                lastStatus = msg
                main.post { listeners.toList().forEach { it.onStatus(msg) } }
            },
            onReady = {
                main.post {
                    val ctx = appContext
                    if (ctx != null) MediaKeepAliveService.start(ctx)
                    listeners.toList().forEach { it.onReady() }
                }
            },
            onLine = { line ->
                main.post { listeners.toList().forEach { it.onLine(line) } }
            },
            onSessionChanged = { active ->
                main.post { listeners.toList().forEach { it.onSessionChanged(active) } }
            },
        )
        client = created
        return created
    }

    fun bind(target: Listener) {
        listeners += target
        target.onStatus(lastStatus)
        if (verified) target.onSessionChanged(true)
    }

    fun unbind(target: Listener) {
        listeners -= target
    }

    fun connect() {
        client?.connect()
    }

    fun disconnect() {
        client?.disconnect()
        lastStatus = "未连接 RaceBrick"
        listeners.toList().forEach { it.onStatus(lastStatus) }
        appContext?.let { MediaKeepAliveService.stop(it) }
    }

    fun send(text: String) {
        client?.send(text)
    }

    fun sendBulk(type: Int, id: Int, data: ByteArray): Boolean =
        client?.sendBulk(type, id, data) ?: false

    fun bulkIdle(): Boolean = client?.bulkIdle() ?: false
}
