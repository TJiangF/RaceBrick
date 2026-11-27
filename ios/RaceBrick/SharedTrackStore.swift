import Foundation
import RaceBrickCore

enum SharedTrackStore {
    private static var fileURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RaceBrick", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("shared_tracks.json")
    }

    static func load() -> [LocalSharedTrack] {
        guard let data = try? Data(contentsOf: fileURL) else { return [] }
        return (try? JSONDecoder().decode([LocalSharedTrack].self, from: data)) ?? []
    }

    static func save(_ tracks: [LocalSharedTrack]) {
        guard let data = try? JSONEncoder().encode(tracks) else { return }
        try? data.write(to: fileURL, options: .atomic)
    }

    static func importTrack(_ payload: TrackPayload) -> (track: LocalSharedTrack, isNew: Bool)? {
        let uuid = payload.trackUuid ?? payload.uuid ?? ""
        guard !uuid.isEmpty else { return nil }
        var tracks = load()
        if let existing = tracks.first(where: { $0.trackUuid == uuid }) {
            return (existing, false)
        }
        let baseName = (payload.name ?? "Shared track").trimmingCharacters(in: .whitespaces)
        var name = baseName.isEmpty ? "Shared track" : String(baseName.prefix(48))
        var suffix = 0
        while tracks.contains(where: { $0.name == name }) {
            suffix += 1
            let tail = suffix == 1 ? " (shared)" : " (shared \(suffix))"
            name = String((baseName.isEmpty ? "Shared track" : baseName).prefix(40)) + tail
        }
        let track = LocalSharedTrack(
            publicId: payload.publicId ?? "",
            trackUuid: uuid,
            name: name,
            payload: payload,
            importedAt: Int(Date().timeIntervalSince1970)
        )
        tracks.append(track)
        save(tracks)
        return (track, true)
    }
}
