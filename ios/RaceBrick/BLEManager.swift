import Foundation
import CoreBluetooth
import Combine
import RaceBrickCore

@MainActor
final class BLEManager: NSObject, ObservableObject {
    @Published private(set) var state = "未连接"
    @Published private(set) var connected = false
    @Published private(set) var name = ""
    @Published private(set) var log: [String] = []
    @Published private(set) var rssi: Int?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var tx: CBCharacteristic?
    private var scanTimer: Timer?
    private var bulk: [Data] = []
    private var sending = false
    private var packetId: UInt8 = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func connect() {
        guard central.state == .poweredOn else {
            state = "蓝牙不可用：\(stateText(central.state))"; return
        }
        disconnect()
        state = "正在搜索 RaceBrick…"
        log.append("扫描 Service \(BLEProtocol.serviceUUID.uuidString)")
        central.scanForPeripherals(withServices: [CBUUID(nsuuid: BLEProtocol.serviceUUID)], options: nil)
        scanTimer?.invalidate()
        scanTimer = Timer.scheduledTimer(withTimeInterval: 12, repeats: false) { [weak self] _ in
            Task { @MainActor in
                guard let self, !self.connected else { return }
                self.central.stopScan()
                self.state = "没有找到 RaceBrick"
            }
        }
    }

    func disconnect() {
        scanTimer?.invalidate(); scanTimer = nil
        if central.state == .poweredOn { central.stopScan() }
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        peripheral = nil; rx = nil; tx = nil
        connected = false; state = "未连接"; name = ""
        bulk.removeAll(); sending = false
        NotificationCenter.default.post(name: .raceBrickBLEState, object: nil, userInfo: ["connected": false])
    }

    func send(_ text: String) {
        guard let p = peripheral, let c = rx, connected else { return }
        let data = Data(text.utf8)
        p.writeValue(data, for: c, type: .withoutResponse)
    }

    func sendBulk(type: UInt8, data: Data) {
        guard connected, let p = peripheral, let c = rx else { return }
        let cap = max(20, p.maximumWriteValueLength(for: .withoutResponse) - BLEProtocol.headerSize)
        let packets = BLEProtocol.bulkPackets(type: type, id: packetId, data: data, payloadCapacity: cap)
        packetId &+= 1
        guard !packets.isEmpty else { return }
        bulk.append(contentsOf: packets)
        pump()
    }

    private func pump() {
        guard !sending, connected, let p = peripheral, let c = rx, !bulk.isEmpty else { return }
        sending = true
        let packet = bulk.removeFirst()
        p.writeValue(packet, for: c, type: .withoutResponse)
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 6_000_000)
            self.sending = false
            self.pump()
        }
    }

    fileprivate func discovered(_ peripheral: CBPeripheral, rssi: Int) {
        self.name = peripheral.name ?? "RaceBrick"
        self.rssi = rssi
        state = "正在连接 \(name)…"
        central.stopScan()
        scanTimer?.invalidate()
        self.peripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    private func stateText(_ s: CBManagerState) -> String {
        switch s {
        case .poweredOff: return "请打开蓝牙"
        case .unauthorized: return "未授权蓝牙"
        case .unsupported: return "设备不支持 BLE"
        case .resetting: return "蓝牙正在重置"
        default: return "等待蓝牙"
        }
    }
}

extension Notification.Name {
    static let raceBrickBLEState = Notification.Name("raceBrickBLEState")
}

extension BLEManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            if central.state != .poweredOn { self.state = self.stateText(central.state) }
        }
    }
    nonisolated func centralManager(_ central: CBCentralManager,
                                    didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any],
                                    rssi RSSI: NSNumber) {
        Task { @MainActor in self.discovered(peripheral, rssi: RSSI.intValue) }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            self.connected = true
            self.state = "已连接 · 发现服务"
            self.log.append("已连接 \(peripheral.name ?? "RaceBrick")")
            peripheral.discoverServices([CBUUID(nsuuid: BLEProtocol.serviceUUID)])
        }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            self.state = "连接失败：\(error?.localizedDescription ?? "unknown")"
            self.connected = false
        }
    }
    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            self.connected = false; self.state = "连接已断开"
            self.peripheral = nil; self.rx = nil; self.tx = nil
        }
    }
}

extension BLEManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            if let error { self.state = "服务发现失败：\(error.localizedDescription)"; return }
            let serviceCB = CBUUID(nsuuid: BLEProtocol.serviceUUID)
            for s in peripheral.services ?? [] where s.uuid == serviceCB {
                peripheral.discoverCharacteristics([
                    CBUUID(nsuuid: BLEProtocol.rxUUID),
                    CBUUID(nsuuid: BLEProtocol.txUUID),
                ], for: s)
            }
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        Task { @MainActor in
            if let error { self.state = "特征发现失败：\(error.localizedDescription)"; return }
            for c in service.characteristics ?? [] {
                if c.uuid == CBUUID(nsuuid: BLEProtocol.rxUUID) { self.rx = c }
                if c.uuid == CBUUID(nsuuid: BLEProtocol.txUUID) {
                    self.tx = c
                    peripheral.setNotifyValue(true, for: c)
                }
            }
            if self.rx != nil && self.tx != nil {
                self.state = "通道就绪 · 已连接"
                self.send("#GB?")
                self.log.append("BLE 通道就绪")
                NotificationCenter.default.post(name: .raceBrickBLEState, object: nil, userInfo: ["connected": true])
            } else {
                self.state = "没有找到 RaceBrick 读写特征"
            }
        }
    }
    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        let text = String(data: data, encoding: .utf8) ?? "<binary \(data.count)B>"
        Task { @MainActor in
            if !text.hasPrefix("M,") && !text.hasPrefix("K,") {
                self.log.append("← \(text.prefix(180))")
                if self.log.count > 80 { self.log.removeFirst() }
                if text.contains("GB") || text.contains("READY") { self.state = "通道就绪 · 已连接" }
            }
        }
    }
}
