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
/// Threading: `emu_tick` runs on the emulation loop thread while bridge calls
/// (memory, state, screenshot) arrive on an arbitrary bridge thread. All `ctx`
/// access is serialized through `emuLock` — the iOS equivalent of Android posting
/// render-thread-critical work onto the render thread.
final class EmulatorRenderer: UIView {

    // MARK: - Public state (read by EmulatorFunctions)

    /// Current lifecycle status: "stopped", "loading", "running", or "paused".
    private(set) var currentStatus: String = "stopped"

    /// The system id passed to the last `loadSystem` (e.g. "sfc"). Empty until staged.
    private(set) var loadedSystem: String = ""

    /// Android-parity name — the staged system id LoadRom/ConnectDevice
    /// validate against (staging is synchronous on iOS, so it equals
    /// `loadedSystem`).
    var stagedSystemId: String { loadedSystem }

    /// Staged region declaration: explicit override ("" = resolve
    /// from the ROM analysis) and preference CSV for multi-region ROMs
    /// ("" = desktop's default "NTSC-U"). Set by LoadSystem, consumed by loadRom.
    var stagedRegion: String = ""
    var stagedPreferredRegions: String = ""

    /// Installed by the surface registry so lifecycle/memory callbacks reach PHP.
    /// Strong: the forwarder holds only the surface name, so there is no retain cycle.
    var eventListener: EmulatorEventListener?

    /// When true the emulation loop ticks extra frames per display frame. Stored for
    /// parity with Android; consumed by the loop's frame pacing. Mirrored to the
    /// native side so run-ahead can suppress itself while fast-forwarding.
    var fastForward: Bool = false {
        didSet { emu_set_fast_forward(ctx, fastForward) }
    }

    /// Periodic battery-save flush toggle (LoadSystem config { autoSave }).
    var autoSave: Bool = true

    /// Speed multiplier (0.25–4.0) applied to the tick budget; fastForward
    /// takes precedence at 4×.
    var speedMultiplier: Double = 1.0

    // MARK: - System / ROM loading

    /// STAGE a system declaration — no core boots until `loadRom`
    /// arrives with a ROM, so the region variant is always resolved ROM-first.
    /// Re-staging over a running core is legal; the running game continues
    /// until the next `loadRom`. System firmware is embedded in the native
    /// library — no assets are required; `biosPath` optionally overrides gba's
    /// embedded open BIOS with a real dump for accuracy.
    func loadSystem(
        _ system: String,
        biosPath: String? = nil,
        bootOptions: [String: Bool] = [:],
        backend: String? = nil
    ) -> Bool {
        emuLock.lock()
        // Staged for the next boot's load(); a running core never changes.
        for (name, value) in bootOptions {
            emu_stage_boot_option(ctx, name, value ? "true" : "false")
        }
        let ok = emu_load_system(ctx, system, biosPath ?? "", backend ?? "")
        emuLock.unlock()
        if ok { loadedSystem = system }
        return ok
    }

    /// The engine serving calls right now: active, else staged, else "".
    func backendName() -> String {
        emuLock.lock()
        let name = String(cString: emu_get_backend_name(ctx))
        emuLock.unlock()
        return name
    }

    /// Whether the staged/active engine exposes the setVideo settings door.
    func videoSettingsSupported() -> Bool {
        emuLock.lock()
        let supported = emu_video_settings_supported(ctx)
        emuLock.unlock()
        return supported
    }

    /// Whether the staged/active engine declares the per-system toggle `key`.
    func toggleSupported(_ key: String) -> Bool {
        emuLock.lock()
        let supported = emu_toggle_supported(ctx, key)
        emuLock.unlock()
        return supported
    }

    /// Set an engine-declared option (libretro core options). Legal only when
    /// the core declares both the key and the value; behavior is the core
    /// author's. Returns "" when applied, else the refusal message. `staged`
    /// targets the engine the next boot uses.
    func setEngineOption(_ key: String, _ value: String, staged: Bool) -> String {
        emuLock.lock()
        let refusal = String(cString: emu_set_engine_option(ctx, key, value, staged))
        emuLock.unlock()
        return refusal
    }

    /// The engine-declared option schema: [{key, choices, default, current}].
    func engineOptionsJson() -> String {
        emuLock.lock()
        let json = String(cString: emu_get_engine_options_json(ctx))
        emuLock.unlock()
        return json
    }

    /// Per-system engine availability + default pick, parsed from the native
    /// JSON: `["gb": (backends: [...], default: "sameboy"), …]`.
    static var backendsJson: [String: Any] {
        let raw = String(cString: emu_get_backends_json())
        let data = raw.data(using: .utf8) ?? Data()
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    }

