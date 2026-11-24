import Foundation

public enum BLEProtocol {
    public static let serviceUUID = UUID(uuidString: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")!
    public static let rxUUID = UUID(uuidString: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")!
    public static let txUUID = UUID(uuidString: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")!
    public static let deviceName = "RaceBrick"
    public static let headerSize = 15
    public static let typeFrame = 5
    public static let typeAlbum = 6

    public static func bulkPackets(type: UInt8, id: UInt8, data: Data, payloadCapacity: Int) -> [Data] {
        guard !data.isEmpty, payloadCapacity > 0 else { return [] }
        let parts = (data.count + payloadCapacity - 1) / payloadCapacity
        guard parts <= 4000 else { return [] }
        var out = [Data](); out.reserveCapacity(parts)
        for p in 0..<parts {
            let off = p * payloadCapacity
            let len = min(payloadCapacity, data.count - off)
            var pkt = Data(capacity: headerSize + len)
            pkt.append(0x4D) // M
            pkt.append(type)
            pkt.append(id)
            appendLE(UInt16(parts), to: &pkt)
            appendLE(UInt16(p), to: &pkt)
            appendLE(UInt32(data.count), to: &pkt)
            appendLE(UInt32(off), to: &pkt)
            pkt.append(data.subdata(in: off..<(off + len)))
            out.append(pkt)
        }
        return out
    }

    private static func appendLE(_ value: UInt16, to data: inout Data) {
        data.append(UInt8(value & 0xFF)); data.append(UInt8((value >> 8) & 0xFF))
    }
    private static func appendLE(_ value: UInt32, to data: inout Data) {
        for i in 0..<4 { data.append(UInt8((value >> (8 * UInt32(i))) & 0xFF)) }
    }
}

/// Minimal 16-color quantizer + RLE used by the existing 320x240 device frame protocol.
public enum FrameCodec {
    public static let width = 320
    public static let height = 240

    public static func palette565(_ palette: [UInt32]) -> [UInt16] {
        palette.map { c in
            let r = UInt16(((c >> 16) & 0xFF) * 31 / 255)
            let g = UInt16(((c >> 8) & 0xFF) * 63 / 255)
            let b = UInt16((c & 0xFF) * 31 / 255)
            return (r << 11) | (g << 5) | b
        }
    }

    public static func nearestIndex(_ argb: UInt32, palette: [UInt32]) -> UInt8 {
        let r = Int((argb >> 16) & 0xFF), g = Int((argb >> 8) & 0xFF), b = Int(argb & 0xFF)
        var best = 0, bestD = Int.max
        for (i, c) in palette.enumerated() {
            let dr = r - Int((c >> 16) & 0xFF)
            let dg = g - Int((c >> 8) & 0xFF)
            let db = b - Int(c & 0xFF)
            let d = dr * dr + dg * dg + db * db
            if d < bestD { bestD = d; best = i }
        }
        return UInt8(best)
    }

    public static func indexImage(width: Int, height: Int, argb: [UInt32], palette: [UInt32]) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Self.width * Self.height)
        guard width > 0, height > 0 else { return out }
        for y in 0..<Self.height {
            let sy = y * height / Self.height
            for x in 0..<Self.width {
                let sx = x * width / Self.width
                out[y * Self.width + x] = nearestIndex(argb[sy * width + sx], palette: palette)
            }
        }
        return out
    }

    public static func encodeRLE(width: UInt16, height: UInt16, indices: [UInt8], palette: [UInt16], frameId: UInt16 = 0) -> Data {
        var out = Data()
        appendLE(frameId, &out)
        appendLE(UInt32(0), &out) // x0
        appendLE(UInt32(0), &out) // y0
        appendLE(width, &out); appendLE(height, &out)
        appendLE(UInt16(0x8000 | 1000), &out)
        for c in palette.prefix(16) { appendLE(c, &out) }
        var i = 0
        while i < indices.count {
            let v = indices[i]
            var run = 1
            while i + run < indices.count, indices[i + run] == v, run < 271 { run += 1 }
            if run < 16 {
                out.append(UInt8((run - 1) << 4) | (v & 0x0F))
            } else {
                out.append(UInt8(0xF0) | (v & 0x0F))
                out.append(UInt8(run - 16))
            }
            i += run
        }
        return out
    }

    private static func appendLE(_ v: UInt16, _ d: inout Data) {
        d.append(UInt8(v & 0xFF)); d.append(UInt8(v >> 8))
    }
    private static func appendLE(_ v: UInt32, _ d: inout Data) {
        for i in 0..<4 { d.append(UInt8((v >> (8 * UInt32(i))) & 0xFF)) }
    }
}
