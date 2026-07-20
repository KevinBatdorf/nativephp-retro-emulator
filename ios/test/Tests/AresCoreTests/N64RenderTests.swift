import XCTest
import RetroEmulator

/// N64 end-to-end render check — parallel-RDP running on the statically
/// linked MoltenVK (Vulkan-over-Metal). This is the proof the Vulkan path
/// actually renders, not just links: boot a real homebrew ROM and assert a
/// non-black frame comes back through ares_get_frame.
///
/// Needs a ROM the repo doesn't ship. Pass it via the test runner:
///   TEST_RUNNER_N64_TEST_ROM=/abs/path/rdpqdemo.z64 xcodebuild test …
/// Skips cleanly when unset (same convention as Android's SufamiTest media).
final class N64RenderTests: XCTestCase {

    func testN64BootsAndRendersViaMoltenVK() throws {
        #if targetEnvironment(simulator)
        // The simulator's Metal driver can't host parallel-RDP: MTLSimDriver
        // aborts on the RDRAM host-pointer import (xpc_shmem API misuse), and
        // with the import disabled it still asserts on linear textures over
        // non-private buffers. Real Metal has neither restriction — this test
        // is a device test.
        throw XCTSkip("N64/MoltenVK render check needs a real device (MTLSimDriver restrictions)")
        #else
        try runN64RenderCheck()
        #endif
    }

    private func runN64RenderCheck() throws {
        // ROM lookup: bundled test resource first (device runs can't see the
        // host filesystem), then the env var (sim/host runs), else skip.
        // Provide it by dropping rdpqdemo.z64 into Tests/AresCoreTests/TestMedia/
        // (gitignored — media stays out of the repo) or via
        // TEST_RUNNER_N64_TEST_ROM=/abs/path xcodebuild test …
        var romPath = Bundle.module.path(forResource: "rdpqdemo", ofType: "z64", inDirectory: "TestMedia")
        if romPath == nil {
            romPath = ProcessInfo.processInfo.environment["N64_TEST_ROM"]
        }
        guard let romPath, FileManager.default.fileExists(atPath: romPath) else {
            throw XCTSkip("no bundled rdpqdemo.z64 and N64_TEST_ROM unset — skipping N64 render check")
        }

        let ctx = ares_create()
        defer { ares_destroy(ctx) }

        XCTAssertTrue(ares_load_system(ctx, "n64", nil, nil), "n64 must stage")

        let rom = try Data(contentsOf: URL(fileURLWithPath: romPath))
        let loaded = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count,
                          nil, "", "")
        }
        // 1 == LOAD_OK. A Vulkan init failure surfaces here (RDP init happens
        // during boot), so this line alone already exercises MoltenVK.
        XCTAssertEqual(loaded, 1, "N64 ROM must boot — MoltenVK/parallel-RDP init failed?")

        // Interpreter-paced: tick until a non-black frame shows up. The
        // rdpqdemo splash renders within the first few seconds of emulated
        // time; 600 frames (~10s) is comfortably past it.
        var buffer = [UInt32](repeating: 0, count: 1024 * 1024)  // capacity in pixels
        var w: UInt32 = 0, h: UInt32 = 0
        var sawContent = false

        for _ in 0..<600 {
            _ = ares_tick(ctx)
            let got = buffer.withUnsafeMutableBufferPointer {
                ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
            }
            guard got, w > 0, h > 0 else { continue }
            // Sample the frame: any pixel with a lit RGB channel = content.
            let count = Int(w) * Int(h)
            var i = 0
            while i < count {
                let px = buffer[i]  // ARGB8888
                if (px & 0x00FF_FFFF) > 0x0010_1010 {
                    sawContent = true
                    break
                }
                i += 97  // stride through the frame, prime to avoid column aliasing
            }
            if sawContent { break }
        }

        XCTAssertTrue(sawContent,
            "parallel-RDP produced only black frames (\(w)x\(h)) — Vulkan render path broken")
    }
}
