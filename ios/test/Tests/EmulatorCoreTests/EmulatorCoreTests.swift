import XCTest
import RetroEmulator

/// Xcframework smoke tests.
///
/// These tests verify that the static library links correctly and that the
/// basic lifecycle API is callable without crashing.  No ROM is required;
/// every assertion that depends on emulation state uses the expected fallback
/// values for an uninitialized context.
final class EmulatorCoreTests: XCTestCase {

    // MARK: - Lifecycle

    func testCreateReturnsNonNullContext() {
        let ctx = emu_create()
        XCTAssertNotNil(ctx, "emu_create() must return a valid context pointer")
        emu_destroy(ctx)
    }

    func testDoubleCreateReturnsSameContext() {
        let ctx1 = emu_create()
        let ctx2 = emu_create()
        XCTAssertEqual(ctx1, ctx2, "second emu_create() must return the existing context")
        emu_destroy(ctx1)
    }

    func testDestroyThenCreateSucceeds() {
        let ctx1 = emu_create()
        emu_destroy(ctx1)
        let ctx2 = emu_create()
        XCTAssertNotNil(ctx2)
        emu_destroy(ctx2)
    }

    // MARK: - Tick (no ROM loaded)

    func testTickWithoutRomReturnsFalse() {
        let ctx = emu_create()!
        let ticked = emu_tick(ctx)
        XCTAssertFalse(ticked, "emu_tick() must return false when no ROM is loaded")
        emu_destroy(ctx)
    }

    func testTickDoesNotCrashWithoutSystem() {
        let ctx = emu_create()!
        // Calling tick multiple times on an un-initialized context must not crash.
        for _ in 0..<5 { _ = emu_tick(ctx) }
        emu_destroy(ctx)
    }

    // MARK: - Frame / audio (no ROM loaded)

    func testGetFrameReturnsFalseWithoutRom() {
        let ctx = emu_create()!
        var w: UInt32 = 99
        var h: UInt32 = 99
        let hasFrame = emu_get_frame(ctx, nil, 0, &w, &h)
        XCTAssertFalse(hasFrame)
        XCTAssertEqual(w, 0)
        XCTAssertEqual(h, 0)
        emu_destroy(ctx)
    }

    func testReadAudioReturnsZeroWithoutRom() {
        let ctx = emu_create()!
        var buf = [Float](repeating: 0, count: 64)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            emu_read_audio(ctx, ptr.baseAddress, ptr.count)
        }
        XCTAssertEqual(count, 0)
        emu_destroy(ctx)
    }

    // MARK: - Metadata (no system loaded)

    func testGetRegionReturnsEmptyWithoutRom() {
        let ctx = emu_create()!
        let region = String(cString: emu_get_region(ctx))
        XCTAssertEqual(region, "")
        emu_destroy(ctx)
    }

    func testGetPortsJsonReturnsEmptyArrayWithoutSystem() {
        let ctx = emu_create()!
        let json = String(cString: emu_get_ports_json(ctx))
        XCTAssertEqual(json, "[]")
        emu_destroy(ctx)
    }

    // MARK: - Input (no crash)

    func testSetInputDoesNotCrash() {
        let ctx = emu_create()!
        emu_set_input(ctx, 1, 0xFF)
        emu_set_input(ctx, 2, 0x00)
        emu_destroy(ctx)
    }

    // MARK: - Pause / resume (no crash)

    func testPauseAndResumeDoNotCrash() {
        let ctx = emu_create()!
        emu_pause(ctx)
        emu_resume(ctx)
        emu_destroy(ctx)
    }

    // MARK: - Memory (no ROM loaded)

    func testReadMemoryFailsWithoutRom() {
        let ctx = emu_create()!
        var buf = [UInt8](repeating: 0, count: 4)
        let result = buf.withUnsafeMutableBufferPointer { ptr in
            emu_read_memory(ctx, 0x7E0010, ptr.baseAddress, Int32(ptr.count))
        }
        XCTAssertEqual(result, -1)
        emu_destroy(ctx)
    }
}
