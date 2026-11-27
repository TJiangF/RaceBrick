import SwiftUI
import RaceBrickCore

struct PublicTracksView: View {
    @EnvironmentObject private var model: AppModel
    @State private var tracks: [PublicTrackSummary] = []
    @State private var cursor: String?
    @State private var loading = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            List {
                Section("公开赛道") {
                    if tracks.isEmpty {
                        Text(loading ? "正在读取…" : "暂时没有公开赛道")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(tracks) { track in
                        NavigationLink {
                            PublicTrackDetailView(summary: track)
                                .environmentObject(model)
                        } label: {
                            VStack(alignment: .leading, spacing: 3) {
                                Text(track.name.isEmpty ? "未命名赛道" : track.name)
                                    .font(.headline)
                                Text("\(Int(track.lengthM)) m · \(track.geo ? "GPS" : "本地")")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                Text("公开 ID \(track.publicId)")
                                    .font(.caption2.monospaced())
                                    .foregroundStyle(.tertiary)
                            }
                        }
                    }
                    if cursor != nil {
                        Button("加载更多") { Task { await load(reset: false) } }
                            .disabled(loading)
                    }
                }
                Section("本地已导入") {
                    if model.sharedTracks.isEmpty {
                        Text("还没有导入公开赛道").foregroundStyle(.secondary)
                    }
                    ForEach(model.sharedTracks) { track in
                        NavigationLink {
                            SharedTrackView(track: track)
                        } label: {
                            VStack(alignment: .leading, spacing: 3) {
                                Text(track.name).font(.headline)
                                Text(track.payload.geo == true ? "GPS" : "本地")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
            .navigationTitle("公开赛道")
            .refreshable { await load(reset: true) }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { Task { await load(reset: true) } } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .disabled(loading)
                }
            }
            .alert("读取失败", isPresented: Binding(
                get: { error != nil },
                set: { if !$0 { error = nil } }
            )) {
                Button("确定", role: .cancel) { error = nil }
            } message: {
                Text(error ?? "")
            }
            .task { await load(reset: true) }
        }
    }

    private func load(reset: Bool) async {
        guard !loading else { return }
        loading = true
        do {
            let page = try await model.api.publicTracks(limit: 20, cursor: reset ? nil : cursor)
            if reset { tracks = page.tracks } else { tracks.append(contentsOf: page.tracks) }
            cursor = page.nextCursor
        } catch {
            self.error = error.localizedDescription
        }
        loading = false
    }
}

private struct PublicTrackDetailView: View {
    @EnvironmentObject private var model: AppModel
    let summary: PublicTrackSummary
    @State private var detail: PublicTrackResponse?
    @State private var loading = true
    @State private var message: String?
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if let detail {
                    let track = detail.track
                    TrackMapCanvas(track: TrackEnvelope(
                        localId: 0,
                        name: track.name ?? summary.name,
                        hash: "",
                        revision: 1,
                        updatedAt: summary.updatedAt,
                        deleted: false,
                        payload: track
                    ))
                    .frame(height: 300)
                    .background(Color(red: 0.035, green: 0.07, blue: 0.1), in: RoundedRectangle(cornerRadius: 12))

                    Text(track.name ?? summary.name)
                        .font(.title2.bold())
                    Text("长度 \(Int(track.lengthM ?? 0)) m · \(track.geo == true ? "GPS" : "本地")")
                        .font(.caption).foregroundStyle(.secondary)
                    Text("公开 ID \(summary.publicId)")
                        .font(.caption2.monospaced()).foregroundStyle(.tertiary)

                    Button {
                        importTrack(track)
                    } label: {
                        Label("导入到本地", systemImage: "square.and.arrow.down")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.sharedTracks.contains(where: { $0.trackUuid == (track.trackUuid ?? track.uuid) }))

                    if let message {
                        Text(message).font(.footnote).foregroundStyle(.secondary)
                    }
                } else if loading {
                    ProgressView("正在读取赛道…")
                } else if let error {
                    Text(error).foregroundStyle(.red)
                }
            }
            .padding(16)
        }
        .navigationTitle(summary.name)
        .navigationBarTitleDisplayMode(.inline)
        .task {
            do { detail = try await model.api.publicTrack(publicId: summary.publicId) }
            catch { self.error = error.localizedDescription }
            loading = false
        }
    }

    private func importTrack(_ track: TrackPayload) {
        let result = model.importPublicTrack(track)
        if result.0 == nil {
            message = "赛道数据无效，无法导入"
        } else if result.1 {
            message = "已导入到本地：\(result.0?.name ?? "")"
        } else {
            message = "本地已有相同 UUID 的赛道，未覆盖"
        }
    }
}

private struct SharedTrackView: View {
    let track: LocalSharedTrack

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                TrackMapCanvas(track: TrackEnvelope(
                    localId: 0,
                    name: track.name,
                    hash: "",
                    revision: 1,
                    updatedAt: track.importedAt,
                    deleted: false,
                    payload: track.payload
                ))
                .frame(height: 300)
                .background(Color(red: 0.035, green: 0.07, blue: 0.1), in: RoundedRectangle(cornerRadius: 12))
                Text(track.name).font(.title2.bold())
                Text(track.payload.geo == true ? "GPS" : "本地")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(16)
        }
        .navigationTitle(track.name)
        .navigationBarTitleDisplayMode(.inline)
    }
}
