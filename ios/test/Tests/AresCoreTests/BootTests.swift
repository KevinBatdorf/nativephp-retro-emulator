import XCTest
import RetroEmulator

/// Boot tests — exercise the full emulation path:
/// ares_load_system → ares_load_rom → ares_tick → ares_get_frame.
///
/// System firmware (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS) is
/// embedded in the native library since Phase 11 — no fixtures needed.
///
/// ROM: synthetic 32 KB LoROM with a valid header and checksum.
///      No Nintendo IP — just an infinite SEI/BRA loop so the CPU keeps running.
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

    func testLoadSystemSucceeds() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil),
                      "ares_load_system(\"sfc\") must return true")
    }

    func testLoadSystemIsIdempotent() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        // Second call should be a no-op and return true.
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
    }

    func testLoadSystemFailsWithUnknownId() {
        // "saturn" has no ares core in this tree, so it is never registered —
        // an id absent from the registry must be rejected. (n64 used to serve
        // this role but is now a compiled, supported iOS system.)
        XCTAssertFalse(ares_load_system(ctx, "saturn", nil),
                       "systems not compiled into this build must be rejected")
    }

    func testN64IsSupported() {
        // N64 is compiled into the iOS build (paraLLEl-RDP on MoltenVK).
        let ids = String(cString: ares_supported_systems()).components(separatedBy: ",")
        XCTAssertTrue(ids.contains("n64"), "'n64' must be supported, got \(ids)")
        XCTAssertTrue(ares_load_system(ctx, "n64", nil),
                      "ares_load_system(\"n64\") must return true")
    }

    func testSupportedSystemsAreReported() {
        let ids = String(cString: ares_supported_systems()).components(separatedBy: ",")
        for id in ["fc", "sfc", "gb", "md"] {
            XCTAssertTrue(ids.contains(id), "'\(id)' must be supported, got \(ids)")
        }
    }

    // MARK: - Multi-system load (Phase 11)

    func testEveryCompiledSystemLoads() {
        // Each system loads its core and reports controller buttons. Sequential
        // load/teardown in one process mirrors the Android Phase 11 test.
        for id in ["fc", "sfc", "gb", "md"] {
            let localCtx = ares_create()
            XCTAssertTrue(ares_load_system(localCtx, id, nil), "\(id): loadSystem failed")
            let json = String(cString: ares_get_ports_json(localCtx))
            XCTAssertTrue(json.contains("buttons"), "\(id): unexpected ports JSON \(json)")
            ares_destroy(localCtx)
        }
        // Re-create for tearDown symmetry.
        ctx = ares_create()
    }

    // MARK: - ROM load

    func testLoadRomSucceedsWithSyntheticLoRom() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))

        let rom = Self.makeMinimalLoRom()
        let ok  = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertTrue(ok, "ares_load_rom() must accept a valid synthetic LoROM")
    }

    func testLoadRomFailsWithoutSystem() {
        let rom = Self.makeMinimalLoRom()
        let ok  = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertFalse(ok, "ares_load_rom() must fail when no system is staged")
    }

    func testLoadRomFailsWithTooSmallData() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))

        let tiny = Data(count: 100)
        let ok   = tiny.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertFalse(ok)
    }

    // MARK: - Tick + frame

    func testTickReturnsTrueAfterBoot() {
        boot()
        XCTAssertTrue(ares_tick(ctx), "ares_tick() must return true when a ROM is running")
    }

    func testFrameIsProduced() {
        boot()
        let (w, h) = tickUntilFrame()
        XCTAssertGreaterThan(w, 0, "frame width must be > 0")
        XCTAssertGreaterThan(h, 0, "frame height must be > 0")
    }

    func testFrameDimensionsAreSnesNative() {
        boot()
        let (w, h) = tickUntilFrame()
        // The performance PPU renders the full 564×242 canvas (incl. overscan
        // borders); the accuracy PPU crops to 256/512 × 224/239.
        XCTAssertTrue(w == 256 || w == 512 || w == 564, "unexpected width \(w)")
        XCTAssertTrue(h == 224 || h == 239 || h == 242, "unexpected height \(h)")
    }

    func testFramePixelsAreCopiedToBuffer() {
        boot()
        var (w, h) = tickUntilFrame()
        XCTAssertGreaterThan(w * h, 0, "no frame produced")

        var pixels = [UInt32](repeating: 0xDEADBEEF, count: Int(w * h))
        pixels.withUnsafeMutableBufferPointer {
            _ = ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
        }
        // All pixels were overwritten (no 0xDEADBEEF sentinel remaining).
        XCTAssertFalse(pixels.contains(0xDEADBEEF),
                       "ares_get_frame() must overwrite all pixel slots")
    }

    /// The screen node delivers frames from its own worker thread, so the first
    /// frame lands asynchronously — tick and poll like a real frontend would.
    private func tickUntilFrame(_ maxTicks: Int = 120) -> (w: UInt32, h: UInt32) {
        var w: UInt32 = 0, h: UInt32 = 0
        for _ in 0..<maxTicks {
            _ = ares_tick(ctx)
            if ares_get_frame(ctx, nil, 0, &w, &h), w > 0 { return (w, h) }
            usleep(2_000)
        }
        return (w, h)
    }

    func testMultipleTicksDoNotCrash() {
        boot()
        for _ in 0..<10 {
            XCTAssertTrue(ares_tick(ctx))
        }
    }

    // MARK: - Region

    func testGetRegionAfterRomLoad() {
        boot()
        let region = String(cString: ares_get_region(ctx))
        // Our synthetic ROM declares country $01 (USA → NTSC).
        XCTAssertFalse(region.isEmpty, "region must not be empty after ROM load")
    }

    // MARK: - Refresh rate hint

    func testRefreshRateHintZeroBeforeSystemLoad() {
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 0.0,
                       "hint must be 0 before a system loads")
    }

    func testRefreshRateHintArrivesAtBootAndStagingLeavesItZero() {
        // Under ROM-first boot (plan 4b) screens register inside ares_load_rom
        // — staging must leave the hint at 0. Expected value follows the core
        // formula at the pinned submodule (sfc/ppu/ppu.cpp:47, NTSC 262 lines).
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 0.0)
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 0.0,
                       "staging must not boot a core")
        boot()
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 60.09848,
                       accuracy: 0.001, "sfc NTSC refresh hint")
    }

    func testPalRomBootsPalCore() {
        // The 4b headline: a PAL ROM must boot the PAL system variant, not
        // just report PAL. The header country byte drives the sfc analyzer
        // ($02 = Europe → PAL, sfc_pak region detection); PAL SFC refresh =
        // cpuFrequency(PAL colorburst · 4.8) / (1364 · 312) ≈ 50.0070
        // (sfc/ppu/ppu.cpp, 312-line PAL frame).
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let rom = Self.makeMinimalLoRom(region: .pal)
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertTrue(ok, "PAL LoROM must load")
        XCTAssertEqual(String(cString: ares_get_region(ctx)), "PAL")
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 50.0070,
                       accuracy: 0.001, "PAL boot must run PAL timing")
    }

    func testRegionOverrideWinsOverAnalysis() {
        // Explicit region override (dev knows best — junk homebrew headers):
        // an NTSC-headered ROM forced to PAL must boot PAL.
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let rom = Self.makeMinimalLoRom()
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, "PAL", nil) == 1
        }
        XCTAssertTrue(ok)
        XCTAssertEqual(String(cString: ares_get_region(ctx)), "PAL")
        XCTAssertEqual(ares_get_refresh_rate_hint(ctx), 50.0070, accuracy: 0.001)
    }

    // MARK: - Ports JSON

    func testGetPortsJsonAfterSystemLoad() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let json = String(cString: ares_get_ports_json(ctx))
        XCTAssertTrue(json.contains("buttons"), "ports JSON must list buttons")
    }

    // MARK: - Helpers

    private func boot() {
        let rom = Self.makeMinimalLoRom()
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertTrue(ok, "boot() failed at ares_load_rom()")
    }

    // MARK: - Battery-save persistence (Phase 13)

    func testBatterySaveRoundTrip() throws {
        // A LoROM with 8KB battery RAM: the LOROM-RAM board reads save.ram at
        // connect and System::save() writes it back to the pak. Seeding a
        // pattern from disk and flushing must round-trip it through the board.
        let prefix = NSTemporaryDirectory() + "phase13-\(UUID().uuidString)"
        let savePath = prefix + ".save.ram"
        let pattern = Data(repeating: 0xAB, count: 8192)
        try pattern.write(to: URL(fileURLWithPath: savePath))

        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let rom = Self.makeMinimalLoRom(withSram: true)
        let ok = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, prefix, nil, nil) == 1
        }
        XCTAssertTrue(ok, "SRAM LoROM must load")

        _ = ares_tick(ctx)
        XCTAssertTrue(ares_flush_saves(ctx), "flush must succeed with a save prefix")

        let flushed = try Data(contentsOf: URL(fileURLWithPath: savePath))
        XCTAssertEqual(flushed.count, 8192)
        XCTAssertEqual(flushed, pattern, "seeded SRAM must survive the board round-trip")
        try? FileManager.default.removeItem(atPath: savePath)
    }

    func testFlushWithoutPrefixReturnsFalse() {
        XCTAssertTrue(ares_load_system(ctx, "sfc", nil))
        let rom = Self.makeMinimalLoRom()
        _ = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, nil, nil, nil) == 1
        }
        XCTAssertFalse(ares_flush_saves(ctx), "no prefix → nothing persisted")
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
    enum RomRegion { case ntsc, pal }

    static func makeMinimalLoRom(withSram: Bool = false, region: RomRegion = .ntsc) -> Data {
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
        // $7FD6 — ROM type: $02 = ROM+RAM+battery, $00 = ROM only.
        rom[hb + 0x26] = withSram ? 0x02 : 0x00
        // $7FD7 — ROM size byte: $05 = 32 KB.
        rom[hb + 0x27] = 0x05
        // $7FD8 — RAM size: 1 << value KB (mia's 0x400 << n), so $03 = 8 KB.
        rom[hb + 0x28] = withSram ? 0x03 : 0x00
        // $7FD9 — Country: USA ($01) = NTSC; Europe ($02) = PAL (the sfc
        // analyzer's region detection reads this byte).
        rom[hb + 0x29] = region == .pal ? 0x02 : 0x01
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
