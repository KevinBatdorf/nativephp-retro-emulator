import RetroEmulator
import XCTest

/// Save-state and AV-option coverage the Phase 13/14 suites skipped —
/// mirrors the Android StateAndOptionsTest.
final class StateAndOptionsTests: XCTestCase {

    private var ctx: OpaquePointer!

    /// WRAM the synthetic ROM's idle loop never touches.
    private let scratch: UInt32 = 0x7E1F00

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

    private func boot() {
        XCTAssertTrue(ares_load_system(ctx, "sfc"))
        let rom = BootTests.makeMinimalLoRom()
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil)
        }
        XCTAssertTrue(ok)
        for _ in 0..<5 { _ = ares_tick(ctx) }
    }

    func testStateSaveRoundTripsWramContents() {
        boot()
        let path = NSTemporaryDirectory() + "state-\(UUID().uuidString).bst"
        defer { try? FileManager.default.removeItem(atPath: path) }

        var marker: [UInt8] = [0x5A, 0x3C]
        ares_write_memory(ctx, scratch, &marker, 2)
        _ = ares_tick(ctx)
        XCTAssertTrue(ares_state_save(ctx, path), "stateSave must succeed while running")

        var zeros: [UInt8] = [0x00, 0x00]
        ares_write_memory(ctx, scratch, &zeros, 2)
        _ = ares_tick(ctx)
        XCTAssertTrue(ares_state_load(ctx, path), "stateLoad must succeed")

        var out = [UInt8](repeating: 0, count: 2)
        XCTAssertEqual(ares_read_memory(ctx, scratch, &out, 2), 2)
        XCTAssertEqual(out, [0x5A, 0x3C], "WRAM must match the snapshot")
    }

    func testStateLoadFailsForMissingFile() {
        boot()
        XCTAssertFalse(
            ares_state_load(ctx, NSTemporaryDirectory() + "does-not-exist.bst"),
            "loading a missing state must fail, not crash"
        )
    }

    func testAudioAndVideoOptionsApplyWhileRunning() {
        boot()

        ares_set_audio(ctx, 0.5, -1.0)
        ares_set_audio(ctx, 1.0, 1.0)
        ares_set_video(ctx, 0.5, 0.5, 1.5, true, true)
        for _ in 0..<5 { _ = ares_tick(ctx) }

        var width: UInt32 = 0
        var height: UInt32 = 0
        var buffer = [UInt32](repeating: 0, count: 1024 * 1024)
        _ = ares_get_frame(ctx, &buffer, buffer.count, &width, &height)
        XCTAssertGreaterThan(width, 0, "frame must still be produced with options applied")
    }
}