    /// Live boot-option value from the running core ("true"/"false", "" when
    /// not exposed). Reads core state, not what was requested.
    func bootOption(_ name: String) -> String {
        emuLock.lock()
        let value = String(cString: emu_get_boot_option(ctx, name))
        emuLock.unlock()
        return value
    }

    /// ares ids compiled into this build (e.g. ["fc", "sfc", "gb", "md"]).
    static var supportedSystems: [String] {
        String(cString: emu_supported_systems()).components(separatedBy: ",")
    }

    /// ROM file extensions (no dots) valid for a system id — the LoadRom
    /// family-mismatch gate.
    func systemExtensions(_ system: String) -> [String] {
        emuLock.lock()
        let exts = String(cString: emu_system_extensions(ctx, system))
        emuLock.unlock()
        return exts.isEmpty ? [] : exts.components(separatedBy: ",")
    }

    /// Boot the staged system with this ROM — the one boot path, first load
    /// and every swap alike. A running game is torn down and a fresh core
    /// boots with the region resolved from this ROM.
    /// - Parameter savePrefix: battery-save location — files are written as
    ///   "<prefix>.save.ram" etc., and existing files seed the cartridge before
    ///   boot. Nil disables persistence.
    func loadRom(_ romData: Data, path: String, savePrefix: String? = nil) -> Bool {
        // Watch addresses belong to the outgoing ROM and are wrapper-held, so
        // clear them here; cheats clear natively in the reboot.
        clearMemoryWatches()

        emuLock.lock()
        let result = romData.withUnsafeBytes {
            emu_load_rom(ctx, $0.bindMemory(to: UInt8.self).baseAddress, $0.count,
                          savePrefix, stagedRegion, stagedPreferredRegions)
        }
        // Fresh screen nodes boot with ares defaults — reapply the surface's
        // options like desktop reapplies its settings. (Context-side prefs —
        // volume, DRC, run-ahead, rewind config, rumble — survive the
        // in-place reboot.)
        if result == 1 {
            applyVideoOptions()
            applyCoreOptions()
        }
        emuLock.unlock()

        switch result {
        case 1:
            romPath = path
            currentStatus = "loading"
            pendingStarted = true
            startLoop()
            return true
        case -1:
            // Failure after teardown began — the emulator is cleanly stopped.
            stopLoop()
            audio.stop()
            currentStatus = "stopped"
            eventListener?.onError(code: "LOAD_FAILED", message: "boot failed; emulator stopped")
            return false
        default:
            // Pre-teardown rejection: a running game is untouched.
            eventListener?.onError(code: "LOAD_FAILED", message: "ROM rejected by analyzer")
            return false
        }
    }


    // MARK: - Lifecycle

    func pauseEmulation() {
        audio.setPaused(true)
        emu_pause(ctx)
        flushSaves()
        currentStatus = "paused"
        eventListener?.onPaused()
    }

    func resumeEmulation() {
        audio.setPaused(false)
        emu_resume(ctx)
        currentStatus = "running"
        eventListener?.onResumed()
    }

    /// Stop emulation and tear down the ares core — Android cycles the
    /// platform via destroy()+init(); emu_reset does the same in place, so a
    /// follow-up loadSystem starts from factory state (system switching goes
    /// through here).
    func stopEmulation() {
        clearMemoryWatches()
        stopLoop()
        audio.stop()
        hapticEngine?.stop()
        lastRumbleState = 0
        emuLock.lock()
        _ = emu_flush_saves(ctx)
        emu_reset(ctx)
        emuLock.unlock()
        loadedSystem = ""
        pendingStarted = false
        currentStatus = "stopped"
        eventListener?.onStopped()
    }

    /// Write battery-backed memory to disk (see emu_flush_saves). Safe from
    /// any thread — serialized on emuLock like every other ctx access.
    func flushSaves() {
        emuLock.lock()
        _ = emu_flush_saves(ctx)
        emuLock.unlock()
    }

    /// Master volume (0–1) and balance (−1 … +1); nil keeps the current
    /// value. Safe from any thread.
    func setAudioOptions(volume: Float? = nil, balance: Float? = nil) {
        if let volume { audioVolume = volume }
        if let balance { audioBalance = balance }
        emu_set_audio(ctx, audioVolume, audioBalance)
    }

    private var audioVolume: Float = 1
    private var audioBalance: Float = 0

