import Foundation

public enum CloudAPIError: LocalizedError {
    case http(Int, String)
    case decoding(String)
    case transport(String)
    public var errorDescription: String? {
        switch self {
        case .http(let code, let detail): return "HTTP \(code): \(detail)"
        case .decoding(let detail): return "数据解析失败: \(detail)"
        case .transport(let detail): return "网络错误: \(detail)"
        }
    }
}

public struct CloudAPI: Sendable {
    public static let defaultBaseURL = URL(string: "https://racebrick.tfworks.cn")!
    public let baseURL: URL
    private let session: URLSession

    public init(baseURL: URL = CloudAPI.defaultBaseURL) {
        self.baseURL = baseURL
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 20
        cfg.timeoutIntervalForResource = 90
        self.session = URLSession(configuration: cfg)
    }

    private var decoder: JSONDecoder {
        let d = JSONDecoder()
        d.keyDecodingStrategy = .convertFromSnakeCase
        return d
    }

    private func request<Out: Decodable>(
        _ method: String, _ path: String, token: String? = nil,
        body: [String: JSONValue]? = nil
    ) async throws -> Out {
        var components = URLComponents(url: baseURL, resolvingAgainstBaseURL: false)!
        let parts = path.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false)
        components.path = String(parts[0]).hasPrefix("/") ? String(parts[0]) : "/" + String(parts[0])
        if parts.count > 1 {
            components.percentEncodedQuery = String(parts[1])
        }
        var req = URLRequest(url: components.url!)
        req.httpMethod = method
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if let token { req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        if let body {
            req.httpBody = try JSONEncoder().encode(body)
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        do {
            let (data, response) = try await session.data(for: req)
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            if !(200..<300).contains(code) {
                let text = String(data: data, encoding: .utf8) ?? ""
                throw CloudAPIError.http(code, text.prefix(300).description)
            }
            do { return try decoder.decode(Out.self, from: data) }
            catch { throw CloudAPIError.decoding(error.localizedDescription) }
        } catch let e as CloudAPIError {
            throw e
        } catch {
            throw CloudAPIError.transport(error.localizedDescription)
        }
    }

    public func login(username: String, password: String) async throws -> AuthResponse {
        try await request("POST", "/api/platform/login", body: [
            "username": .string(username), "password": .string(password)
        ])
    }

    public func register(username: String, password: String) async throws -> AuthResponse {
        try await request("POST", "/api/platform/register", body: [
            "username": .string(username), "password": .string(password)
        ])
    }

    public func me(token: String) async throws -> CloudUser {
        let response: MeResponse = try await request("GET", "/api/platform/me", token: token)
        return response.user
    }

    public func devices(token: String) async throws -> [CloudDevice] {
        let r: DevicesResponse = try await request("GET", "/api/platform/devices", token: token)
        return r.devices
    }

    public func bind(uid: String, token: String) async throws {
        let _: EmptyResponse = try await request("POST", "/api/platform/bind", token: token,
                                                  body: ["device_uid": .string(uid)])
    }

    public func device(uid: String, token: String) async throws -> DeviceDetail {
        try await request("GET", "/api/platform/devices/\(uid)", token: token)
    }

    public func updateTrack(uid: String, localId: Int, token: String, fields: [String: JSONValue]) async throws {
        let _: EmptyResponse = try await request(
            "PATCH", "/api/platform/devices/\(uid)/tracks/\(localId)", token: token, body: fields)
    }

    public func publicTracks(limit: Int = 20, cursor: String? = nil) async throws -> PublicTracksResponse {
        var path = "/api/public/v1/tracks?limit=\(min(max(limit, 1), 50))"
        if let cursor, !cursor.isEmpty,
           let encoded = cursor.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) {
            path += "&cursor=\(encoded)"
        }
        return try await request("GET", path)
    }

    public func publicTrack(publicId: String) async throws -> PublicTrackResponse {
        let encoded = publicId.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? publicId
        return try await request("GET", "/api/public/v1/tracks/\(encoded)")
    }

    public func adminUsers(token: String) async throws -> [AdminUser] {
        let r: AdminUsersResponse = try await request("GET", "/api/platform/admin/users", token: token)
        return r.users
    }
}

public struct EmptyResponse: Codable, Sendable {
    public var ok: Bool?
    public var revision: Int?
}

public struct AdminUser: Codable, Identifiable, Sendable {
    public let id: Int
    public let username: String
    public let role: String
    public let createdAt: Int
    public let devices: Int
}
public struct AdminUsersResponse: Codable, Sendable { public let users: [AdminUser] }
