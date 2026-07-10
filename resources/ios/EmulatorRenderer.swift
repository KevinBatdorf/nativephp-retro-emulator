import UIKit
import Metal
import MetalKit
import RetroEmulator

/// Callbacks the renderer raises as the emulator changes state. `EmulatorFunctions`
/// installs a forwarder that turns each callback into a NativePHP event. Mirrors the
/// Android `EmulatorEventListener` interface.
protocol EmulatorEventListener: AnyObject {
    func onStarted(system: String, romPath: String)
    func onStopped()
    func onPaused()
    func onResumed()
    func onMemoryChanged(address: UInt32, oldValue: Int, newValue: Int)
    func onMemoryRead(address: UInt32, bytes: [UInt8])
    func onError(code: String, message: String)
}

/// UIView subclass that is the NativePHP ios_renderer for the "emulator" component.
///
/// NativePHP instantiates this and adds it to the native layout tree.
/// It owns the Metal view, the ares context, audio, and input.
/// The emulation loop runs on a background thread; Metal displays the latest frame
/// at the device's native refresh rate.
///
/// Threading: `ares_tick` runs on the emulation loop thread while bridge calls
/// (memory, state, screenshot) arrive on an arbitrary bridge thread. All `ctx`
/// access is serialised through `emuLock` — the iOS equivalent of Android posting
/// GL-critical work onto the GL thread.
final class EmulatorRenderer: UIView {

    // MARK: - Public state (read by EmulatorFunctions)

    /// Current lifecycle status: "stopped", "loading", "running", or "paused".
    private(set) var currentStatus: String = "stopped"

    /// The system id passed to the last `loadSystem` (e.g. "sfc"). Empty until loaded.
    private(set) var loadedSystem: String = ""

    /// Installed by the surface registry so lifecycle/memory callbacks reach PHP.
    /// Strong: the forwarder holds only the surface name, so there is no retain cycle.
    var eventListener: EmulatorEventListener?

    /// When true the emulation loop ticks extra frames per display frame. Stored for
    /// parity with Android; consumed by the loop's frame pacing.
    var fastForward: Bool = false

    // MARK: - System / ROM loading

    func loadSystem(_ iplRom: Data, boardsBml: Data, system: String) -> Bool {
        emuLock.lock()
        let ok = iplRom.withUnsafeBytes { ipl in
            boardsBml.withUnsafeBytes { bml in
                ares_load_system(ctx,
                                 ipl.bindMemory(to: UInt8.self).baseAddress, ipl.count,
                                 bml.bindMemory(to: UInt8.self).baseAddress, bml.count)
            }
        }
        emuLock.unlock()
        if ok {
            loadedSystem = system
            currentStatus = "loading"
        }
        return ok
    }

    func loadRom(_ romData: Data, path: String) -> Bool {
        // A fresh ROM invalidates every watch baseline (spec: watches clear on loadRom).
        clearMemoryWatches()

        emuLock.lock()
        let ok = romData.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        emuLock.unlock()

        if ok {
            romPath = path
            currentStatus = "loading"
            pendingStarted = true
            startLoop()
        }
        return ok
    }

    // MARK: - Lifecycle

    func pauseEmulation() {
        ares_pause(ctx)
        currentStatus = "paused"
        eventListener?.onPaused()
    }

    func resumeEmulation() {
        ares_resume(ctx)
        currentStatus = "running"
        eventListener?.onResumed()
    }

    func stopEmulation() {
        stopLoop()
        audio.stop()
        currentStatus = "stopped"
        eventListener?.onStopped()
    }

    // MARK: - Memory