    /// Merge global display options — nil keeps the current value, and the
    /// surface's options persist across ROM/system reloads (desktop reapplies
    /// its settings at load the same way). overscan false (default) trims the
    /// borders like desktop. Presentation settings apply on the next draw.
    func setVideoOptions(
        luminance: Float? = nil, saturation: Float? = nil, gamma: Float? = nil,
        colorBleed: Bool? = nil, overscan: Bool? = nil,
        output: String? = nil, fixedScale: Int? = nil, aspectCorrection: String? = nil
    ) {
        emuLock.lock()
        if let output { presentation.output = output }
        if let fixedScale { presentation.fixedScale = fixedScale }
        if let aspectCorrection { presentation.aspectCorrection = aspectCorrection }
        if let luminance { videoOptions.luminance = luminance }
        if let saturation { videoOptions.saturation = saturation }
        if let gamma { videoOptions.gamma = gamma }
        if let colorBleed { videoOptions.colorBleed = colorBleed }
        if let overscan { videoOptions.overscan = overscan }
        let pres = presentation
        applyVideoOptions()
        emuLock.unlock()
        metalRenderer.setPresentation(pres)
    }

    /// Push the merged screen-node options into ares. Callers hold emuLock.
    private func applyVideoOptions() {
        emu_set_video(
            ctx, videoOptions.luminance, videoOptions.saturation, videoOptions.gamma,
            videoOptions.colorBleed, videoOptions.overscan)
    }

    /// Apply a librashader `.slangp` preset by path; nil clears (passthrough).
    /// Returns false when the preset fails to load. Safe from any thread —
    /// the chain swap is serialized inside the Metal renderer.
    func syncSetShader(path: String?) -> Bool {
        metalRenderer.setShader(path)
    }

    /// Merge per-system emulation toggles — recognized keys update the
    /// persisted map and apply live (a no-op on cores without the node, and
    /// reapplied on the next boot). Applies immediately when a core is loaded.
    func setCoreOptions(_ options: [String: Bool]) {
        emuLock.lock()
        for (key, value) in options where coreOptions.keys.contains(key) {
            coreOptions[key] = value
            // No-ops natively when no core is loaded yet; reapplied at boot.
            emu_set_core_boolean(ctx, key, value)
        }
        emuLock.unlock()
    }

    /// Reapply all toggles to a freshly booted core. Callers hold emuLock.
    private func applyCoreOptions() {
        for (key, value) in coreOptions { emu_set_core_boolean(ctx, key, value) }
    }

    // MARK: - Memory

