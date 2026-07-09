import XCTest
import RetroEmulator

/// Phase 9 boot tests — the first tests that exercise the full emulation path:
/// ares_load_system → ares_load_rom → ares_tick → ares_get_frame.
///
/// ROM: synthetic 32 KB LoROM with a valid header and checksum.
///      No Nintendo IP — just an infinite SEI/BRA loop so the CPU keeps running.
/// IPL: zero-filled. SPC700 audio won't boot, but CPU + PPU run fine.
/// BML: boards.bml from the ares submodule (open-source, bundled as a test fixture).
final class BootTests: XCTestCase {

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

    // MARK: - System load

    func testLoadSystemSucceedsWithBoardsBml() throws {
        let bml = try boardsBml()
        let ipl = Data(count: 64)   // zero-filled — SPC700 won't execute, CPU/PPU will

        let ok = bml.withUnsafeBytes { bmlPtr in
            ipl.withUnsafeBytes { iplPtr in
                ares_load_system(
                    ctx,
                    iplPtr.bindMemory(to: UInt8.self).baseAddress, ipl.count,
                    bmlPtr.bindMemory(to: UInt8.self).baseAddress, bml.count
                )
            }
        }
        XCTAssertTrue(ok, "ares_load_system() must return true with valid boards.bml")
    }

