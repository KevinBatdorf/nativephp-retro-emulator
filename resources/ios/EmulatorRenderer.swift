import UIKit
import Metal
import MetalKit
import CoreHaptics
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
    /// parity with Android; consumed by the loop's frame pacing. Mirrored to the
    /// native side so run-ahead can suppress itself while fast-forwarding.
    var fastForward: Bool = false {
        didSet { ares_set_fast_forward(ctx, fastForward) }
    }

    /// Periodic battery-save flush toggle (LoadSystem config { autoSave }).
    var autoSave: Bool = true

    /// Speed multiplier (0.25–4.0) applied to the tick budget; fastForward
    /// takes precedence at 4×.
    var speedMultiplier: Double = 1.0

    // MARK: - System / ROM loading

    /// Initialise the ares core for `system` (an ares id such as "sfc", "fc",
    /// "gb", "md"). System firmware is embedded in the native library — no
    /// assets are required.
    func loadSystem(_ system: String) -> Bool {
        emuLock.lock()
        let ok = ares_load_system(ctx, system)
        emuLock.unlock()
        if ok {
            loadedSystem = system
            currentStatus = "loading"
        }
        return ok
    }

    /// ares ids compiled into this build (e.g. ["fc", "sfc", "gb", "md"]).
    static var supportedSystems: [String] {
        String(cString: ares_supported_systems()).components(separatedBy: ",")
    }

    /// - Parameter savePrefix: battery-save location — files are written as
    ///   "<prefix>.save.ram" etc., and existing files seed the cartridge before
    ///   boot. Nil disables persistence.
    func loadRom(_ romData: Data, path: String, savePrefix: String? = nil) -> Bool {
        // A fresh ROM invalidates every watch baseline (spec: watches clear on loadRom).
        clearMemoryWatches()

        emuLock.lock()
        let ok = romData.withUnsafeBytes {
            ares_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count, savePrefix)
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
        flushSaves()
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
        flushSaves()
        audio.stop()
        currentStatus = "stopped"
        eventListener?.onStopped()
    }

    /// Write battery-backed memory to disk (see ares_flush_saves). Safe from
    /// any thread — serialised on emuLock like every other ctx access.
    func flushSaves() {
        emuLock.lock()
        _ = ares_flush_saves(ctx)
        emuLock.unlock()
    }

    /// Master volume (0–1) and stereo balance (−1 … +1). Safe from any thread.
    func setAudioOptions(volume: Float, balance: Float) {
        ares_set_audio(ctx, volume, balance)
    }

    /// Video post-processing options, serialised on emuLock. overscan false
    /// (default) trims the borders like desktop.
    func setVideoOptions(
        luminance: Float, saturation: Float, gamma: Float,
        colorBleed: Bool, interframeBlending: Bool, overscan: Bool
    ) {
        emuLock.lock()
        ares_set_video(ctx, luminance, saturation, gamma, colorBleed, interframeBlending, overscan)
        emuLock.unlock()
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

    // MARK: - Rumble

    private var hapticEngine: CHHapticEngine?
    private var lastRumbleState: UInt32 = 0

    /// Gate rumble forwarding from ares' motor nodes. Safe from any thread.
    func setRumbleEnabled(_ enabled: Bool) {
        emuLock.lock()
        ares_set_rumble_enabled(ctx, enabled)
        emuLock.unlock()
        if !enabled {
            lastRumbleState = 0
            hapticEngine?.stop()
        }
    }

    /// Whether this device can rumble at all.
    var hasHaptics: Bool { CHHapticEngine.capabilitiesForHardware().supportsHaptics }

    /// Called from the emulation loop when the packed motor state changes.
    /// Motors publish on/off transitions — a long continuous event stands in
    /// for "held" and the zero transition stops the engine.
    private func applyRumble(_ state: UInt32) {
        guard hasHaptics else { return }
        if state == 0 {
            hapticEngine?.stop()
            return
        }
        let strong = Float((state >> 16) & 0xFFFF) / 65535.0
        let weak = Float(state & 0xFFFF) / 65535.0
        let intensity = max(strong, weak)
        do {
            if hapticEngine == nil {
                hapticEngine = try CHHapticEngine()
                hapticEngine?.resetHandler = { [weak self] in self?.hapticEngine = nil }
            }
            guard let engine = hapticEngine else { return }
            try engine.start()
            let event = CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: strong >= weak ? 0.3 : 0.7),
                ],
                relativeTime: 0,
                duration: 10
            )
            let player = try engine.makePlayer(with: CHHapticPattern(events: [event], parameters: []))
            try player.start(atTime: CHHapticTimeImmediate)
        } catch {
            // Haptics are best-effort — a failed engine start drops this pulse.
        }
    }

    /// Poll the packed motor state (atomic native-side; no lock needed).
    func pollRumble() {
        let state = ares_get_rumble_state(ctx)
        if state != lastRumbleState {
            lastRumbleState = state
            applyRumble(state)
        }
    }

    // MARK: - Rewind / run-ahead

    /// Enable/disable rewind capture (see ares_configure_rewind). Serialised on emuLock.
    func configureRewind(enabled: Bool, bufferSeconds: Int) {
        emuLock.lock()
        ares_configure_rewind(ctx, enabled, Int32(bufferSeconds))
        emuLock.unlock()
    }

    /// Enter/exit rewind playback. Returns 1 rewinding, 0 playing, -1 rewind not enabled.
    func toggleRewind() -> Int {
        emuLock.lock()
        defer { emuLock.unlock() }
        return Int(ares_toggle_rewind(ctx))
    }

    /// Enable/disable one-frame run-ahead (see ares_set_run_ahead). Serialised on emuLock.
    func setRunAhead(enabled: Bool) {
        emuLock.lock()
        ares_set_run_ahead(ctx, enabled)
        emuLock.unlock()
    }

    /// Enable/disable dynamic rate control (see ares_set_dynamic_rate_control).
    /// Serialised on emuLock.
    func setDynamicRateControl(enabled: Bool) {
        emuLock.lock()
        ares_set_dynamic_rate_control(ctx, enabled)
        emuLock.unlock()
    }

    // MARK: - Cheats

    /// Register (or replace) a cheat code. Returns false when no valid
    /// ADDR:VALUE pair parses. Serialised on emuLock.
    func addCheat(code: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return ares_add_cheat(ctx, code)
    }

    /// Remove a cheat by exact code string. Returns false when it wasn't active.
    func removeCheat(code: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return ares_remove_cheat(ctx, code)
    }

    func clearCheats() {
        emuLock.lock()
        ares_clear_cheats(ctx)
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

        // Self-register under the default surface so bridge calls resolve.
        // The iOS EDGE renderer entry point (the EmulatorSurface counterpart)
        // will register under the node's real `name` prop once it exists.
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
        // Ticks are budgeted by wall clock against the console frame rate —
        // an unpaced loop runs as fast as the CPU allows. 60.0988 is NTSC
        // SNES/NES; GB/MD are within 0.6% and the resampler absorbs it.
        let targetFps = 60.0988
        var lastTick = DispatchTime.now()
        var accumulator = 0.0
        var lastAutoSave = DispatchTime.now()
        while running {
            let now = DispatchTime.now()
            // Clamp long gaps (suspend/resume) so we don't fast-run the debt.
            let elapsed = min(
                Double(now.uptimeNanoseconds - lastTick.uptimeNanoseconds) / 1e9, 0.25)
            lastTick = now
            let speed = fastForward ? 4.0 : speedMultiplier
            accumulator += elapsed * targetFps * speed
            var ticks = Int(accumulator)
            if ticks > 8 {
                ticks = 8          // can't keep up — drop the debt, don't spiral
                accumulator = 0
            } else {
                accumulator -= Double(ticks)
            }

            var w: UInt32 = 0, h: UInt32 = 0
            var pixels: [UInt32] = []
            var geometry = [Double](repeating: 0, count: 7)
            if ticks > 0 {
                emuLock.lock()
                for _ in 0..<ticks { ares_tick(ctx) }

                // Periodic battery-save flush (30 s, ares' desktop cadence).
                if autoSave,
                   DispatchTime.now().uptimeNanoseconds - lastAutoSave.uptimeNanoseconds
                       >= 30_000_000_000 {
                    lastAutoSave = DispatchTime.now()
                    _ = ares_flush_saves(ctx)
                }

                // Copy latest frame while holding the lock — ares_get_frame reads ctx.
                ares_get_frame(ctx, nil, 0, &w, &h)
                if w > 0 && h > 0 {
                    pixels = [UInt32](repeating: 0, count: Int(w * h))
                    pixels.withUnsafeMutableBufferPointer {
                        _ = ares_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
                    }
                    ares_get_video_geometry(ctx, &geometry)
                }
                emuLock.unlock()

                pollRumble()
            }

            if w > 0 && h > 0 {
                metalRenderer.submitFrame(
                    pixels, width: Int(w), height: Int(h),
                    geometry: VideoGeometry(values: geometry))
                metalView.draw()

                // EmulatorStarted fires once, on the first rendered frame.
                if pendingStarted {
                    pendingStarted = false
                    currentStatus = "running"
                    eventListener?.onStarted(system: loadedSystem, romPath: romPath)
                }
            }

            pollWatches()
            usleep(2_000)  // yield — the accumulator absorbs the jitter
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
