import Foundation

public struct Pal16Image: Sendable, Equatable {
    public let width: Int
    public let height: Int
    /// Premultiplied opaque ARGB values.
    public let pixels: [UInt32]
}

public enum Pal16Error: Error { case invalid(String) }

public enum Pal16 {
    /// Decode RaceBrick pal16: [w u16][h u16][paletteCount u16][palette u16 LE][RLE].
    public static func decode(_ data: Data) throws -> Pal16Image {
        let b = [UInt8](data)
        guard b.count >= 6 else { throw Pal16Error.invalid("header") }
        func u8(_ i: Int) -> Int { Int(b[i]) }
        func u16(_ i: Int) -> Int { u8(i) | (u8(i + 1) << 8) }
        let w = u16(0), h = u16(2), pc = u16(4)
        guard w > 0, h > 0, w <= 512, h <= 512, pc > 0, pc <= 16,
              b.count >= 6 + pc * 2, w * h <= 512 * 512 else {
            throw Pal16Error.invalid("dimensions")
        }
        var palette = [UInt32]()
        var p = 6
        for _ in 0..<pc {
            let c = u16(p); p += 2
            let r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31
            let rr = UInt32(r * 255 / 31), gg = UInt32(g * 255 / 63), bb = UInt32(bl * 255 / 31)
            palette.append(0xFF000000 | (rr << 16) | (gg << 8) | bb)
        }
        var out = [UInt32](repeating: 0xFF000000, count: w * h)
        var k = 0
        while p < b.count, k < out.count {
            let v = u8(p); p += 1
            var run = (v >> 4) + 1
            let idx = v & 15
            if v >> 4 == 15 {
                guard p < b.count else { break }
                run = 16 + u8(p); p += 1
            }
            let c = idx < palette.count ? palette[idx] : 0xFF000000
            let end = min(out.count, k + run)
            while k < end { out[k] = c; k += 1 }
        }
        guard k == out.count else { throw Pal16Error.invalid("short rle") }
        return Pal16Image(width: w, height: h, pixels: out)
    }
}
