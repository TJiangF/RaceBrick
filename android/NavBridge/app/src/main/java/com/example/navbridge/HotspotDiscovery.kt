package com.example.navbridge

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface

data class DiscoveredDevice(
    val uid: String,
    val ip: String,
    val firmware: String = "",
) {
    val baseUrl: String get() = "http://$ip"
}

object HotspotDiscovery {
    private const val PORT = 4210
    private const val REQUEST = "RACEBRICK_DISCOVER"

    fun find(savedBase: String?, timeoutMs: Int = 3500): DiscoveredDevice? {
        findUdp(timeoutMs)?.let { return it }
        if (!savedBase.isNullOrBlank()) return probe(savedBase)
        return null
    }

    fun probe(base: String): DiscoveredDevice? = runCatching {
        val o = RaceApi.status(base)
        val uid = o.optString("uid", "")
        if (uid.isBlank()) return null
        val ip = o.optString("ip", "").ifBlank {
            runCatching { java.net.URL(base).host }.getOrDefault("")
        }
        DiscoveredDevice(uid, ip, "local")
    }.getOrNull()

    private fun findUdp(timeoutMs: Int): DiscoveredDevice? {
        val payload = REQUEST.toByteArray(Charsets.UTF_8)
        val sockets = ArrayList<DatagramSocket>()
        val targets = HashSet<String>()
        runCatching {
            val ifaces = NetworkInterface.getNetworkInterfaces() ?: return@runCatching
            for (iface in ifaces) {
                if (!iface.isUp || iface.isLoopback) continue
                for (ia in iface.interfaceAddresses) {
                    val addr = ia.address
                    if (addr !is Inet4Address || addr.isLoopbackAddress) continue
                    val broadcast = ia.broadcast?.hostAddress
                        ?: prefixBroadcast(addr, ia.networkPrefixLength.toInt())
                    if (broadcast == null) continue
                    targets += broadcast
                    runCatching {
                        val s = DatagramSocket(null)
                        s.reuseAddress = true
                        s.broadcast = true
                        s.bind(InetSocketAddress(addr, 0))
                        s.soTimeout = 300
                        s.send(DatagramPacket(payload, payload.size, InetAddress.getByName(broadcast), PORT))
                        sockets += s
                    }
                }
            }
        }
        if (sockets.isEmpty()) {
            runCatching {
                val s = DatagramSocket()
                s.broadcast = true
                s.soTimeout = 300
                s.send(DatagramPacket(payload, payload.size, InetAddress.getByName("255.255.255.255"), PORT))
                sockets += s
            }
        }
        val deadline = System.currentTimeMillis() + timeoutMs
        try {
            while (System.currentTimeMillis() < deadline) {
                var got = false
                for (s in sockets) {
                    val buf = ByteArray(512)
                    val pkt = DatagramPacket(buf, buf.size)
                    try {
                        s.receive(pkt)
                        got = true
                        val text = String(pkt.data, 0, pkt.length, Charsets.UTF_8).trim()
                        parse(text, pkt.address.hostAddress)?.let { return it }
                    } catch (_: java.net.SocketTimeoutException) {
                    }
                }
                if (!got) Thread.sleep(40)
            }
        } finally {
            for (s in sockets) runCatching { s.close() }
        }
        return null
    }

    private fun parse(text: String, sourceIp: String?): DiscoveredDevice? {
        if (!text.startsWith("RACEBRICK|")) return null
        val parts = text.split('|')
        if (parts.size < 3) return null
        val uid = parts[1].trim()
        val ip = parts[2].trim().ifBlank { sourceIp.orEmpty() }
        if (uid.isEmpty() || ip.isEmpty()) return null
        return DiscoveredDevice(uid, ip, parts.getOrNull(3).orEmpty())
    }

    private fun prefixBroadcast(addr: Inet4Address, prefix: Int): String? {
        if (prefix < 0 || prefix > 32) return null
        val raw = addr.address
        var value = 0
        for (b in raw) value = (value shl 8) or (b.toInt() and 0xFF)
        val mask = if (prefix == 0) 0 else (-1 shl (32 - prefix))
        val bcast = value or mask.inv()
        return "${(bcast ushr 24) and 0xFF}.${(bcast ushr 16) and 0xFF}." +
            "${(bcast ushr 8) and 0xFF}.${bcast and 0xFF}"
    }
}
