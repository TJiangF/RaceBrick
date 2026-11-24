import SwiftUI
import RaceBrickCore

struct DeviceDetailView: View {
    @EnvironmentObject private var model: AppModel
    let uid: String
    @State private var tab = 0
    @State private var editingTrack: TrackEnvelope?

    var body: some View {
        Group {
            if let detail = model.detail, detail.device.uid == uid {
                ScrollView {
                    VStack(alignment: .leading, spacing: 14) {
                        header(detail.device)
                        Picker("", selection: $tab) {
                            Text("Track \(detail.tracks.count)").tag(0)
                            Text("Race Log \(detail.sessions.count)").tag(1)
                            Text("Legacy \(detail.logs.count)").tag(2)
                        }
                        .pickerStyle(.segmented)
                        if !detail.pendingCommands.isEmpty {
                            Label("\(detail.pendingCommands.count) 个服务器设置等待设备同步",
                                  systemImage: "arrow.triangle.2.circlepath")
                                .font(.footnote).foregroundStyle(.orange)
                        }
                        switch tab {
                        case 0: tracks(detail.tracks)
                        case 1: sessions(detail.sessions)
                        default: logs(detail.logs)
                        }
                    }
                    .padding(16)
                }
                .refreshable { await model.loadDevice(uid) }
            } else if model.busy {
                ProgressView("载入设备…")
            } else {
                VStack(spacing: 8) {
                    Image(systemName: "externaldrive.badge.questionmark").font(.largeTitle)
                    Text("设备未加载").font(.headline)
                    Button("刷新") { Task { await model.loadDevice(uid) } }
                }
            }
        }
        .navigationTitle(model.detail?.device.displayName ?? uid)
        .navigationBarTitleDisplayMode(.inline)
        .task(id: uid) { await model.loadDevice(uid) }
        .sheet(item: $editingTrack) { track in
            TrackEditorView(uid: uid, track: track)
                .environmentObject(model)
        }
    }

