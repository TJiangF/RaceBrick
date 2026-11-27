import Foundation
import RaceBrickCore

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var token: String?
    @Published private(set) var user: CloudUser?
    @Published private(set) var devices: [CloudDevice] = []
    @Published private(set) var detail: DeviceDetail?
    @Published private(set) var adminUsers: [AdminUser] = []
    @Published private(set) var sharedTracks: [LocalSharedTrack] = SharedTrackStore.load()
    @Published var selectedDeviceUID: String?
    @Published private(set) var busy = false
    @Published var errorMessage: String?

    let api = CloudAPI()

    var authenticated: Bool { token != nil && user != nil }
    var isAdmin: Bool { user?.isAdmin == true }

    init() {
        token = KeychainStore.load()
    }

    func restore() async {
        guard let t = token else { return }
        do {
            user = try await api.me(token: t)
            await loadDevices()
        } catch {
            token = nil
            user = nil
            KeychainStore.delete()
        }
    }

    func login(username: String, password: String) async {
        await run {
            let auth = try await self.api.login(username: username, password: password)
            self.token = auth.token
            self.user = auth.user
            KeychainStore.save(auth.token)
            await self.loadDevices()
        }
    }

    func register(username: String, password: String) async {
        await run {
            let auth = try await self.api.register(username: username, password: password)
            self.token = auth.token
            self.user = auth.user
            KeychainStore.save(auth.token)
            await self.loadDevices()
        }
    }

    func logout() {
        token = nil
        user = nil
        devices = []
        detail = nil
        selectedDeviceUID = nil
        KeychainStore.delete()
    }

    func loadDevices() async {
        guard let t = token else { return }
        do {
            devices = try await api.devices(token: t)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func bind(uid: String) async {
        guard let t = token else { return }
        await run {
            try await self.api.bind(uid: uid, token: t)
            await self.loadDevices()
            self.selectedDeviceUID = uid
        }
    }

    func loadDevice(_ uid: String) async {
        guard let t = token else { return }
        await run {
            self.detail = try await self.api.device(uid: uid, token: t)
            self.selectedDeviceUID = uid
        }
    }

    func updateTrack(uid: String, localId: Int, fields: [String: JSONValue]) async {
        guard let t = token else { return }
        await run {
            try await self.api.updateTrack(uid: uid, localId: localId, token: t, fields: fields)
            self.detail = try await self.api.device(uid: uid, token: t)
        }
    }

    func loadAdminUsers() async {
        guard let t = token else { return }
        await run { self.adminUsers = try await self.api.adminUsers(token: t) }
    }

    func importPublicTrack(_ payload: TrackPayload) -> (LocalSharedTrack?, Bool) {
        guard let result = SharedTrackStore.importTrack(payload) else { return (nil, false) }
        sharedTracks = SharedTrackStore.load()
        return (result.track, result.isNew)
    }

    private func run(_ block: @escaping () async throws -> Void) async {
        busy = true
        defer { busy = false }
        do { try await block() }
        catch { errorMessage = error.localizedDescription }
    }
}
