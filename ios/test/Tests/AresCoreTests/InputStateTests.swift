import RetroEmulator
import XCTest

/// ares_get_input — the read-back seam StickInputTest asserts through on
/// Android. Bits match the iOS EmulatorInput constants (kSnesButtons).
final class InputStateTests: XCTestCase {

    private var ctx: OpaquePointer!

    override func setUp() {
        super.setUp()
        ctx = ares_create()
        XCTAssertNotNil(ctx)
    }

    override func tearDown() {
        ares_destroy(ctx)
        ctx = nil
        super.tearDown()
    }

    func testInputMaskRoundTripsPerPort() {
        ares_set_input(ctx, 1, 0b0000_0101)
        ares_set_input(ctx, 2, 0b1000_0000)
        XCTAssertEqual(ares_get_input(ctx, 1), 0b0000_0101)
        XCTAssertEqual(ares_get_input(ctx, 2), 0b1000_0000)

        ares_set_input(ctx, 1, 0)
        XCTAssertEqual(ares_get_input(ctx, 1), 0)
        XCTAssertEqual(ares_get_input(ctx, 2), 0b1000_0000)
    }

    func testUnknownPortAndNullContextReadZero() {
        ares_set_input(ctx, 1, 0xFFF)
        XCTAssertEqual(ares_get_input(ctx, 3), 0)
        XCTAssertEqual(ares_get_input(nil, 1), 0)
    }
}
