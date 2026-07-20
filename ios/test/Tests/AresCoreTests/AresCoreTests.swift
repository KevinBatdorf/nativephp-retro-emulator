import XCTest
import RetroEmulator

/// Phase 8 — xcframework smoke tests.
///
/// These tests verify that the static library links correctly and that the
/// basic lifecycle API is callable without crashing.  No ROM is required;
/// every assertion that depends on emulation state uses the expected fallback
/// values for an uninitialized context.
final class AresCoreTests: XCTestCase {

    // MARK: - Lifecycle

    func testCreateReturnsNonNullContext() {
        let ctx = ares_create()
        XCTAssertNotNil(ctx, "ares_create() must return a valid context pointer")
        ares_destroy(ctx)
    }

    func testDoubleCreateReturnsSameContext() {
        let ctx1 = ares_create()
        let ctx2 = ares_create()
        XCTAssertEqual(ctx1, ctx2, "second ares_create() must return the existing context")
        ares_destroy(ctx1)
    }

    func testDestroyThenCreateSucceeds() {
        let ctx1 = ares_create()
        ares_destroy(ctx1)
        let ctx2 = ares_create()
        XCTAssertNotNil(ctx2)
        ares_destroy(ctx2)
    }

    // MARK: - Tick (no ROM loaded)

    func testTickWithoutRomReturnsFalse() {
        let ctx = ares_create()!
        let ticked = ares_tick(ctx)
        XCTAssertFalse(ticked, "ares_tick() must return false when no ROM is loaded")
        ares_destroy(ctx)
    }

    func testTickDoesNotCrashWithoutSystem() {
        let ctx = ares_create()!
        // Calling tick multiple times on an un-initialized context must not crash.
        for _ in 0..<5 { _ = ares_tick(ctx) }
        ares_destroy(ctx)
    }

    // MARK: - Frame / audio (no ROM loaded)

    func testGetFrameReturnsFalseWithoutRom() {
        let ctx = ares_create()!
        var w: UInt32 = 99
        var h: UInt32 = 99
        let hasFrame = ares_get_frame(ctx, nil, 0, &w, &h)
        XCTAssertFalse(hasFrame)
        XCTAssertEqual(w, 0)
        XCTAssertEqual(h, 0)
        ares_destroy(ctx)
    }

    func testReadAudioReturnsZeroWithoutRom() {
        let ctx = ares_create()!
        var buf = [Float](repeating: 0, count: 64)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            ares_read_audio(ctx, ptr.baseAddress, ptr.count)
        }
        XCTAssertEqual(count, 0)
        ares_destroy(ctx)
    }

    // MARK: - Metadata (no system loaded)

    func testGetRegionReturnsEmptyWithoutRom() {
        let ctx = ares_create()!
        let region = String(cString: ares_get_region(ctx))
        XCTAssertEqual(region, "")
        ares_destroy(ctx)
    }

    func testGetPortsJsonReturnsEmptyArrayWithoutSystem() {
        let ctx = ares_create()!
        let json = String(cString: ares_get_ports_json(ctx))
        XCTAssertEqual(json, "[]")
        ares_destroy(ctx)
    }

    // MARK: - Input (no crash)

    func testSetInputDoesNotCrash() {
        let ctx = ares_create()!
        ares_set_input(ctx, 1, 0xFF)
        ares_set_input(ctx, 2, 0x00)
        ares_destroy(ctx)
    }

    // MARK: - Pause / resume (no crash)

    func testPauseAndResumeDoNotCrash() {
        let ctx = ares_create()!
        ares_pause(ctx)
        ares_resume(ctx)
        ares_destroy(ctx)
    }

    // MARK: - Memory (no ROM loaded)

    func testReadMemoryFailsWithoutRom() {
        let ctx = ares_create()!
        var buf = [UInt8](repeating: 0, count: 4)
        let result = buf.withUnsafeMutableBufferPointer { ptr in
            ares_read_memory(ctx, 0x7E0010, ptr.baseAddress, Int32(ptr.count))
        }
        XCTAssertEqual(result, -1)
        ares_destroy(ctx)
    }
}
