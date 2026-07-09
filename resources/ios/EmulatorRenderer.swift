import UIKit
import Metal
import MetalKit
import RetroEmulator

/// UIView subclass that is the NativePHP ios_renderer for the "emulator" component.
///
/// NativePHP instantiates this and adds it to the native layout tree.
/// It owns the Metal view, the ares context, audio, and input.
/// The emulation loop runs on a background thread; Metal displays the latest frame
/// at the device's native refresh rate.
final class EmulatorRenderer: UIView {

    // MARK: - Public API (called by EmulatorFunctions bridge in Phase 10)

    func loadSystem(_ iplRom: Data, boardsBml: Data) -> Bool {
        let ok = iplRom.withUnsafeBytes { ipl in
            boardsBml.withUnsafeBytes { bml in
                ares_load_system(ctx,
                                 ipl.bindMemory(to: UInt8.self).baseAddress, ipl.count,
                                 bml.bindMemory(to: UInt8.self).baseAddress, bml.count)
            }
        }
        return ok
    }

    func loadRom(_ romData: Data) -> Bool {
        let ok = romData.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        if ok { startLoop() }
        return ok
    }

    func pause()  { ares_pause(ctx) }
    func resume() { ares_resume(ctx) }

    func stop() {
        stopLoop()
        audio.stop()
        ares_destroy(ctx)
        // ctx is now a dangling pointer — nil it out
        // (AresContext is a singleton; Phase 10 will re-create via ares_create())
    }

    func setInput(port: Int32, bits: UInt32) {
        ares_set_input(ctx, port, bits)
    }

    // MARK: - Private

    private let ctx: OpaquePointer
    private let metalView: MTKView
    private let metalRenderer: MetalFrameRenderer
    private let audio = EmulatorAudio()
    private let input: EmulatorInput

    private var loopThread: Thread?
    private var running = false

    override init(frame: CGRect) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("Metal not available")
        }
        ctx = ares_create()!

        metalView = MTKView(frame: .zero, device: device)
        metalView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.clearColor = MTLClearColorMake(0, 0, 0, 1)
        // Renderer drives its own draw calls from the emulation loop.
        metalView.isPaused = true
        metalView.enableSetNeedsDisplay = false

        metalRenderer = MetalFrameRenderer(device: device, pixelFormat: .bgra8Unorm)!
        metalView.delegate = metalRenderer

        audio.ctx = ctx
        input = EmulatorInput(ctx: ctx)

        super.init(frame: frame)

        addSubview(metalView)
        metalView.frame = bounds

        input.startObserving()
    }

    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

    deinit {
        stopLoop()
        input.stopObserving()
        audio.stop()
    }

    // MARK: - Emulation loop

    private func startLoop() {
        guard !running else { return }
        running = true
        try? audio.start()

        let t = Thread { [weak self] in self?.emulationLoop() }
        t.name = "com.retroemulator.emu-loop"
        t.qualityOfService = .userInteractive
        t.start()
        loopThread = t
    }

    private func stopLoop() {
        running = false
        loopThread = nil
    }

    private func emulationLoop() {
        while running {
            ares_tick(ctx)

            // Copy latest frame and push to renderer.
            var w: UInt32 = 0, h: UInt32 = 0
            ares_get_frame(ctx, nil, 0, &w, &h)
            if w > 0 && h > 0 {
                var pixels = [UInt32](repeating: 0, count: Int(w * h))
                pixels.withUnsafeMutableBufferPointer {
                    _ = ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
                }
                metalRenderer.submitFrame(pixels, width: Int(w), height: Int(h))
                metalView.draw()
            }
        }
    }
}