    private func header(_ d: CloudDevice) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(d.displayName).font(.title3.bold())
                Text(d.uid).font(.caption.monospaced()).foregroundStyle(.secondary)
                Text("\(d.board.isEmpty ? "ESP32-S3" : d.board) · \(d.fw)")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            Spacer()
            Circle().fill(d.online ? .green : .gray.opacity(0.4)).frame(width: 10, height: 10)
        }
        .padding(14)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private func tracks(_ tracks: [TrackEnvelope]) -> some View {
        LazyVStack(spacing: 12) {
            if tracks.isEmpty { Text("设备尚未上传 Track").foregroundStyle(.secondary).padding() }
            ForEach(tracks) { track in
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(track.name.isEmpty ? "Track \(track.localId)" : track.name).font(.headline)
                            Text("slot \(track.localId) · " +
                                 (track.payload?.geo == true ? "GPS" : "local") +
                                 " · rev \(track.revision)")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("编辑") { editingTrack = track }
                            .buttonStyle(.bordered)
                    }
                    TrackMapCanvas(track: track).frame(height: 190)
                    HStack {
                        Text("长度 \(Int(track.payload?.lengthM ?? 0))m")
                        Text("起始线 \(String(format: "%.1f", track.payload?.startWidth ?? 5))m")
                        Text("偏移 \(String(format: "%+.1f", track.payload?.startOffset ?? 0))m")
                    }
                    .font(.caption2).foregroundStyle(.secondary)
                }
                .padding(12)
                .background(Color(red: 0.08, green: 0.11, blue: 0.15), in: RoundedRectangle(cornerRadius: 14))
            }
        }
    }

    private func sessions(_ sessions: [SessionEnvelope]) -> some View {
        LazyVStack(spacing: 10) {
            if sessions.isEmpty { Text("还没有 Race Log").foregroundStyle(.secondary).padding() }
            ForEach(sessions) { s in
                NavigationLink {
                    SessionDetailView(session: s)
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(s.trackName.isEmpty ? "Session \(s.localSlot)" : s.trackName)
                                .font(.headline)
                            Text("\(s.laps) 圈 · \(Self.duration(s.durationMs))")
                                .font(.caption).foregroundStyle(.secondary)
                            if let best = s.bestLap, let t = s.bestTime {
                                Text("最快 L\(best) · \(Self.lap(t))")
                                    .font(.caption.bold()).foregroundStyle(.purple)
                            }
                        }
                        Spacer()
                        Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                    }
                    .padding(12)
                    .background(Color(red: 0.08, green: 0.11, blue: 0.15), in: RoundedRectangle(cornerRadius: 12))
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func logs(_ logs: [RaceLogEnvelope]) -> some View {
        LazyVStack(spacing: 10) {
            if logs.isEmpty { Text("没有旧版 Race Log").foregroundStyle(.secondary).padding() }
            ForEach(logs) { log in
                VStack(alignment: .leading, spacing: 4) {
                    Text(log.trackName.isEmpty ? "Race Log \(log.localIndex)" : log.trackName)
                        .font(.headline)
                    Text("\(log.laps) 圈" + (log.bestLap != nil ? " · 最快 L\(log.bestLap!) · \(Self.lap(log.bestTime ?? 0))" : ""))
                        .font(.caption).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
                .background(Color(red: 0.08, green: 0.11, blue: 0.15), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    static func lap(_ seconds: Double) -> String {
        guard seconds.isFinite else { return "--" }
        let m = Int(seconds / 60), s = seconds - Double(m * 60)
        return String(format: "%d:%06.3f", m, s)
    }
    static func duration(_ ms: Int?) -> String {
        guard let ms else { return "--" }
        let s = ms / 1000
        return String(format: "%d:%02d", s / 60, s % 60)
    }
}

struct TrackEditorView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let uid: String
    let track: TrackEnvelope
    @State private var name: String
    @State private var startF: Double
    @State private var startWidth: Double
    @State private var offset: Double
    @State private var reverse: Bool
    @State private var saving = false

    init(uid: String, track: TrackEnvelope) {
        self.uid = uid
        self.track = track
        _name = State(initialValue: track.payload?.name ?? track.name)
        _startF = State(initialValue: (track.payload?.startF ?? 0) * 100)
        _startWidth = State(initialValue: track.payload?.startWidth ?? track.payload?.width ?? 5)
        _offset = State(initialValue: track.payload?.startOffset ?? 0)
        _reverse = State(initialValue: track.payload?.reverse ?? false)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("赛道") {
                    TextField("名称", text: $name)
                    LabeledContent("长度", value: "\(Int(track.payload?.lengthM ?? 0)) m")
                    LabeledContent("Track slot", value: "\(track.localId)")
                }
                Section("起始线") {
                    VStack(alignment: .leading) {
                        Text("位置 \(startF, specifier: "%.1f")%")
                        Slider(value: $startF, in: 0...99.9, step: 0.1)
                    }
                    VStack(alignment: .leading) {
                        Text("宽度 \(startWidth, specifier: "%.1f") m")
                        Slider(value: $startWidth, in: 1...50, step: 0.5)
                    }
                    VStack(alignment: .leading) {
                        Text("左右偏移 \(offset, specifier: "%+.1f") m")
                        Slider(value: $offset, in: -20...20, step: 0.5)
                    }
                    Toggle("反向起点方向（CCW）", isOn: $reverse)
                }
                Section {
                    Text("保存后先写入服务器，并生成设备同步命令；设备联网同步后生效。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("编辑 Track")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("取消") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("保存") {
                        Task {
                            saving = true
                            var fields: [String: JSONValue] = [
                                "name": .string(name),
                                "start_f": .number(startF / 100),
                                "start_width": .number(startWidth),
                                "start_offset": .number(offset),
                                "reverse": .bool(reverse),
                            ]
                            if name.trimmingCharacters(in: .whitespaces).isEmpty {
                                fields["name"] = .string(track.name)
                            }
                            await model.updateTrack(uid: uid, localId: track.localId, fields: fields)
                            saving = false
                            dismiss()
                        }
                    }
                    .disabled(saving)
                }
            }
        }
    }
}