    func testLoadSystemIsIdempotent() throws {
        let (ipl, bml) = try fixtures()
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))
        // Second call should be a no-op and return true.
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))
    }

    func testLoadSystemFailsWithEmptyBml() {
        let ipl  = Data(count: 64)
        let bml  = Data()
        let ok   = bml.withUnsafeBytes { bmlPtr in
            ipl.withUnsafeBytes { iplPtr in
                ares_load_system(ctx,
                    iplPtr.bindMemory(to: UInt8.self).baseAddress, ipl.count,
                    bmlPtr.bindMemory(to: UInt8.self).baseAddress, bml.count)
            }
        }
        XCTAssertFalse(ok)
    }

    // MARK: - ROM load

    func testLoadRomSucceedsWithSyntheticLoRom() throws {
        let (ipl, bml) = try fixtures()
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))

        let rom = Self.makeMinimalLoRom()
        let ok  = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        XCTAssertTrue(ok, "ares_load_rom() must accept a valid synthetic LoROM")
    }

    func testLoadRomFailsWithoutSystem() {
        let rom = Self.makeMinimalLoRom()
        let ok  = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        XCTAssertFalse(ok, "ares_load_rom() must fail when no system is loaded")
    }

    func testLoadRomFailsWithTooSmallData() throws {
        let (ipl, bml) = try fixtures()
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))

        let tiny = Data(count: 100)
        let ok   = tiny.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        XCTAssertFalse(ok)
    }

    // MARK: - Tick + frame

    func testTickReturnsTrueAfterBoot() throws {
        try boot()
        XCTAssertTrue(ares_tick(ctx), "ares_tick() must return true when a ROM is running")
    }

    func testFrameIsProducedAfterOneTick() throws {
        try boot()
        _ = ares_tick(ctx)

        var w: UInt32 = 0, h: UInt32 = 0
        let hasFrame = ares_get_frame(ctx, nil, 0, &w, &h)
        XCTAssertTrue(hasFrame, "ares_get_frame() must return true after one tick")
        XCTAssertGreaterThan(w, 0, "frame width must be > 0")
        XCTAssertGreaterThan(h, 0, "frame height must be > 0")
    }

    func testFrameDimensionsAreSnesNative() throws {
        try boot()
        _ = ares_tick(ctx)

        var w: UInt32 = 0, h: UInt32 = 0
        _ = ares_get_frame(ctx, nil, 0, &w, &h)
        // SNES outputs 256 or 512 wide, 224 or 239 tall depending on display mode.
        XCTAssertTrue(w == 256 || w == 512, "width must be 256 or 512, got \(w)")
        XCTAssertTrue(h == 224 || h == 239, "height must be 224 or 239, got \(h)")
    }

    func testFramePixelsAreCopiedToBuffer() throws {
        try boot()
        _ = ares_tick(ctx)

        var w: UInt32 = 0, h: UInt32 = 0
        _ = ares_get_frame(ctx, nil, 0, &w, &h)

        var pixels = [UInt32](repeating: 0xDEADBEEF, count: Int(w * h))
        pixels.withUnsafeMutableBufferPointer {
            _ = ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
        }
        // All pixels were overwritten (no 0xDEADBEEF sentinel remaining).
        XCTAssertFalse(pixels.contains(0xDEADBEEF),
                       "ares_get_frame() must overwrite all pixel slots")
    }

    func testMultipleTicksDoNotCrash() throws {
        try boot()
        for _ in 0..<10 {
            XCTAssertTrue(ares_tick(ctx))
        }
    }

    // MARK: - Region

    func testGetRegionAfterRomLoad() throws {
        try boot()
        let region = String(cString: ares_get_region(ctx))
        // Our synthetic ROM declares country $01 (USA → NTSC).
        XCTAssertFalse(region.isEmpty, "region must not be empty after ROM load")
    }

    // MARK: - Ports JSON

    func testGetPortsJsonAfterSystemLoad() throws {
        let (ipl, bml) = try fixtures()
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))
        let json = String(cString: ares_get_ports_json(ctx))
        XCTAssertTrue(json.contains("Controller Port"), "ports JSON must list controller ports")
    }

    // MARK: - Helpers

    private func fixtures() throws -> (ipl: Data, bml: Data) {
        (Data(count: 64), try boardsBml())
    }

    @discardableResult
    private func loadSystem(ipl: Data, bml: Data) -> Bool {
        bml.withUnsafeBytes { bmlPtr in
            ipl.withUnsafeBytes { iplPtr in
                ares_load_system(ctx,
                    iplPtr.bindMemory(to: UInt8.self).baseAddress, ipl.count,
                    bmlPtr.bindMemory(to: UInt8.self).baseAddress, bml.count)
            }
        }
    }

    private func boot() throws {
        let (ipl, bml) = try fixtures()
        let rom = Self.makeMinimalLoRom()
        XCTAssertTrue(loadSystem(ipl: ipl, bml: bml))
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        XCTAssertTrue(ok, "boot() failed at ares_load_rom()")
    }

    private func boardsBml() throws -> Data {
        guard let url = Bundle.module.url(forResource: "boards", withExtension: "bml",
                                          subdirectory: "Fixtures") else {
            throw XCTSkip("boards.bml fixture not found in test bundle")
        }
        return try Data(contentsOf: url)
    }

    // MARK: - Minimal synthetic LoROM

    /// Builds a valid 32 KB LoROM that passes ares header detection.
    ///
    /// Layout:
    ///   $0000  78        SEI   — chosen opcode scores +8 in scoreHeader
    ///   $0001  80 FD     BRA   -3 → back to $8000 (infinite loop)
    ///   $7FB0+ header    LoROM internal header
    ///   $7FFC  00 80     emulation-mode reset vector → $8000
    ///   $7FDC-$7FDF      checksum complement + checksum
    static func makeMinimalLoRom() -> Data {
        let size = 0x8000  // 32 KB
        var rom  = [UInt8](repeating: 0, count: size)

        // Boot code at $0000 (mapped to address $8000 in LoROM bank 0).
        rom[0x0000] = 0x78  // SEI  — scores +8 in scoreHeader
        rom[0x0001] = 0x80  // BRA
        rom[0x0002] = 0xFD  // -3   → $8003 - 3 = $8000 (loops forever)

        // LoROM internal header base: $7FB0.
        let hb = 0x7FB0

        // Title at hb+$10 (21 bytes).
        let title = Array("ARES TEST ROM        ".utf8)
        for (i, b) in title.prefix(21).enumerated() { rom[hb + 0x10 + i] = b }

        // $7FD5 — map mode: LoROM ($20).
        rom[hb + 0x25] = 0x20
        // $7FD6 — ROM type: ROM only ($00).
        rom[hb + 0x26] = 0x00
        // $7FD7 — ROM size byte: $05 = 32 KB.
        rom[hb + 0x27] = 0x05
        // $7FD8 — RAM size: none.
        rom[hb + 0x28] = 0x00
        // $7FD9 — Country: USA ($01).
        rom[hb + 0x29] = 0x01
        // $7FDA — Developer ID.
        rom[hb + 0x2A] = 0x00
        // $7FDB — Version.
        rom[hb + 0x2B] = 0x00

        // Emulation-mode reset vector at $7FFC-$7FFD → $8000.
        rom[0x7FFC] = 0x00
        rom[0x7FFD] = 0x80

        // Zero checksum bytes before computing.
        rom[hb + 0x2C] = 0x00; rom[hb + 0x2D] = 0x00  // complement
        rom[hb + 0x2E] = 0x00; rom[hb + 0x2F] = 0x00  // checksum

        var sum: UInt16 = 0
        for b in rom { sum = sum &+ UInt16(b) }

        rom[hb + 0x2E] = UInt8(sum & 0xFF)
        rom[hb + 0x2F] = UInt8(sum >> 8)
        let comp = ~sum
        rom[hb + 0x2C] = UInt8(comp & 0xFF)
        rom[hb + 0x2D] = UInt8(comp >> 8)

        return Data(rom)
    }
}