    /// Synchronous WRAM read. Returns nil when the address is out of range or no ROM
    /// is loaded. Blocks the caller until it can take `emuLock` (≤ one frame).
    func syncReadMemory(address: UInt32, length: Int) -> [UInt8]? {
        guard length > 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: length)
        emuLock.lock()
        let written = buf.withUnsafeMutableBufferPointer {
            emu_read_memory(ctx, address, $0.baseAddress, Int32(length))
        }
        emuLock.unlock()
        guard written >= 0 else { return nil }
        return Array(buf.prefix(Int(written)))
    }

    func writeMemory(address: UInt32, bytes: [UInt8]) {
        guard !bytes.isEmpty else { return }
        emuLock.lock()
        bytes.withUnsafeBufferPointer {
            emu_write_memory(ctx, address, $0.baseAddress, Int32(bytes.count))
        }
        emuLock.unlock()
    }

    // MARK: - Rumble

    private var hapticEngine: CHHapticEngine?
    private var lastRumbleState: UInt32 = 0

    /// Gate rumble forwarding from ares' motor nodes. Safe from any thread.
    func setRumbleEnabled(_ enabled: Bool) {
        emuLock.lock()
        emu_set_rumble_enabled(ctx, enabled)
        emuLock.unlock()
        if !enabled {
            lastRumbleState = 0
            hapticEngine?.stop()
        }
    }

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
        let state = emu_get_rumble_state(ctx)
        if state != lastRumbleState {
            lastRumbleState = state
            applyRumble(state)
        }
    }

    // MARK: - Rewind / run-ahead

    /// Enable/disable rewind capture (see emu_configure_rewind). Safe from any thread.
    func configureRewind(enabled: Bool, bufferSeconds: Int) {
        emuLock.lock()
        emu_configure_rewind(ctx, enabled, Int32(bufferSeconds))
        emuLock.unlock()
    }

    /// Enter/exit rewind playback. Returns 1 rewinding, 0 playing, -1 rewind not enabled.
    func toggleRewind() -> Int {
        emuLock.lock()
        defer { emuLock.unlock() }
        return Int(emu_toggle_rewind(ctx))
    }

    /// Enable/disable one-frame run-ahead (see emu_set_run_ahead). Safe from any thread.
    func setRunAhead(enabled: Bool) {
        emuLock.lock()
        emu_set_run_ahead(ctx, enabled)
        emuLock.unlock()
    }

    /// Enable/disable dynamic rate control (see emu_set_dynamic_rate_control).
    /// Safe from any thread.
    func setDynamicRateControl(enabled: Bool) {
        emuLock.lock()
        emu_set_dynamic_rate_control(ctx, enabled)
        emuLock.unlock()
    }

    // MARK: - Cheats

    /// Register (or replace) a cheat code. Returns false when no valid
    /// ADDR:VALUE pair parses. Safe from any thread.
    func addCheat(code: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return emu_add_cheat(ctx, code)
    }

    /// Remove a cheat by exact code string. Returns false when it wasn't active.
    func removeCheat(code: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return emu_remove_cheat(ctx, code)
    }

    func clearCheats() {
        emuLock.lock()
        emu_clear_cheats(ctx)
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
        return emu_state_save(ctx, path)
    }

    func syncStateLoad(path: String) -> Bool {
        emuLock.lock()
        defer { emuLock.unlock() }
        return emu_state_load(ctx, path)
    }

    // MARK: - Metadata

    func getRegion() -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_get_region(ctx))
    }

    func getPortsJson() -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_get_ports_json(ctx))
    }

    // MARK: - Devices + input (device-handle model)

    /// Register (or swap) the device on a physical port; empty device
    /// disconnects. Validated against the staged system by id; the actual
    /// allocate/connect is deferred to the emulation thread. Returns "" or a
    /// category-A error code.
    func connectDevice(port: Int, device: String) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_connect_device(ctx, stagedSystemId, Int32(port), device))
    }

    /// The logical ports a physical port's registered device occupies
    /// (4 for a Super Multitap).
    func devicePorts(port: Int) -> [Int] {
        var out = [Int32](repeating: 0, count: 5)
        emuLock.lock()
        let count = out.withUnsafeMutableBufferPointer {
            emu_device_ports(ctx, stagedSystemId, Int32(port), $0.baseAddress, Int32($0.count))
        }
        emuLock.unlock()
        return out.prefix(Int(count)).map(Int.init)
    }

    /// Buttons held on a port, from any source. See `emu_get_pressed_buttons`.
    func pressedButtons(port: Int) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_get_pressed_buttons(ctx, Int32(port)))
    }

    func pressButton(port: Int, name: String, down: Bool) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_press_button(ctx, Int32(port), name, down))
    }

    /// Accumulate a relative axis delta (mouse X/Y).
    func setAxis(port: Int, name: String, value: Int) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_set_axis(ctx, Int32(port), name, Int32(value)))
    }

    /// Aim an axis device at an absolute normalized (0..1) screen position.
    func aimAt(port: Int, x: Float, y: Float) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        return String(cString: emu_aim_at(ctx, Int32(port), x, y))
    }

    /// Merge a per-port controller remap; empty arrays reset to defaults.
    func setInputMapping(port: Int, emulated: [String], source: [String]) -> String {
        emuLock.lock()
        defer { emuLock.unlock() }
        let emuDup = emulated.map { strdup($0) }
        let srcDup = source.map { strdup($0) }
        defer {
            emuDup.forEach { free($0) }
            srcDup.forEach { free($0) }
        }
        var emuC: [UnsafePointer<CChar>?] = emuDup.map { UnsafePointer($0) }
        var srcC: [UnsafePointer<CChar>?] = srcDup.map { UnsafePointer($0) }
        let result = emu_set_input_mapping(ctx, Int32(port), &emuC, &srcC, Int32(emulated.count))
        return String(cString: result!)
    }

    /// Stage a slotted-media ROM (SuFami Turbo slot A/B, BS-X BS Memory) for
    /// the next loadRom.
    func stageSlot(index: Int, rom: Data) {
        emuLock.lock()
        rom.withUnsafeBytes {
            emu_stage_slot(ctx, Int32(index),
                            $0.bindMemory(to: UInt8.self).baseAddress, $0.count)
        }
        emuLock.unlock()
    }

    // MARK: - Screenshot

    /// Encode the latest frame as PNG. Returns nil when no frame is available.
    /// Pixels are interpreted as BGRA8 (matching the Metal display path).
    func syncScreenshot() -> Data? {
        // With an active filter chain, capture the post-shader intermediate —
        // what the user actually sees. Passthrough falls to the raw frame,
        // which IS the presented content then.
        if let shaded = metalRenderer.screenshotShaded() {
            return Self.pngFromBGRA(bytes: shaded.bytes, width: shaded.width, height: shaded.height)
        }

        emuLock.lock()
        var w: UInt32 = 0, h: UInt32 = 0
        emu_get_frame(ctx, nil, 0, &w, &h)
        guard w > 0, h > 0 else { emuLock.unlock(); return nil }
        var pixels = [UInt32](repeating: 0, count: Int(w * h))
        let ok = pixels.withUnsafeMutableBufferPointer {
            emu_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
        }
        emuLock.unlock()
        guard ok else { return nil }

        let width = Int(w), height = Int(h)
        return pixels.withUnsafeMutableBytes { raw -> Data? in
            Self.pngFromBGRA(data: raw.baseAddress!, width: width, height: height)
        }
    }

    private static func pngFromBGRA(bytes: [UInt8], width: Int, height: Int) -> Data? {
        var copy = bytes
        return copy.withUnsafeMutableBytes { raw -> Data? in
            pngFromBGRA(data: raw.baseAddress!, width: width, height: height)
        }
    }

    private static func pngFromBGRA(data: UnsafeMutableRawPointer, width: Int, height: Int) -> Data? {
        let bitmapInfo = CGBitmapInfo(rawValue:
            CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let context = CGContext(
            data: data,
            width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bitmapInfo.rawValue
        ), let cgImage = context.makeImage() else {
            return nil
        }
        return UIImage(cgImage: cgImage).pngData()
    }

    // MARK: - Private

    private struct WatchEntry {
        let length: Int
        var lastValue: Int?
    }

    /// Current screen-node options, merged under emuLock. Kept so setVideo
    /// has merge semantics and a system reload can reapply them.
    private struct VideoOptions {
        var luminance: Float = 1
        var saturation: Float = 1
        var gamma: Float = 1
        var colorBleed = false
        var overscan = false
    }

    private var videoOptions = VideoOptions()

    /// Per-system emulation toggles. Default off — applied even when unset,
    /// since the cores default them on; reapplied on each boot like
    /// videoOptions. Unsupported keys no-op natively, so every system carries
    /// the full set.
    private var coreOptions: [String: Bool] = [
        "colorEmulation": false,
        "deepBlackBoost": false,
        "interframeBlending": false,
        "showIcons": false,
    ]

    private var presentation = PresentationSettings()

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

    /// Surface name this renderer is registered under. Set by the EDGE entry
    /// point (EmulatorSurfaceView) from the node's `name` prop.
    var surfaceName = "main"

    /// Guard for the declarative element setup — updateUIView re-stages only
    /// when the (system, config, rom) key actually changes.
    var declaredBootKey: String?

    private var loopThread: Thread?
    private var running = false

    override init(frame: CGRect) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fatalError("Metal not available")
        }
        ctx = emu_create()!

        metalView = MTKView(frame: .zero, device: device)
        metalView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.clearColor = MTLClearColorMake(0, 0, 0, 1)
        // ares emits already-gamma'd (sRGB-encoded) pixels. Tag the layer sRGB so
        // CoreAnimation shows them 1:1 without an extra gamma on P3/EDR displays —
        // matching the Android Vulkan path's UNORM + sRGB-nonlinear choice. Without
        // this the picture reads darker/uneven.
        (metalView.layer as? CAMetalLayer)?.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
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
        EmulatorFunctions.unregister(name: surfaceName, renderer: self)
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
        // an unpaced loop runs as fast as the CPU allows. 60.0988 is only the
        // fallback until the core's first Platform::refreshRateHint arrives
        // during power-on; after that the hint is authoritative — per-system
        // and per-region (SFC NTSC 60.0988, GB 59.7275, PAL ~50), polled per
        // iteration because some cores re-hint on video-mode changes.
        let fallbackFps = 60.0988
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
            let hint = emu_get_refresh_rate_hint(ctx)
            let targetFps = hint > 0 ? hint : fallbackFps
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
                for _ in 0..<ticks { emu_tick(ctx) }

                // Periodic battery-save flush (30 s, ares' desktop cadence).
                if autoSave,
                   DispatchTime.now().uptimeNanoseconds - lastAutoSave.uptimeNanoseconds
                       >= 30_000_000_000 {
                    lastAutoSave = DispatchTime.now()
                    _ = emu_flush_saves(ctx)
                }

                // Copy latest frame while holding the lock — emu_get_frame reads ctx.
                emu_get_frame(ctx, nil, 0, &w, &h)
                if w > 0 && h > 0 {
                    pixels = [UInt32](repeating: 0, count: Int(w * h))
                    pixels.withUnsafeMutableBufferPointer {
                        _ = emu_get_frame(ctx, $0.baseAddress, $0.count, &w, &h)
                    }
                    emu_get_video_geometry(ctx, &geometry)
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
