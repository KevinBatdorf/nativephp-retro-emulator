import XCTest
import RetroEmulator

/// PlayStation registration + media-path contract on the static iOS build.
/// The real disc boot (BIOS + .cue/.bin + swap) runs on Android hardware in
/// Ps1BootTest.kt — the core and pak code are shared; what iOS adds is the
/// static-link registration and these API seams.
final class Ps1Tests: XCTestCase {

    private var ctx: OpaquePointer!

    override func setUp() {
        super.setUp()
        ctx = ares_create()
    }

    override func tearDown() {
        ares_destroy(ctx)
        ctx = nil
        super.tearDown()
    }

    func testPs1IsCompiledIn() {
        let ids = String(cString: ares_supported_systems()).components(separatedBy: ",")
        XCTAssertTrue(ids.contains("ps1"), "'ps1' must be compiled in, got \(ids)")
    }

    func testPs1LoadsAndUsesMediaPath() {
        XCTAssertTrue(ares_load_system(ctx, "ps1", nil))
        XCTAssertTrue(ares_uses_media_path(ctx), "ps1 must load media by path")
    }

    func testByteLoadIsRejectedForDiscSystems() {
        XCTAssertTrue(ares_load_system(ctx, "ps1", nil))
        var bytes = [UInt8](repeating: 0, count: 2048)
        let result = bytes.withUnsafeBufferPointer {
            ares_load_rom(ctx, $0.baseAddress, $0.count, nil, nil, nil)
        }
        XCTAssertEqual(result, 0, "disc systems must reject the byte path pre-teardown")
    }

    func testMediaLoadWithoutBiosIsGated() {
        XCTAssertTrue(ares_load_system(ctx, "ps1", nil))
        // A cue that exists but no staged BIOS: the firmware gate must answer
        // before any teardown. Write a minimal cue to tmp.
        let cue = NSTemporaryDirectory() + "ps1-gate-test.cue"
        try? "FILE \"missing.bin\" BINARY\nTRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
            .write(toFile: cue, atomically: true, encoding: .utf8)
        let result = ares_load_media(ctx, cue, nil, nil, nil)
        XCTAssertEqual(result, -2, "expected BIOS_REQUIRED (-2) without firmware")
    }
}
