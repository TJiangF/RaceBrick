import SwiftUI
import RaceBrickCore

struct TrackMapCanvas: View {
    let track: TrackEnvelope
    @State private var satellite: SatelliteOverlay?

    var body: some View {
        GeometryReader { geo in
            Canvas { context, size in
                guard let payload = track.payload,
                      let points = payload.points, points.count >= 2,
                      let bounds = TrackGeometry.bounds(points) else { return }
                let w = Double(size.width), h = Double(size.height)
                let pad = 14.0
                let scale = min((w - pad * 2) / bounds.width,
                                (h - pad * 2) / bounds.height)
                let ox = (w - bounds.width * scale) / 2
                let oy = (h - bounds.height * scale) / 2
                func map(_ p: [Double]) -> CGPoint {
                    CGPoint(x: CGFloat(ox + (p[0] - bounds.minX) * scale),
                            y: CGFloat(h - oy - (p[1] - bounds.minY) * scale))
                }
                if let satellite {
                    let a = map([Double(satellite.bounds.minX), Double(satellite.bounds.maxY)])
                    let b = map([Double(satellite.bounds.maxX), Double(satellite.bounds.minY)])
                    let rect = CGRect(x: min(a.x, b.x), y: min(a.y, b.y),
                                      width: abs(b.x - a.x), height: abs(b.y - a.y))
                    context.opacity = 0.5
                    context.draw(Image(uiImage: satellite.image), in: rect)
                    context.opacity = 1
                }
                var path = Path()
                path.move(to: map(points[0]))
                for p in points.dropFirst() { path.addLine(to: map(p)) }
                context.stroke(path, with: .color(.cyan), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))
                drawStartLine(context: context, payload: payload, points: points, bounds: bounds,
                              scale: scale, ox: ox, oy: oy, size: size)
            }
        }
        .background(Color(red: 0.035, green: 0.07, blue: 0.1))
        .clipShape(RoundedRectangle(cornerRadius: 10))
        .task(id: track.id) {
            satellite = await SatelliteService.load(track: track)
        }
    }

    private func drawStartLine(context: GraphicsContext, payload: TrackPayload,
                               points: [[Double]], bounds: TrackBounds,
                               scale: Double, ox: Double, oy: Double, size: CGSize) {
        let h = Double(size.height)
        func map(_ p: CGPointLike) -> CGPoint {
            CGPoint(x: CGFloat(ox + (p.x - bounds.minX) * scale),
                    y: CGFloat(h - oy - (p.y - bounds.minY) * scale))
        }
        guard let base = TrackGeometry.point(points, fraction: payload.startF ?? 0),
              let tangent = TrackGeometry.tangent(points, fraction: payload.startF ?? 0) else { return }
        var tx = tangent.x, ty = -tangent.y
        let len = max(hypot(tx, ty), 1e-9); tx /= len; ty /= len
        if payload.reverse == true { tx = -tx; ty = -ty }
        let nx = -ty, ny = tx
        let normLen = TrackGeometry.normalizedPolylineLength(points)
        let pxPerM = (payload.lengthM ?? 0) > 0 && normLen > 0
            ? scale * normLen / (payload.lengthM ?? 1) : 0
        let off = (payload.startOffset ?? 0) * pxPerM
        let center = map(base)
        let x = center.x + CGFloat(nx * off), y = center.y + CGFloat(ny * off)
        let half = CGFloat(max((payload.startWidth ?? payload.width ?? 5) * pxPerM / 2, 3))
        var line = Path()
        line.move(to: CGPoint(x: x - CGFloat(nx) * half, y: y - CGFloat(ny) * half))
        line.addLine(to: CGPoint(x: x + CGFloat(nx) * half, y: y + CGFloat(ny) * half))
        let color: Color = payload.reverse == true ? .orange : .red
        context.stroke(line, with: .color(color), lineWidth: 4)
        let tip = CGPoint(x: x + CGFloat(tx * 14), y: y + CGFloat(ty * 14))
        let back = CGPoint(x: x + CGFloat(tx * 3), y: y + CGFloat(ty * 3))
        var arrow = Path()
        arrow.move(to: tip)
        arrow.addLine(to: CGPoint(x: back.x + CGFloat(nx * 7), y: back.y + CGFloat(ny * 7)))
        arrow.move(to: tip)
        arrow.addLine(to: CGPoint(x: back.x - CGFloat(nx * 7), y: back.y - CGFloat(ny * 7)))
        context.stroke(arrow, with: .color(color), lineWidth: 3)
    }
}
