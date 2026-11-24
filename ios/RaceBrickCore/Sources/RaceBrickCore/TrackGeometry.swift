import Foundation

public struct TrackBounds: Sendable, Equatable {
    public let minX: Double, maxX: Double, minY: Double, maxY: Double
    public var centerX: Double { (minX + maxX) / 2 }
    public var centerY: Double { (minY + maxY) / 2 }
    public var width: Double { maxX - minX }
    public var height: Double { maxY - minY }
}

public enum TrackGeometry {
    public static func bounds(_ points: [[Double]]) -> TrackBounds? {
        guard let first = points.first, first.count >= 2 else { return nil }
        var minX = first[0], maxX = first[0], minY = first[1], maxY = first[1]
        for p in points where p.count >= 2 {
            minX = min(minX, p[0]); maxX = max(maxX, p[0])
            minY = min(minY, p[1]); maxY = max(maxY, p[1])
        }
        guard maxX > minX, maxY > minY else { return nil }
        return TrackBounds(minX: minX, maxX: maxX, minY: minY, maxY: maxY)
    }

    public static func normalizedPolylineLength(_ points: [[Double]]) -> Double {
        guard points.count > 1 else { return 0 }
        var total = 0.0
        for i in 1..<points.count {
            guard points[i].count >= 2, points[i - 1].count >= 2 else { continue }
            total += hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1])
        }
        return total
    }

    public static func point(_ points: [[Double]], fraction: Double) -> CGPointLike? {
        guard points.count >= 2, let first = points.first, first.count >= 2 else { return nil }
        let f = min(max(fraction, 0), 0.9999)
        let pos = f * Double(points.count - 1)
        let i = min(max(Int(pos), 0), points.count - 2)
        let frac = pos - Double(i)
        let a = points[i], b = points[i + 1]
        guard a.count >= 2, b.count >= 2 else { return nil }
        return CGPointLike(x: a[0] + (b[0] - a[0]) * frac, y: a[1] + (b[1] - a[1]) * frac)
    }

    public static func tangent(_ points: [[Double]], fraction: Double) -> CGPointLike? {
        guard let a = point(points, fraction: max(0, fraction - 0.004)),
              let b = point(points, fraction: min(0.9999, fraction + 0.004)) else { return nil }
        let dx = b.x - a.x, dy = b.y - a.y
        let len = hypot(dx, dy)
        return len > 1e-9 ? CGPointLike(x: dx / len, y: dy / len) : nil
    }
}

public struct CGPointLike: Sendable, Equatable {
    public var x: Double
    public var y: Double
    public init(x: Double, y: Double) { self.x = x; self.y = y }
}
