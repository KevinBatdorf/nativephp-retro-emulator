import XCTest
import RetroEmulator

/// On-device N64 render proof — parallel-RDP on the statically linked
/// MoltenVK, running on REAL Metal (the simulator's driver can't host it;
/// see ios/test N64RenderTests). Boots rdpqdemo.z64 and asserts a non-black
/// frame comes back through ares_get_frame, then saves it as an attachment
/// for eyes-on review.
final class N64DeviceRenderTests: XCTestCase {

    /// The plugin's default path: no env override, parallel-RDP's specialized
    /// pipelines with ubershader fallback while they compile.
    func testN64BootsAndRendersViaMoltenVK() throws {
        try runRenderCheck(ubershader: nil)
    }

    /// Forced-ubershader still renders (the Android/Adreno mode) — proves the
    /// slow-but-universal path stays viable on MoltenVK too.
    func testN64RendersWithForcedUbershader() throws {
        try runRenderCheck(ubershader: true)
    }

    private func runRenderCheck(ubershader: Bool?) throws {
        guard let romPath = Bundle(for: Self.self).path(forResource: "rdpqdemo", ofType: "z64") else {
            throw XCTSkip("rdpqdemo.z64 not staged in Tests/Resources — skipping")
        }

        let ctx = ares_create()
        defer { ares_destroy(ctx) }

        XCTAssertTrue(ares_load_system(ctx, "n64", nil, nil), "n64 must stage")

        // The RDP reads the env at load_rom; tests share a process, so clear
        // any leftover override before opting in. strtol: "1" = ubershader.
        unsetenv("PARALLEL_RDP_UBERSHADER")
        if let ubershader {
            setenv("PARALLEL_RDP_UBERSHADER", ubershader ? "1" : "0", 1)
        }

        let rom = try Data(contentsOf: URL(fileURLWithPath: romPath))
        let loaded = rom.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count,
                          nil, "", "")
        }
        // 1 == LOAD_OK. Vulkan/RDP init happens during boot, so this line
        // alone exercises MoltenVK device creation.
        XCTAssertEqual(loaded, 1, "N64 ROM must boot — MoltenVK/parallel-RDP init failed?")

        // Interpreter-paced: tick until a non-black frame shows up. The
        // rdpqdemo splash renders within the first few seconds of emulated
        // time; 600 frames is comfortably past it.
        var buffer = [UInt32](repeating: 0, count: 1024 * 1024)  // capacity in pixels
        var w: UInt32 = 0, h: UInt32 = 0
        var sawContent = false

        for _ in 0..<600 {
            _ = ares_tick(ctx)
            let got = buffer.withUnsafeMutableBufferPointer {
                ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
            }
            guard got, w > 0, h > 0 else { continue }
            let count = Int(w) * Int(h)
            var i = 0
            while i < count {
                let px = buffer[i]  // ARGB8888
                if (px & 0x00FF_FFFF) > 0x0010_1010 {
                    sawContent = true
                    break
                }
                i += 97  // prime stride avoids column aliasing
            }
            if sawContent { break }
        }

        XCTAssertTrue(sawContent,
            "parallel-RDP produced only black frames (\(w)x\(h)) — Vulkan render path broken")

        // Attach the frame for the eyes-on gate.
        if sawContent, let img = Self.makeImage(from: buffer, width: Int(w), height: Int(h)) {
            let att = XCTAttachment(image: img)
            att.name = (ubershader == true) ? "n64-rdpqdemo-frame-ubershader" : "n64-rdpqdemo-frame"
            att.lifetime = .keepAlways
            add(att)
        }
    }

    private static func makeImage(from argb: [UInt32], width: Int, height: Int) -> UIImage? {
        // ARGB8888 (a in the high byte) → CGImage.
        var pixels = argb
        let data = Data(bytes: &pixels, count: width * height * 4)
        guard let provider = CGDataProvider(data: data as CFData),
              let cg = CGImage(width: width, height: height,
                               bitsPerComponent: 8, bitsPerPixel: 32,
                               bytesPerRow: width * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                               provider: provider, decode: nil,
                               shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return UIImage(cgImage: cg)
    }
}
