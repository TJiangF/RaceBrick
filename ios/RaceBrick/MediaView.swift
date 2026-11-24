import SwiftUI
import MediaPlayer
import AVFoundation
import UIKit

struct MediaView: View {
    @State private var status = "iOS 媒体限制"
    @State private var audioSession = AVAudioSession.sharedInstance()

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Label("iOS 不允许读取其他 App 的媒体通知/歌词", systemImage: "exclamationmark.shield")
                        .font(.headline)
                    Text("Android 版通过 NotificationListener 读取网易云、QQ音乐等应用的标题/歌词并推送到 ESP。iOS 没有等价的公共 API：MPNowPlayingInfoCenter 只能提供当前 App 自己的播放信息。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("iOS 可用的媒体方案") {
                    Text("• App 自身播放音频时，用 MPNowPlayingInfoCenter 发布标题、封面和进度。")
                    Text("• 用 MPRemoteCommandCenter 接收播放/暂停/上一首/下一首命令。")
                    Text("• 通过 BLE 把自有播放器的状态发送给 ESP。")
                    Text("• 第三方 Apple Music / Spotify 的歌词读取不支持，不能做 Android 同样的镜像。")
                }
                .font(.footnote)
                Section("当前音频会话") {
                    LabeledContent("Category", value: audioSession.category.rawValue)
                    LabeledContent("模式", value: audioSession.mode.rawValue)
                    Button("配置播放器音频会话") {
                        do {
                            try audioSession.setCategory(.playback, mode: .default, options: [])
                            try audioSession.setActive(true)
                            status = "音频会话已配置；媒体信息需由自有播放器发布"
                        } catch {
                            status = "配置失败：\(error.localizedDescription)"
                        }
                    }
                    Text(status).font(.caption).foregroundStyle(.secondary)
                }
                Section("建议") {
                    Text("如果需要在 iOS 上实现类似 Android 的歌词页，需将媒体播放能力集成到 RaceBrick 自有播放器，或仅展示系统 Now Playing 可见的信息。")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("打开系统设置") {
                        if let url = URL(string: UIApplication.openSettingsURLString) {
                            UIApplication.shared.open(url)
                        }
                    }
                }
            }
            .navigationTitle("Media")
        }
    }
}
