import Foundation
import CoreLocation
import MapKit
import UIKit
import Combine
import RaceBrickCore

@MainActor
final class NavFrameStreamer: NSObject, ObservableObject {
    @Published private(set) var streaming = false
    @Published private(set) var status = "地图推流未启动"
    @Published private(set) var locationText = "--"
    private let location = CLLocationManager()
    private var timer: Timer?
    private weak var ble: BLEManager?
    private var lastCoordinate: CLLocationCoordinate2D?
    private var rendering = false

    private let palette: [UInt32] = [
        0xFF101820, 0xFF1F2937, 0xFF374151, 0xFF4B5563,
        0xFF6B7280, 0xFF9CA3AF, 0xFFD1D5DB, 0xFFF9FAFB,
        0xFF0E7490, 0xFF06B6D4, 0xFF22C55E, 0xFF84CC16,
        0xFFEAB308, 0xFFF97316, 0xFFEF4444, 0xFF8B5CF6,
    ]

    override init() {
        super.init()
        location.delegate = self
        location.desiredAccuracy = kCLLocationAccuracyBest
        location.distanceFilter = 3
    }

    func start(ble: BLEManager) {
        self.ble = ble
        let auth = location.authorizationStatus
        switch auth {
        case .notDetermined:
            location.requestWhenInUseAuthorization()
        case .denied, .restricted:
            self.status = "定位权限被拒绝"
            return
        default:
            break
        }
        streaming = true
        status = "地图推流启动，等待定位…"
        location.startUpdatingLocation()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.renderAndSend() }
        }
    }

    func stop() {
        streaming = false
        timer?.invalidate(); timer = nil
        location.stopUpdatingLocation()
        status = "地图推流已停止"
    }

    private func renderAndSend() async {
        guard streaming, !rendering, let coordinate = lastCoordinate,
              let ble, ble.connected else { return }
        rendering = true
        defer { rendering = false }
        let options = MKMapSnapshotter.Options()
        options.region = MKCoordinateRegion(center: coordinate,
                                            latitudinalMeters: 700,
                                            longitudinalMeters: 700)
        options.size = CGSize(width: FrameCodec.width, height: FrameCodec.height)
        options.mapType = .standard
        options.showsBuildings = false
        options.pointOfInterestFilter = .excludingAll
        do {
            let snapshot = try await withCheckedThrowingContinuation { (cont: CheckedContinuation<MKMapSnapshotter.Snapshot, Error>) in
                MKMapSnapshotter(options: options).start { snap, error in
                    if let snap { cont.resume(returning: snap) }
                    else { cont.resume(throwing: error ?? NSError(domain: "map", code: 1)) }
                }
            }
            guard let argb = argbPixels(snapshot.image) else { return }
            let idx = FrameCodec.indexImage(width: snapshot.image.cgImage?.width ?? FrameCodec.width,
                                            height: snapshot.image.cgImage?.height ?? FrameCodec.height,
                                            argb: argb, palette: palette)
            let raw = FrameCodec.encodeRLE(width: UInt16(FrameCodec.width),
                                           height: UInt16(FrameCodec.height),
                                           indices: idx,
                                           palette: FrameCodec.palette565(palette))
            ble.sendBulk(type: UInt8(BLEProtocol.typeFrame), data: raw)
            status = "已发送 \(raw.count / 1024) KB 地图帧"
        } catch {
            status = "地图截图失败：\(error.localizedDescription)"
        }
    }

    private func argbPixels(_ image: UIImage) -> [UInt32]? {
        guard let cg = image.cgImage else { return nil }
        let w = cg.width, h = cg.height
        let count = w * h * 4
        let data = UnsafeMutablePointer<UInt8>.allocate(capacity: count)
        defer { data.deallocate() }
        data.initialize(repeating: 0, count: count)
        guard let ctx = CGContext(data: data, width: w, height: h,
                                  bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        var out = [UInt32](repeating: 0, count: w * h)
        for i in 0..<(w * h) {
            let r = UInt32(data[i * 4]), g = UInt32(data[i * 4 + 1]), b = UInt32(data[i * 4 + 2])
            out[i] = 0xFF000000 | (r << 16) | (g << 8) | b
        }
        return out
    }
}

extension NavFrameStreamer: CLLocationManagerDelegate {
    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in
            if manager.authorizationStatus == .authorizedWhenInUse ||
                manager.authorizationStatus == .authorizedAlways {
                if streaming { location.startUpdatingLocation() }
            }
        }
    }
    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        Task { @MainActor in
            self.lastCoordinate = loc.coordinate
            self.locationText = String(format: "%.6f, %.6f", loc.coordinate.latitude, loc.coordinate.longitude)
        }
    }
}
