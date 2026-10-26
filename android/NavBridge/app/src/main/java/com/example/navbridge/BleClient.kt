package com.example.navbridge

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.util.UUID
import kotlin.math.min

/**
 * BLE 中心设备，协议对齐 MotoNav 1.75 固件。
 *
 * - 服务 6e400001 / 写 6e400002 / 通知 6e400003
 * - ASCII 指令：一次 write 一条，不需要换行
 * - 大块数据：'M' type id parts(2) seq(2) total(4) offset(4) + payload
 * - 圆屏配对验证通过后发 "U,1"，之后才发大块数据
 */
class BleClient(
    context: Context,
    private val onStatus: (String) -> Unit,
    private val onReady: () -> Unit,
    private val onLine: (String) -> Unit,
) {
    companion object {
        val SERVICE: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val TYPE_ROUTE = 2
        const val TYPE_RASTER = 3
        const val TYPE_FRAME = 5   // 手机整帧：320×240 16 色 RLE

        private const val BULK_HEADER = 15
        private const val MAX_WRITE = 500
    }

    private val app = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val lock = Any()

    // 控制指令优先于大块数据，避免传底图时定位/转向被堵在队列里。
    private val control = ArrayDeque<ByteArray>()
    private val bulk = ArrayDeque<ByteArray>()

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null
    @Volatile private var mtu = 23
    @Volatile private var writing = false
    private var scanning = false

    @Volatile var connected = false
        private set
    @Volatile var verified = false
        private set

    private val adapter get() =
        (app.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private val heartbeat = object : Runnable {
        override fun run() {
            // 配对/连接完成后用 #GB? 兜底重发 U,1
            if (connected && !verified &&
                gatt?.device?.bondState == BluetoothDevice.BOND_BONDED
            ) {
                send("#GB?")
            }
            handler.postDelayed(this, 2500)
        }
    }

    @SuppressLint("MissingPermission")
    fun connect() {
        if (connected) { onStatus("已经连接"); return }
        val a = adapter ?: run { onStatus("这台设备没有蓝牙"); return }
        if (!a.isEnabled) { onStatus("请先打开蓝牙"); return }
        startScan()
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (scanning) return
        val a = adapter ?: return
        val scanner = a.bluetoothLeScanner ?: run { onStatus("无法获取 BLE 扫描器"); return }
        scanning = true
        onStatus("正在搜索 RaceBrick…")
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        runCatching { scanner.startScan(filters, settings, scanCallback) }
            .onFailure { scanning = false; onStatus("开始扫描失败：${it.message}") }
        handler.postDelayed({
            if (scanning) {
                stopScan()
                onStatus("没找到 RaceBrick。确认设备已开机、在附近，且没有连着别的手机")
            }
        }, 15000)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!scanning) return
        scanning = false
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            stopScan()
            onStatus("发现 ${device.name ?: device.address}，正在连接…")
            runCatching {
                gatt = device.connectGatt(app, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            }.onFailure { onStatus("连接失败：${it.message}") }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            onStatus("扫描失败，错误码 $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connected = true
                onStatus("已连接，正在发现服务…")
                g.discoverServices()
            } else {
                connected = false
                verified = false
                writing = false
                synchronized(lock) { control.clear(); bulk.clear() }
                onStatus("设备已断开")
                runCatching { g.close() }
                if (gatt === g) gatt = null
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SERVICE)
            if (svc == null) {
                onStatus("设备没有 NavBridge 服务，请确认固件已启用 BLE")
                return
            }
            rxChar = svc.getCharacteristic(RX)
            txChar = svc.getCharacteristic(TX)
            if (rxChar == null || txChar == null) {
                onStatus("找不到读写特征")
                return
            }
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
            enableNotify(g, txChar!!)
            runCatching { g.requestMtu(517) }
            if (g.device.bondState != BluetoothDevice.BOND_BONDED) {
                onStatus("请在系统配对框确认连接 RaceBrick")
                runCatching { g.device.createBond() }
            } else {
                send("#GB?")
            }
            handler.removeCallbacks(heartbeat)
            handler.post(heartbeat)
            handler.postDelayed({ pump() }, 120)
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            this@BleClient.mtu = mtu
            onStatus("通道就绪，MTU=$mtu，单片 ${payloadCap()} 字节")
            handler.post { pump() }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            handleNotify(c.value ?: return)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            handleNotify(value)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            // NO_RESPONSE 写回调在不同协议栈上不可靠，统一由 pump 的固定节拍推进
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableNotify(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(c, true)
        val desc = c.getDescriptor(CCCD) ?: return
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            g.writeDescriptor(desc)
        }
    }

    private fun handleNotify(value: ByteArray) {
        val text = String(value, Charsets.UTF_8).trim()
        if (text.isNotEmpty() && text.all { it.code in 32..126 }) onLine(text)
        when {
            text == "U,1" -> {
                if (!verified) {
                    verified = true
                    onStatus("设备验证通过")
                    // 默认原版配色、开小地图、白天
                    send("#t,0,1")
                    send("#m,1")
                    send("#n,0")
                    onReady()
                }
            }
            text.startsWith("K,") -> onLine("圆屏确认收到大块数据 $text")
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        handler.removeCallbacks(heartbeat)
        stopScan()
        writing = false
        synchronized(lock) { control.clear(); bulk.clear() }
        verified = false
        connected = false
        runCatching { gatt?.disconnect() }
        runCatching { gatt?.close() }
        gatt = null
    }

    private fun payloadCap(): Int {
        // 固件单个写包上限按 500 字节算，再扣掉 15 字节大块头
        val frame = min(mtu - 3, MAX_WRITE)
        return frame - BULK_HEADER
    }

    fun send(text: String) {
        enqueue(control, text.toByteArray(Charsets.UTF_8))
        handler.post { pump() }
    }

    /** 大块通道是否空闲（没有排队、也没有正在写）。整帧发送用它丢帧，避免越积越多。 */
    fun bulkIdle(): Boolean = !writing && synchronized(lock) { bulk.isEmpty() }

    fun sendBulk(type: Int, id: Int, data: ByteArray) {
        if (data.isEmpty()) return
        // 没连圆屏/没配对时不发，数据由 MainActivity 缓存，连上后自动补发
        if (!verified) return
        val cap = payloadCap()
        if (cap < 40) return
        val parts = (data.size + cap - 1) / cap
        if (parts > 4000) return
        for (p in 0 until parts) {
            val off = p * cap
            val len = min(cap, data.size - off)
            val pkt = ByteArray(BULK_HEADER + len)
            pkt[0] = 'M'.code.toByte()
            pkt[1] = type.toByte()
            pkt[2] = id.toByte()
            pkt[3] = (parts and 0xFF).toByte()
            pkt[4] = ((parts shr 8) and 0xFF).toByte()
            pkt[5] = (p and 0xFF).toByte()
            pkt[6] = ((p shr 8) and 0xFF).toByte()
            val total = data.size
            pkt[7] = (total and 0xFF).toByte()
            pkt[8] = ((total shr 8) and 0xFF).toByte()
            pkt[9] = ((total shr 16) and 0xFF).toByte()
            pkt[10] = ((total shr 24) and 0xFF).toByte()
            pkt[11] = (off and 0xFF).toByte()
            pkt[12] = ((off shr 8) and 0xFF).toByte()
            pkt[13] = ((off shr 16) and 0xFF).toByte()
            pkt[14] = ((off shr 24) and 0xFF).toByte()
            System.arraycopy(data, off, pkt, BULK_HEADER, len)
            enqueue(bulk, pkt)
        }
        handler.post { pump() }
    }

    private fun enqueue(q: ArrayDeque<ByteArray>, bytes: ByteArray) {
        synchronized(lock) {
            // 队列挤满时丢掉最早的，防止内存爆掉
            if (q.size > 4000) q.removeFirst()
            q.addLast(bytes)
        }
    }

    @SuppressLint("MissingPermission")
    private fun pump() {
        val g = gatt ?: return
        val c = rxChar ?: return
        if (!connected) return
        if (writing) return
        val bytes = synchronized(lock) {
            control.removeFirstOrNull() ?: bulk.removeFirstOrNull()
        } ?: return
        writing = true
        val ok = runCatching { write(g, c, bytes) }.getOrDefault(false)
        if (!ok) writing = false
        // 有些协议栈 NO_RESPONSE 不回写回调，固定节拍继续发
        handler.postDelayed({
            if (writing) writing = false
            pump()
        }, 20)
    }

    @SuppressLint("MissingPermission")
    private fun write(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, value, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            c.value = value
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }
}
