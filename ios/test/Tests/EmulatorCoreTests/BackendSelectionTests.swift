import XCTest
import RetroEmulator

/// Backend selection through the seam on the static iOS build: an unnamed
/// boot runs the built-in engine (ares), an explicit engine is honored, an
/// unknown one fails to stage, and the discovery JSON lists availability
/// without suggesting a pick.
final class BackendSelectionTests: XCTestCase {
    private var ctx: OpaquePointer!

    override func setUp() {
        super.setUp()
        ctx = emu_create()
        emu_reset(ctx)
    }

    override func tearDown() {
        emu_reset(ctx)
        super.tearDown()
    }

    func testGbUnnamedBootsTheBuiltInEngine() {
        XCTAssertTrue(emu_load_system(ctx, "gb", nil, nil))
        XCTAssertEqual(String(cString: emu_get_backend_name(ctx)), "ares",
                       "an unnamed boot runs the built-in engine — every other engine is an explicit choice")
    }

    func testExplicitAresIsHonored() {
        XCTAssertTrue(emu_load_system(ctx, "gb", nil, "ares"))
        XCTAssertEqual(String(cString: emu_get_backend_name(ctx)), "ares")
    }

    func testUnknownBackendFailsToStage() {
        XCTAssertFalse(emu_load_system(ctx, "gb", nil, "quicknes"),
                       "an engine that does not serve gb must fail, not fall back")
    }

    func testSameBoyBootsARom() {
        let rom = Self.makeMinimalGbRom()
        XCTAssertTrue(emu_load_system(ctx, "gb", nil, "sameboy"))
        let ok = rom.withUnsafeBytes {
            emu_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, rom.count,
                         nil, nil, nil)
        }
        XCTAssertEqual(ok, 1, "sameboy must boot the synthetic ROM")

        // The DMG boot ROM scrolls the logo for ~4 s before handing control
        // to the cartridge; run long enough for the payload to execute.
        for _ in 0..<400 { _ = emu_tick(ctx) }

        var width: UInt32 = 0
        var height: UInt32 = 0
        var pixels = [UInt32](repeating: 0, count: 160 * 144)
        XCTAssertTrue(emu_get_frame(ctx, &pixels, pixels.count, &width, &height))
        XCTAssertEqual(width, 160)
        XCTAssertEqual(height, 144)