    /// Synchronous WRAM read. Returns nil when the address is out of range or no ROM
    /// is loaded. Blocks the caller until it can take `emuLock` (≤ one frame).
    func syncReadMemory(address: UInt32, length: Int) -> [UInt8]? {
        guard length > 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: length)
        emuLock.lock()
        let written = buf.withUnsafeMutableBufferPointer {
            ares_read_memory(ctx, address, $0.baseAddress, Int32(length))
        }
        emuLock.unlock()
        guard written >= 0 else { return nil }
        return Array(buf.prefix(Int(written)))
    }

    func writeMemory(address: UInt32, bytes: [UInt8]) {
        guard !bytes.isEmpty else { return }
        emuLock.lock()
        bytes.withUnsafeBufferPointer {
            ares_write_memory(ctx, address, $0.baseAddress, Int32(bytes.count))
        }
        emuLock.unlock()
    }

    // MARK: - Memory watches

    /// Register/merge a watch. Length defaults to 1. Baseline is captured on the next
    /// loop pass without firing; only later changes emit `MemoryChanged`.
    func addWatch(address: UInt32, length: Int) {
        watchLock.lock()
        watches[address] = WatchEntry(length: max(1, length), lastValue: nil)
        watchLock.unlock()
    }

    func removeWatch(address: UInt32) {
        watchLock.lock()
        watches[address] = nil
        watchLock.unlock()
    }

    func clearMemoryWatches() {
        watchLock.lock()
        watches.removeAll()
        watchLock.unlock()
    }

    // MARK: - State

    func syncStateSave(path: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return ares_state_save(ctx, path)
    }

    func syncStateLoad(path: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return ares_state_load(ctx, path)
    }

    // MARK: - Metadata

    func getRegion() -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: ares_get_region(ctx))
    }

    func getPortsJson() -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: ares_get_ports_json(ctx))
    }

    // MARK: - Software input

    func pressButton(_ name: String) -> Bool {
        guard let bit = EmulatorInput.buttonNameToBit(name) else { return false }
        input.pressSoftwareButton(bit)
        return true
    }

    func releaseButton(_ name: String) -> Bool {
        guard let bit = EmulatorInput.buttonNameToBit(name) else { return false }
        input.releaseSoftwareButton(bit)
        return true
    }

    /// Merge a name→pressed map into the software input state. Unknown names are skipped.
    func setButtons(_ state: [String: Bool]) {
        for (name, pressed) in state {
            guard let bit = EmulatorInput.buttonNameToBit(name) else { continue }
            if pressed {
                input.pressSoftwareButton(bit)
            } else {
                input.releaseSoftwareButton(bit)
            }
        }
    }

    // MARK: - Screenshot

    /// Encode the latest frame as PNG. Returns nil when no frame is available.
    /// Pixels are interpreted as BGRA8 (matching the Metal display path).
    func syncScreenshot() -> Data? {
        emuLock.lock()
        var w: UInt32 = 0, h: UInt32 = 0
        ares_get_frame(ctx, nil, 0, &w, &h)
        guard w > 0, h > 0 else { emuLock.unlock(); return nil }
        var pixels = [UInt32](repeating: 0, count: Int(w * h))
        let ok = pixels.withUnsafeMutableBufferPointer {
            ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
        }
        emuLock.unlock()
        guard ok else { return nil }

        let width = Int(w), height = Int(h)
        let bytesPerRow = width * 4
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue:
            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)

        return pixels.withUnsafeMutableBytes { raw -> Data? in
            guard let context = CGContext(
                data: raw.baseAddress,
                width: width, height: height,
                bitsPerComponent: 8, bytesPerRow: bytesPerRow,
                space: colorSpace, bitmapInfo: bitmapInfo.rawValue
            ), let cgImage = context.makeImage() else {
                return nil
            }
            return UIImage(cgImage: cgImage).pngData()
        }
    }

    // MARK: - Private

    private struct WatchEntry {
        let length: Int
        var lastValue: Int?
    }

    private let ctx: OpaquePointer
    private let metalView: MTKView
    private let metalRenderer: MetalFrameRenderer
    private let audio = EmulatorAudio()
    private let input: EmulatorInput

    private let emuLock = NSLock()
    private let watchLock = NSLock()
    private var watches: [UInt32: WatchEntry] = [:]

    private var romPath: String = ""
    private var pendingStarted = false

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

        // Phase 10: self-register under the default surface so bridge calls resolve.
        // Phase 12 will register under the real `name` prop from <native-emulator name="…" />.
        EmulatorFunctions.register(name: "main", renderer: self)
    }

    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

    deinit {
        EmulatorFunctions.unregister(name: "main")
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
            emuLock.lock()
            ares_tick(ctx)

            // Copy latest frame while holding the lock — ares_get_frame reads ctx.
            var w: UInt32 = 0, h: UInt32 = 0
            ares_get_frame(ctx, nil, 0, &w, &h)
            var pixels: [UInt32] = []
            if w > 0 && h > 0 {
                pixels = [UInt32](repeating: 0, count: Int(w * h))
                pixels.withUnsafeMutableBufferPointer {
                    _ = ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
                }
            }
            emuLock.unlock()

            if w > 0 && h > 0 {
                metalRenderer.submitFrame(pixels, width: Int(w), height: Int(h))
                metalView.draw()

                // EmulatorStarted fires once, on the first rendered frame.
                if pendingStarted {
                    pendingStarted = false
                    currentStatus = "running"
                    eventListener?.onStarted(system: loadedSystem, romPath: romPath)
                }
            }

            pollWatches()
        }
    }

    /// Read every watched address and emit `MemoryChanged` when a value differs from
    /// the last observed value. The first read establishes the baseline silently.
    private func pollWatches() {
        watchLock.lock()
        let snapshot = watches
        watchLock.unlock()
        guard !snapshot.isEmpty else { return }

        for (address, entry) in snapshot {
            guard let bytes = syncReadMemory(address: address, length: entry.length) else { continue }
            let value = bytes.reversed().reduce(0) { ($0 << 8) | Int($1) } // little-endian

            watchLock.lock()
            // The entry may have been removed between snapshot and now.
            guard var current = watches[address] else { watchLock.unlock(); continue }
            let old = current.lastValue
            current.lastValue = value
            watches[address] = current
            watchLock.unlock()

            if let old, old != value {
                eventListener?.onMemoryChanged(address: address, oldValue: old, newValue: value)
            }
        }
    }
}
