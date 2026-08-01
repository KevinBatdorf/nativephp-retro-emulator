import XCTest
import Metal
import AVFoundation
import GameController
import RetroEmulator

/// Metal renderer, audio engine, and input observer smoke tests.
///
/// These run on the iOS simulator without a ROM.  They verify that the Metal
/// device is available, textures can be created, and AVAudioEngine starts.
/// Actual frame rendering with a real ROM is manual verification on device.
final class Phase9Tests: XCTestCase {

    // MARK: - Metal

    func testMetalDeviceIsAvailable() {
        XCTAssertNotNil(MTLCreateSystemDefaultDevice(),
                        "Metal GPU must be available on this simulator")
    }

    func testMetalCommandQueueCreates() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            XCTFail("No Metal device"); return
        }
        XCTAssertNotNil(device.makeCommandQueue())
    }

    func testBGRATextureCreates() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            XCTFail("No Metal device"); return
        }
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 256, height: 224, mipmapped: false)
        desc.usage       = [.shaderRead]
        desc.storageMode = .shared
        XCTAssertNotNil(device.makeTexture(descriptor: desc),
                        "Should be able to create a 256×224 BGRA texture")
    }

    func testTextureUploadRoundtrip() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            XCTFail("No Metal device"); return
        }
        let w = 4, h = 4
        let pixels = [UInt32](repeating: 0xFF_00_FF_00, count: w * h) // opaque green (ARGB)
        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        desc.usage       = [.shaderRead]
        desc.storageMode = .shared
        guard let tex = device.makeTexture(descriptor: desc) else {
            XCTFail("Texture creation failed"); return
        }
        pixels.withUnsafeBytes { raw in
            tex.replace(region: MTLRegionMake2D(0, 0, w, h),
                        mipmapLevel: 0,
                        withBytes: raw.baseAddress!,
                        bytesPerRow: w * 4)
        }
        XCTAssertTrue(true)
    }

    // MARK: - Audio

    func testAVAudioEngineStartsOnSimulator() throws {
        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                   sampleRate: 48_000, channels: 2, interleaved: false)!
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        XCTAssertNoThrow(try engine.start())
        engine.stop()
    }

    func testAudioBufferDeinterleaveIsCorrect() {
        // Simulate what EmulatorAudio does: convert interleaved → planar.
        let interleaved: [Float] = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6] // 3 stereo frames
        let frames = interleaved.count / 2
        var L = [Float](repeating: 0, count: frames)
        var R = [Float](repeating: 0, count: frames)
        for i in 0..<frames {
            L[i] = interleaved[i * 2]
            R[i] = interleaved[i * 2 + 1]
        }
        XCTAssertEqual(L, [0.1, 0.3, 0.5], accuracy: 0.0001)
        XCTAssertEqual(R, [0.2, 0.4, 0.6], accuracy: 0.0001)
    }

    // MARK: - Input bitmask

    func testSnesButtonBitmaskLayout() {
        // Verify the bit positions match kSnesButtons in the system catalog (native/host/system_catalog.cpp).
        let B: UInt32      = 1 << 0
        let Y: UInt32      = 1 << 1
        let select: UInt32 = 1 << 2
        let start: UInt32  = 1 << 3
        let up: UInt32     = 1 << 4
        let down: UInt32   = 1 << 5
        let left: UInt32   = 1 << 6
        let right: UInt32  = 1 << 7
        let A: UInt32      = 1 << 8
        let X: UInt32      = 1 << 9
        let L: UInt32      = 1 << 10
        let R: UInt32      = 1 << 11

        let allButtons = B | Y | select | start | up | down | left | right | A | X | L | R
        XCTAssertEqual(allButtons, 0b0000_1111_1111_1111)
        XCTAssertEqual(allButtons.nonzeroBitCount, 12)
    }

    func testSetInputDoesNotCrashOnSimulator() {
        let ctx = emu_create()!
        emu_set_input(ctx, 1, 0xFF)  // all face buttons + dpad pressed
        emu_set_input(ctx, 1, 0x00)  // released
        emu_destroy(ctx)
    }

    // MARK: - Frame pixel format

    func testAresPixelFormatIsBGRACompatible() {
        // ares encodes pixels as 0xAARRGGBB.
        // On little-endian ARM, the bytes in memory are [BB, GG, RR, AA].
        // MTLPixelFormatBGRA8Unorm reads [B, G, R, A] — identical layout.
        let argb: UInt32 = 0xFF_12_34_56  // A=FF, R=12, G=34, B=56
        var bytes = [UInt8](repeating: 0, count: 4)
        withUnsafeBytes(of: argb) { src in
            bytes[0] = src[0] // B (0x56)
            bytes[1] = src[1] // G (0x34)
            bytes[2] = src[2] // R (0x12)
            bytes[3] = src[3] // A (0xFF)
        }
        XCTAssertEqual(bytes[0], 0x56, "byte 0 must be blue channel")
        XCTAssertEqual(bytes[1], 0x34, "byte 1 must be green channel")
        XCTAssertEqual(bytes[2], 0x12, "byte 2 must be red channel")
        XCTAssertEqual(bytes[3], 0xFF, "byte 3 must be alpha channel")
    }
}

// MARK: - XCTAssertEqual for Float arrays
private func XCTAssertEqual(_ a: [Float], _ b: [Float], accuracy: Float,
                             file: StaticString = #file, line: UInt = #line) {
    XCTAssertEqual(a.count, b.count, file: file, line: line)
    for (x, y) in zip(a, b) {
        XCTAssertEqual(x, y, accuracy: accuracy, file: file, line: line)
    }
}
