import Foundation

public struct CloudUser: Codable, Equatable, Sendable {
    public let id: Int
    public let username: String
    public let role: String
    public var isAdmin: Bool { role == "admin" }
}

public struct AuthResponse: Codable, Sendable {
    public let token: String
    public let user: CloudUser
}

public struct MeResponse: Codable, Sendable { public let user: CloudUser }

public struct CloudDevice: Codable, Identifiable, Sendable {
    public var id: String { uid }
    public let uid: String
    public let name: String
    public let board: String
    public let fw: String
    public let owner: String?
    public let bound: Bool
    public let createdAt: Int
    public let lastSeen: Int?
    public let lastSyncAt: Int?
    public let tracks: Int?
    public let sessions: Int?
    public var displayName: String { name.isEmpty ? uid : name }
    public var online: Bool { (lastSeen ?? 0) > Int(Date().timeIntervalSince1970) - 600 }
}

public struct DevicesResponse: Codable, Sendable { public let devices: [CloudDevice] }

public struct TrackPayload: Codable, Sendable {
    public var name: String?
    public var geo: Bool?
    public var lengthM: Double?
    public var width: Double?
    public var startWidth: Double?
    public var startF: Double?
    public var startOffset: Double?
    public var reverse: Bool?
    public var points: [[Double]]?
    public var glat0: Double?
    public var glon0: Double?
    public var nscale: Double?
    public var nox: Double?
    public var noy: Double?
    public var lineLat: Double?
    public var lineLon: Double?
    public var lineHdg: Double?
    public var start: TrackStart?
}

public struct TrackStart: Codable, Sendable {
    public var lat: Double?
    public var lon: Double?
    public var hdg: Double?
}

public struct TrackEnvelope: Codable, Identifiable, Sendable {
    public var id: Int { localId }
    public let localId: Int
    public let name: String
    public let hash: String
    public let revision: Int
    public let updatedAt: Int
    public let deleted: Bool
    public let payload: TrackPayload?
}

public struct LapSummary: Codable, Identifiable, Sendable {
    public var id: Int { lap }
    public let lap: Int
    public let time: Double
    public let samples: Int
    public let distanceM: Double
    public let avgKmh: Double
    public let maxKmh: Double
    /// [x, y, speed_kmh, elapsed_s]
    public let points: [[Double]]
}

public struct SessionSummary: Codable, Sendable {
    public let laps: [LapSummary]
    public let bestLap: Int?
    public let bestTime: Double?
}

public struct SessionEnvelope: Codable, Identifiable, Sendable {
    public var id: Int { localSlot }
    public let localSlot: Int
    public let hash: String
    public let revision: Int
    public let deleted: Bool
    public let updatedAt: Int
    public let trackLocalId: Int?
    public let trackName: String
    public let startEpoch: Int?
    public let durationMs: Int?
    public let laps: Int
    public let bestLap: Int?
    public let bestTime: Double?
    public let summary: SessionSummary?
}

public struct RaceLogEnvelope: Codable, Identifiable, Sendable {
    public var id: Int { localIndex }
    public let localIndex: Int
    public let hash: String
    public let revision: Int
    public let deleted: Bool
    public let updatedAt: Int
    public let trackName: String
    public let laps: Int
    public let bestLap: Int?
    public let bestTime: Double?
    public let payload: [String: JSONValue]?
}

public struct DeviceDetail: Codable, Sendable {
    public let device: CloudDevice
    public let tracks: [TrackEnvelope]
    public let sessions: [SessionEnvelope]
    public let logs: [RaceLogEnvelope]
    public let pendingCommands: [PendingCommand]
}

public struct PendingCommand: Codable, Sendable {
    public let id: Int
    public let type: String?
    public let kind: String?
    public let payload: String?
    public let status: String?
    public let createdAt: Int?
}

public enum JSONValue: Codable, Sendable {
    case string(String), number(Double), bool(Bool), object([String: JSONValue]), array([JSONValue]), null

    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let v = try? c.decode(String.self) { self = .string(v) }
        else if let v = try? c.decode(Bool.self) { self = .bool(v) }
        else if let v = try? c.decode(Double.self) { self = .number(v) }
        else if let v = try? c.decode([String: JSONValue].self) { self = .object(v) }
        else { self = .array(try c.decode([JSONValue].self)) }
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .string(let v): try c.encode(v)
        case .number(let v): try c.encode(v)
        case .bool(let v): try c.encode(v)
        case .object(let v): try c.encode(v)
        case .array(let v): try c.encode(v)
        case .null: try c.encodeNil()
        }
    }
}
