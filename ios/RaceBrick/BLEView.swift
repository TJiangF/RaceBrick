import SwiftUI
import RaceBrickCore

struct BLEView: View {
    @EnvironmentObject private var ble: BLEManager
    @StateObject private var streamer = NavFrameStreamer()

    var body: some View {
        NavigationStack {
            List {
                Section("RaceBrick BLE") {
                    HStack {
                        Circle().fill(ble.connected ? .green : .gray)
                            .frame(width: 10, height: 10)
                        VStack(alignment: .leading) {
                            Text(ble.name.isEmpty ? "RaceBrick" : ble.name).font(.headline)
                            Text(ble.state).font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if let rssi = ble.rssi, !ble.connected {
                            Text("\(rssi) dBm").font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                    }
                    if ble.connected {
                        Button("断开", role: .destructive) { ble.disconnect() }
                    } else {
                        Button("搜索并连接") { ble.connect() }
                    }
                }
                Section("导航地图推流") {
                    Text("iOS 使用 Apple 地图；每秒截图 320×240，量化为 16 色并推送到 ESP。")
                        .font(.footnote).foregroundStyle(.secondary)
                    Text(streamer.locationText).font(.caption.monospaced()).foregroundStyle(.secondary)
                    Text(streamer.status).font(.caption).foregroundStyle(.secondary)
                    Button(streamer.streaming ? "停止推流" : "开始地图推流") {
                        if streamer.streaming { streamer.stop() }
                        else { streamer.start(ble: ble) }
                    }
                    .disabled(!ble.connected)
                }
                Section("测试") {
                    Button("发送状态查询 #GB?") { ble.send("#GB?") }
                    Button("发送测试媒体标题") { ble.send("M,TITLE,RaceBrick%20iOS") }
                    Text("媒体标题需要 URL 编码；真实媒体读取受 iOS 限制，详见 Media 页。")
                        .font(.caption2).foregroundStyle(.secondary)
                }
                Section("BLE 日志") {
                    ForEach(Array(ble.log.suffix(30).enumerated()), id: \.offset) { _, line in
                        Text(line).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Device Link")
        }
    }
}
