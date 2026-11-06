package com.example.navbridge

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PatternMatcher

/**
 * Android 10+ direct connection to the RaceBrick SoftAP.
 *
 * ESP32 does not support Wi-Fi Direct/P2P, so "direct" means joining the
 * RaceBrick-XXXX SoftAP. The system asks the user once; this class requests the
 * matching AP by prefix and binds the app process to that local network while
 * the RACE screen is open. Leaving RACE unbinds it so NAV regains internet.
 */
class WifiDirectConnector(private val context: Context) {
    companion object {
        const val AP_PREFIX = "RaceBrick-"
        const val AP_PASSWORD = "racebrick"
        const val AP_URL = "http://192.168.4.1"
    }

    private val main = Handler(Looper.getMainLooper())
    private val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var network: Network? = null

    val isConnected: Boolean get() = network != null

    /** Bind the WiFi network if the user already joined RaceBrick manually. */
    fun bindCurrentEsp(): Boolean {
        val ssid = runCatching { wifi.connectionInfo.ssid }.getOrNull()
            ?.trim('"').orEmpty()
        if (!ssid.startsWith(AP_PREFIX)) return false
        val existing = cm.allNetworks.firstOrNull { n ->
            val caps = cm.getNetworkCapabilities(n) ?: return@firstOrNull false
            caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
        } ?: return false
        network = existing
        cm.bindProcessToNetwork(existing)
        return true
    }

    fun connect(onResult: (ok: Boolean, message: String) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            onResult(false, "一键直连需要 Android 10+；请手动连接 ${AP_PREFIX}XXXX")
            return
        }
        disconnect()

        // The user may already have joined RaceBrick manually. In that case
        // requesting WifiNetworkSpecifier would fail, so bind the existing
        // WiFi network instead and reuse it.
        if (bindCurrentEsp()) {
            val ssid = runCatching { wifi.connectionInfo.ssid }.getOrNull()
                ?.trim('"').orEmpty()
            onResult(true, "已连接 ESP WiFi：$ssid")
            return
        }

        val specifier = WifiNetworkSpecifier.Builder()
            .setSsidPattern(PatternMatcher(AP_PREFIX, PatternMatcher.PATTERN_PREFIX))
            .setWpa2Passphrase(AP_PASSWORD)
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifier)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                this@WifiDirectConnector.network = network
                // Send all app HTTP through the ESP AP while RACE is open.
                cm.bindProcessToNetwork(network)
                main.post { onResult(true, "已直连 ESP：$AP_URL") }
            }

            override fun onUnavailable() {
                this@WifiDirectConnector.network = null
                main.post { onResult(false, "没有找到 RaceBrick-XXXX，确认 ESP 已启动 AP") }
            }

            override fun onLost(network: Network) {
                if (this@WifiDirectConnector.network == network) {
                    this@WifiDirectConnector.network = null
                    cm.bindProcessToNetwork(null)
                    main.post { onResult(false, "ESP 直连已断开") }
                }
            }
        }
        callback = cb
        try {
            cm.requestNetwork(request, cb)
        } catch (e: Exception) {
            callback = null
            onResult(false, "请求直连失败：${e.message}")
        }
    }

    fun disconnect() {
        callback?.let {
            runCatching { cm.unregisterNetworkCallback(it) }
        }
        callback = null
        if (network != null) {
            cm.bindProcessToNetwork(null)
            network = null
        }
    }
}
