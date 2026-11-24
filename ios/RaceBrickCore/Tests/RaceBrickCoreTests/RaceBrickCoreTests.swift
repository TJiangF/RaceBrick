import XCTest
@testable import RaceBrickCore

final class RaceBrickCoreTests: XCTestCase {
    func testPal16DecodeRoundTripHeader() throws {
        var data = Data()
        func u16(_ v: UInt16) { data.append(UInt8(v & 0xFF)); data.append(UInt8(v >> 8)) }
        u16(2); u16(1); u16(2)
        u16(0xF800); u16(0x001F)
        data.append(0x01) // two pixels color 0
        data.append(0x11) // two pixels color 1
        let img = try Pal16.decode(data)
        XCTAssertEqual(img.width, 2)
        XCTAssertEqual(img.height, 1)
        XCTAssertEqual(img.pixels, [0xFFFF0000, 0xFFFF0000])
    }

    func testBulkPacketHeader() {
        let packets = BLEProtocol.bulkPackets(type: 5, id: 7, data: Data(repeating: 0x41, count: 10), payloadCapacity: 4)
        XCTAssertEqual(packets.count, 3)
        XCTAssertEqual(packets[0].count, 19)
        XCTAssertEqual(packets[0][0], 0x4D)
        XCTAssertEqual(packets[0][1], 5)
        XCTAssertEqual(packets[0][2], 7)
    }

    func testTrackGeometry() {
        let p = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]]
        let b = TrackGeometry.bounds(p)!
        XCTAssertEqual(b.width, 1)
        XCTAssertEqual(TrackGeometry.point(p, fraction: 0.5)!.x, 1)
    }
}
