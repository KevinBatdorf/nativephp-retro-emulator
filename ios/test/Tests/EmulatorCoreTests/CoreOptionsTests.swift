import RetroEmulator
import XCTest

/// Per-system emulation toggles reach the core through the generic setBoolean
/// scan, only on cores that declare the node. Deep Black Boost is SNES's — the
/// user-visible change this phase ships (plan.md item 6): ares' sfc core
/// defaults it on, the wrapper turns it off unless the dev opts in. Mirrors the
/// Android CoreOptionsTest (GB coverage lives there — iOS boots only SFC).
final class CoreOptionsTests: XCTestCase {

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

    func testDeepBlackBoostTogglesOnSnesAndIsAbsentElsewhere() {
        XCTAssertTrue(emu_load_system(ctx, "sfc", nil))
        let rom = BootTests.makeMinimalLoRom()
        let ok = rom.withUnsafeBytes {
            emu_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertTrue(ok)
        for _ in 0..<5 { _ = emu_tick(ctx) }

        // ares' sfc core registers Deep Black Boost defaulting on.
        XCTAssertEqual(emu_get_core_boolean(ctx, "deepBlackBoost"), 1,
                       "sfc core should boot with Deep Black Boost on (ares default)")
        emu_set_core_boolean(ctx, "deepBlackBoost", false)
        XCTAssertEqual(emu_get_core_boolean(ctx, "deepBlackBoost"), 0, "false must apply")
        emu_set_core_boolean(ctx, "deepBlackBoost", true)
        XCTAssertEqual(emu_get_core_boolean(ctx, "deepBlackBoost"), 1, "true must apply")

        // Keys the sfc core doesn't declare, and an unknown key, report absent
        // so the applier no-ops instead of erroring.
        XCTAssertEqual(emu_get_core_boolean(ctx, "colorEmulation"), -1, "sfc has no Color Emulation node")
        XCTAssertEqual(emu_get_core_boolean(ctx, "interframeBlending"), -1, "sfc has no Interframe Blending node")
        XCTAssertEqual(emu_get_core_boolean(ctx, "bogusKey"), -1, "unknown key is absent")
    }
}