        var wram = [UInt8](repeating: 0, count: 4)
        XCTAssertEqual(emu_read_memory(ctx, 0xC000, &wram, 4), 4)
        XCTAssertEqual(Array(wram), [0xA5, 0x5A, 0xC3, 0x3C],
                       "the cartridge payload must have written its WRAM signature")
    }

    /// 32 KB no-MBC cartridge that passes the DMG boot ROM's logo + header
    /// checksum, writes A5 5A C3 3C to 0xC000, then spins. The logo bytes are
    /// the mandatory header content every licensed and homebrew ROM carries.
    private static func makeMinimalGbRom() -> [UInt8] {
        var rom = [UInt8](repeating: 0, count: 0x8000)

        // Entry point: nop; jp 0x0150.
        rom[0x100] = 0x00
        rom[0x101] = 0xC3
        rom[0x102] = 0x50
        rom[0x103] = 0x01

        let logo: [UInt8] = [
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
            0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
            0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
            0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
            0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
        ]
        rom.replaceSubrange(0x104..<0x134, with: logo)

        // Header checksum over 0x134-0x14C.
        var checksum: UInt8 = 0
        for address in 0x134...0x14C {
            checksum = checksum &- rom[address] &- 1
        }
        rom[0x14D] = checksum

        // Payload at 0x0150: write the signature to 0xC000-0xC003, spin.
        let payload: [UInt8] = [
            0x3E, 0xA5,             // ld a, 0xA5
            0xEA, 0x00, 0xC0,       // ld (0xC000), a
            0x3E, 0x5A,             // ld a, 0x5A
            0xEA, 0x01, 0xC0,       // ld (0xC001), a
            0x3E, 0xC3,             // ld a, 0xC3
            0xEA, 0x02, 0xC0,       // ld (0xC002), a
            0x3E, 0x3C,             // ld a, 0x3C
            0xEA, 0x03, 0xC0,       // ld (0xC003), a
            0x18, 0xFE,             // jr -2 (spin)
        ]
        rom.replaceSubrange(0x150..<(0x150 + payload.count), with: payload)
        return rom
    }

    func testBackendsJsonListsClaimantsAndSuggestsNothing() throws {
        let raw = String(cString: emu_get_backends_json())
        let json = try XCTUnwrap(
            JSONSerialization.jsonObject(with: Data(raw.utf8)) as? [String: Any]
        )
        let gb = try XCTUnwrap(json["gb"] as? [String: Any])
        let engines = try XCTUnwrap(gb["backends"] as? [String])
        XCTAssertTrue(engines.contains("ares") && engines.contains("sameboy"))
        // Availability only — no crowned pick anywhere in the discovery
        // surface; unnamed boots run the built-in engine.
        XCTAssertNil(gb["default"], "backendsJson must not suggest a default")
    }

    func testGbaUnnamedBootsTheBuiltInEngine() {
        XCTAssertTrue(emu_load_system(ctx, "gba", nil, nil))
        XCTAssertEqual(String(cString: emu_get_backend_name(ctx)), "ares",
                       "an unnamed boot runs the built-in engine — every other engine is an explicit choice")
    }

    func testMgbaBootsARom() {
        let rom = Self.makeMinimalGbaRom()
        XCTAssertTrue(emu_load_system(ctx, "gba", nil, "mgba"))
        let ok = rom.withUnsafeBytes {
            emu_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, rom.count,
                         nil, nil, nil)
        }
        XCTAssertEqual(ok, 1, "mgba must boot the synthetic ROM")

        for _ in 0..<60 { _ = emu_tick(ctx) }

        var width: UInt32 = 0
        var height: UInt32 = 0
        var pixels = [UInt32](repeating: 0, count: 240 * 160)
        XCTAssertTrue(emu_get_frame(ctx, &pixels, pixels.count, &width, &height))
        XCTAssertEqual(width, 240)
        XCTAssertEqual(height, 160)

        var ewram = [UInt8](repeating: 0, count: 2)
        XCTAssertEqual(emu_read_memory(ctx, 0x02000000, &ewram, 2), 2)
        XCTAssertEqual(Array(ewram), [0xA5, 0x5A],
                       "the cartridge payload must have written its EWRAM signature")
    }

    /// Minimal GBA cartridge: entry branch to 0xC0, an ARM payload that
    /// writes A5 5A to the start of EWRAM, then spins. mGBA's HLE BIOS jumps
    /// to the entry point without checking the header logo.
    private static func makeMinimalGbaRom() -> [UInt8] {
        var rom = [UInt8](repeating: 0, count: 0x1000)
        func put(_ word: UInt32, at offset: Int) {
            rom[offset]     = UInt8(word & 0xFF)
            rom[offset + 1] = UInt8((word >> 8) & 0xFF)
            rom[offset + 2] = UInt8((word >> 16) & 0xFF)
            rom[offset + 3] = UInt8((word >> 24) & 0xFF)
        }
        put(0xEA00002E, at: 0x00)   // b 0xC0
        put(0xE3A00402, at: 0xC0)   // mov r0, #0x02000000
        put(0xE3A010A5, at: 0xC4)   // mov r1, #0xA5
        put(0xE5C01000, at: 0xC8)   // strb r1, [r0]
        put(0xE3A0105A, at: 0xCC)   // mov r1, #0x5A
        put(0xE5C01001, at: 0xD0)   // strb r1, [r0, #1]
        put(0xEAFFFFFE, at: 0xD4)   // b . (spin)
        return rom
    }

}
