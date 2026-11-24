import Foundation
import UIKit
import RaceBrickCore

struct SatelliteOverlay: @unchecked Sendable {
    let image: UIImage
    /// Normalized track-space bounds of the image.
    let bounds: CGRect
    let spanM: Double
}

enum SatelliteService {
    private static let baseURL = URL(string: "https://racebrick.tfworks.cn/api/v1/nav")!
    private static let geoRadius = 6_378_137.0
    private static let lock = NSLock()
    private static var sequence = 1

    static func load(track: TrackEnvelope) async -> SatelliteOverlay? {
        guard let payload = track.payload,
              let points = payload.points, points.count >= 2,
              let nscale = payload.nscale, nscale > 1e-9,
              let glat0 = payload.glat0, let glon0 = payload.glon0,
              let bounds = TrackGeometry.bounds(points) else { return nil }
        let spanM = max(bounds.width, bounds.height) / nscale * 1.12
        guard spanM.isFinite, spanM > 0 else { return nil }
        let span = min(max(spanM, 60), 5_000)
        let cx = bounds.centerX, cy = bounds.centerY
        let mx = (cx - (payload.nox ?? 0)) / nscale
        let my = (cy - (payload.noy ?? 0)) / nscale
        let lat = glat0 + (my / geoRadius) * 180 / .pi
        let lon = glon0 + (mx / (geoRadius * cos(glat0 * .pi / 180))) * 180 / .pi
        lock.lock(); let seq = sequence; sequence += 1; lock.unlock()
        let body: [String: Any] = [
            "device_id": "ios-racebrick", "seq": seq,
            "lng": lon, "lat": lat, "size_m": span,
            "heading": 0, "size": 256, "width": 256, "height": 256,
            "zoom": 1.0, "format": "pal16", "mode": "satellite", "coord": "wgs84",
        ]
        var req = URLRequest(url: baseURL)
        req.httpMethod = "POST"
        req.timeoutInterval = 20
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
        req.httpBody = try? JSONSerialization.data(withJSONObject: body)
        do {
            let (data, response) = try await URLSession.shared.data(for: req)
            guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else { return nil }
            guard let decoded = try? Pal16.decode(data), let image = decoded.uiImage else { return nil }
            let actualSpan = Double(http.value(forHTTPHeaderField: "X-Nav-Span-M") ?? "") ?? span
            let half = actualSpan * nscale / 2
            let rect = CGRect(x: cx - half, y: cy - half, width: half * 2, height: half * 2)
            return SatelliteOverlay(image: image, bounds: rect, spanM: actualSpan)
        } catch {
            return nil
        }
    }
}

private extension Pal16Image {
    var uiImage: UIImage? {
        var bytes = [UInt8](repeating: 255, count: width * height * 4)
        for i in 0..<pixels.count {
            let p = pixels[i]
            let o = i * 4
            bytes[o] = UInt8((p >> 16) & 0xFF)
            bytes[o + 1] = UInt8((p >> 8) & 0xFF)
            bytes[o + 2] = UInt8(p & 0xFF)
            bytes[o + 3] = 255
        }
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        guard let cg = CGImage(width: width, height: height, bitsPerComponent: 8,
                               bitsPerPixel: 32, bytesPerRow: width * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue:
                                   CGImageAlphaInfo.premultipliedLast.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: true,
                               intent: .defaultIntent) else { return nil }
        return UIImage(cgImage: cg)
    }
}
