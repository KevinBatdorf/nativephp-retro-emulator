import RetroEmulator
import XCTest

/// emu_get_input — the read-back seam StickInputTest asserts through on
/// Android. Bits match the iOS EmulatorInput constants (kSnesButtons).
final class InputStateTests: XCTestCase {

    private var ctx: OpaquePointer!

    override func setUp() {
        super.setUp()
        ctx = emu_create()
        XCTAssertNotNil(ctx)
    }

    override func tearDown() {
        emu_destroy(ctx)
        ctx = nil
        super.tearDown()
    }

    func testInputMaskRoundTripsPerPort() {
        emu_set_input(ctx, 1, 0b0000_0101)
        emu_set_input(ctx, 2, 0b1000_0000)
        XCTAssertEqual(emu_get_input(ctx, 1), 0b0000_0101)
        XCTAssertEqual(emu_get_input(ctx, 2), 0b1000_0000)

        emu_set_input(ctx, 1, 0)
        XCTAssertEqual(emu_get_input(ctx, 1), 0)
        XCTAssertEqual(emu_get_input(ctx, 2), 0b1000_0000)
    }

    func testUnknownPortAndNullContextReadZero() {
        emu_set_input(ctx, 1, 0xFFF)
        XCTAssertEqual(emu_get_input(ctx, 3), 0)
        XCTAssertEqual(emu_get_input(nil, 1), 0)
    }
}
