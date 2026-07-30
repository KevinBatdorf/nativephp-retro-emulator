import Foundation
import GameController

/// iOS-side `Emulator.*` bridge functions for the NativePHP Retro Emulator plugin.
///
/// Mirrors the Android registration in `resources/android/EmulatorFunctions.kt`:
/// each nested class handles one bridge function declared in `nativephp.json`, and a
/// `NativeEventForwarder` turns renderer callbacks into NativePHP events. The surface
/// registry maps the `name` from `<native-emulator name="…" />` to its `EmulatorRenderer`.
///
/// Threading:
///  - Bridge functions run on an arbitrary NativePHP bridge thread.
///  - `ctx`-critical ares operations are serialized inside `EmulatorRenderer` (`emuLock`).
///  - Events are dispatched to PHP on the main thread via `NativeElementBridge`.
enum EmulatorFunctions {

    // MARK: - Surface registry

    private static let registryLock = NSLock()
    private static var surfaces: [String: EmulatorRenderer] = [:]

    // Last slot each surface saved to — undoStateSave reverts that slot's file
    // (ares state.undoSlot, desktop-ui/program/states.cpp:8). In-memory like
    // ares; a fresh process forgets it and undo reports nothing_to_undo.
    private static let undoLock = NSLock()
    private static var undoSaveSlot: [String: String] = [:]

    /// Register an `EmulatorRenderer` under `name` and install the event forwarder.
    /// Called by the renderer when it is added to the native layout tree.
    static func register(name: String, renderer: EmulatorRenderer) {
        renderer.eventListener = NativeEventForwarder(surface: name)
        registryLock.lock()
        surfaces[name] = renderer
        registryLock.unlock()
    }

    /// Remove a surface from the registry — but only if `renderer` still owns the
    /// slot. During a fast remount (navigation / recomposition) the EDGE element
    /// can instantiate a new renderer that re-registers under the same name
    /// BEFORE the old instance's dismantle fires. A blind remove-by-name would
    /// then wipe the live renderer's entry, stranding every bridge call with
    /// SURFACE_NOT_FOUND. Compare identity so a superseded teardown leaves the
    /// live entry in place. (Same fix as Android's unregisterSurface.)
    static func unregister(name: String, renderer: EmulatorRenderer) {
        renderer.eventListener = nil
        registryLock.lock()
        let removed = surfaces[name] === renderer
        if removed { surfaces.removeValue(forKey: name) }
        registryLock.unlock()
        NSLog(removed
            ? "[RetroEmulator] Surface unregistered: \(name)"
            : "[RetroEmulator] Surface unregister skipped — '\(name)' owned by a newer instance")
    }

    /// The renderer driving `name`, for sibling elements that feed input straight
    /// into the core. Nil until the emulator surface mounts, so an overlay laid
    /// out beside it simply drops presses until there is a core to receive them.
    static func renderer(named name: String) -> EmulatorRenderer? {
        registryLock.lock()
        defer { registryLock.unlock() }

        return surfaces[name]
    }

    /// Apply a `<native:emulator>` element's declarative setup — the same
    /// staging → boot path Emulator::loadSystem() / loadRom() drive imperatively,
    /// routed through the real bridge functions so behavior is identical by
    /// construction. `configJson` is the merged effective config (global ⊕
    /// system, inputCapture already hoisted onto the surface) as a JSON object
    /// string. Runs off the caller's thread: LoadRom reads the ROM file inline
    /// and the native load blocks on the emulation lock.
    static func applyDeclarativeSetup(surface: String, system: String, configJson: String, rom: String) {
        guard !system.isEmpty else { return }

        let thread = Thread {
            let config = (try? JSONSerialization.jsonObject(with: Data(configJson.utf8)))
                as? [String: Any] ?? [:]

            // Stage the system with its playback/system config — LoadSystem
            // reads the staged + core-toggle keys and ignores the presentation
            // keys, which fan out to their own channels below.
            _ = try? LoadSystem().execute(parameters: [
                "surface": surface, "system": system, "config": config])

            registryLock.lock()
            let staged = surfaces[surface]?.stagedSystemId
            registryLock.unlock()
            guard staged == system else {
                NSLog("[RetroEmulator] Declarative boot: system '%@' did not stage on '%@'", system, surface)
                return
            }

            // Fan the presentation/AV knobs out, mirroring loadSystem's fan-out.
            // SetVideo null-merges so it's safe to call unconditionally; SetAudio
            // defaults the missing key, so gate it on an actual audio knob.
            _ = try? SetVideo().execute(parameters: ["surface": surface, "options": config])
            if config["volume"] != nil || config["balance"] != nil {
                _ = try? SetAudio().execute(parameters: ["surface": surface, "options": config])
            }
            if let rumble = config["rumble"] as? Bool {
                _ = try? SetRumble().execute(parameters: ["surface": surface, "enabled": rumble])
            }
            if let shader = config["shader"] as? String {
                _ = try? SetShader().execute(parameters: ["surface": surface, "path": shader])
            }

            // Boot the staged system with the ROM (every load is a fresh boot).
            if !rom.isEmpty {
                _ = try? LoadRom().execute(parameters: ["surface": surface, "path": rom])
            }
        }
        thread.name = "emu-declarative-boot-\(surface)"
        thread.start()
    }

    // MARK: - Internal helpers

    private static func surfaceName(_ parameters: [String: Any]) -> String {
        parameters["surface"] as? String ?? "main"
    }

    private static func renderer(_ parameters: [String: Any]) -> EmulatorRenderer? {
        let name = surfaceName(parameters)
        // A screen's mount() runs before SwiftUI has rendered the emulator
        // node, so the first Boot/LoadSystem arrives just ahead of register.
        // Briefly await registration instead of failing (Android does the same).
        let deadline = Date().addingTimeInterval(3.0)
        while true {
            registryLock.lock()
            let found = surfaces[name]
            registryLock.unlock()
            if let found { return found }
            if Date() >= deadline { return nil }
            Thread.sleep(forTimeInterval: 0.025)
        }
    }

    /// Report an operational outcome (category B): a valid call the world said
    /// no to — a missing ROM, an empty slot, a failed save. These surface as an
    /// EmulatorError event, never as a bridge error, so the PHP wrapper returns
    /// fluently and the event carries the detail. The "failed" status keeps the
    /// response off the wrapper's throw path (which fires on "error").
    private static func operationalError(
        _ renderer: EmulatorRenderer, code: String, message: String
    ) -> [String: Any] {
        renderer.eventListener?.onError(code: code, message: message)
        return BridgeResponse.success(data: ["status": "failed", "code": code, "message": message])
    }

    /// Scale a whole-percent option to the native 0..1 range. Out of range fails
    /// rather than clamps: a gamma of 1, mistaken for ares' 1.0-2.0 exponent,
    /// renders an almost black screen. Mirrors Android's `percent(...)`.
    private static func percent(
        _ options: [String: Any], _ key: String, min: Int, max: Int
    ) -> (scaled: Float?, error: [String: Any]?) {
        guard let raw = options[key] as? NSNumber else { return (nil, nil) }
        let value = raw.doubleValue
        if value < Double(min) || value > Double(max) {
            return (nil, BridgeResponse.error(
                code: "INVALID_PARAMETERS",
                message: "\(key) is a whole percentage (\(min)-\(max), 100 = unchanged) — got \(value)"
            ))
        }

        return (Float(value) / 100, nil)
    }

    // Native input calls return "" on success or "CODE"/"CODE:detail" on a
    // category-A error. Map that to a bridge response; the code stays in a
    // variable so it doesn't register on the enum drift scan (the codes it
    // emits appear as literals elsewhere / in ConnectDevice).
    private static func statusResponse(
        _ result: String, success: [String: Any]
    ) -> [String: Any] {
        if result.isEmpty { return BridgeResponse.success(data: success) }
        let code = String(result.split(separator: ":", maxSplits: 1)[0])
        let detail = result.contains(":")
            ? String(result.split(separator: ":", maxSplits: 1)[1]) : ""
        let message: String
        switch code {
        case "SYSTEM_NOT_LOADED": message = "no system is loaded"
        case "UNKNOWN_BUTTON":    message = "Unknown button: \(detail)"
        default:                  message = "invalid parameters"
        }
        return BridgeResponse.error(code: code, message: message)
    }

    private static func surfaceNotFound(_ parameters: [String: Any]) -> [String: Any] {
        BridgeResponse.error(
            code: "SURFACE_NOT_FOUND",
            message: "No surface '\(surfaceName(parameters))' registered"
        )
    }

    private static func uint32(_ value: Any?) -> UInt32? {
        if let n = value as? NSNumber { return n.uint32Value }
        return nil
    }

    private static func int(_ value: Any?) -> Int? {
        if let n = value as? NSNumber { return n.intValue }
        return nil
    }

    private static func documentsURL(_ components: String...) -> URL {
        var url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        for component in components { url.appendPathComponent(component) }
        return url
    }

    // Per-system emulation toggles carried on loadSystem() config and
    // setSystemOptions(). Native maps each to its ares node and no-ops where
    // the core doesn't declare it (see native/core_options.hpp).
    private static let coreToggleKeys = ["colorEmulation", "deepBlackBoost", "interframeBlending", "showIcons"]

    private static func coreToggles(from options: [String: Any]) -> [String: Bool] {
        var result: [String: Bool] = [:]
        for key in coreToggleKeys {
            if let value = options[key] as? Bool { result[key] = value }
        }
        return result
    }

    // MARK: - Event forwarder

    /// Translates `EmulatorEventListener` callbacks into NativePHP events. Holds only the
    /// surface name so it never retains the renderer.
    private final class NativeEventForwarder: EmulatorEventListener {

        private let surface: String

        init(surface: String) {
            self.surface = surface
        }

        private func dispatch(_ eventClass: String, _ extra: [String: Any]) {
            var payload: [String: Any] = ["surface": surface]
            payload.merge(extra) { _, new in new }
            let json = (try? JSONSerialization.data(withJSONObject: payload))
                .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
            DispatchQueue.main.async {
                NativeElementBridge.sendNativeEvent(eventName: eventClass, payloadJson: json)
            }
        }

        func onStarted(system: String, romPath: String) {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\EmulatorStarted",
                     ["system": system, "romPath": romPath])
        }

        func onStopped() {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\EmulatorStopped", [:])
        }

        func onPaused() {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\EmulatorPaused", [:])
        }

        func onResumed() {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\EmulatorResumed", [:])
        }

        func onMemoryChanged(address: UInt32, oldValue: Int, newValue: Int) {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\MemoryChanged",
                     ["address": Int(address), "oldValue": oldValue, "newValue": newValue])
        }

        func onMemoryRead(address: UInt32, bytes: [UInt8]) {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\MemoryRead",
                     ["address": Int(address), "bytes": bytes.map { Int($0) }])
        }

        func onError(code: String, message: String) {
            dispatch("KevinBatdorf\\RetroEmulator\\Events\\EmulatorError",
                     ["code": code, "message": message])
        }
    }

    /// Bind to the named surface declared in the component tree.
    class Boot: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard renderer(parameters) != nil else { return surfaceNotFound(parameters) }
            return BridgeResponse.success(data: ["status": "bound", "surface": surfaceName(parameters)])
        }
    }

    /// Initialize the ares core for a system. Supported systems are the ones
    /// compiled into the native library — reported by GetSystems with
    /// `supported: true`. System firmware (SFC ipl.rom + boards.bml, GB boot
    /// ROM, MD TMSS) is embedded; no biosPath is needed for these systems.
    /// config keys: biosPath (String?), autoSave, speed, runAhead, rewind,
    /// rewindBufferSeconds, dynamicRateControl, pixelAccuracy (boot-only
    /// renderer choice, see Configure).
    class LoadSystem: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }

            let system = parameters["system"] as? String ?? "sfc"
            let supported = EmulatorRenderer.supportedSystems
            guard supported.contains(system) else {
                return BridgeResponse.error(
                    code: "UNSUPPORTED_SYSTEM",
                    message: "System '\(system)' is not supported in this build — available: \(supported.joined(separator: ", "))"
                )
            }

            let config = parameters["config"] as? [String: Any] ?? [:]
            let runAhead = (config["runAhead"] as? NSNumber)?.intValue ?? 0
            guard (0...1).contains(runAhead) else {
                return BridgeResponse.error(
                    code: "INVALID_PARAMETERS",
                    message: "runAhead must be 0 or 1 — ares supports one hidden frame"
                )
            }

            renderer.fastForward = false
            renderer.autoSave = config["autoSave"] as? Bool ?? true
            renderer.stagedRegion = config["region"] as? String ?? ""
            renderer.stagedPreferredRegions =
                ((config["preferredRegions"] as? [Any]) ?? [])
                    .compactMap { $0 as? String }.joined(separator: ",")
            renderer.setRunAhead(enabled: runAhead == 1)
            renderer.setDynamicRateControl(enabled: config["dynamicRateControl"] as? Bool ?? true)
            renderer.configureRewind(
                enabled: config["rewind"] as? Bool ?? false,
                bufferSeconds: (config["rewindBufferSeconds"] as? NSNumber)?.intValue ?? 0
            )
            if let speed = (config["speed"] as? NSNumber)?.doubleValue {
                renderer.speedMultiplier = min(max(speed, 0.25), 4.0)
            }
            let toggles = coreToggles(from: config)
            if !toggles.isEmpty { renderer.setCoreOptions(toggles) }

            // Boot-only: picks the renderer implementation before load. Cores
            // without a choice (fc, gb, md) accept and ignore it.
            let bootOptions = [
                "Pixel Accuracy": config["pixelAccuracy"] as? Bool ?? false,
            ]

            guard renderer.loadSystem(system,
                                      biosPath: config["biosPath"] as? String,
                                      bootOptions: bootOptions) else {
                return BridgeResponse.error(code: "LOAD_FAILED", message: "emu_load_system failed for '\(system)'")
            }

            return BridgeResponse.success(data: ["status": "staged", "system": system])
        }
    }

    /// Stage a slotted-media ROM (SuFami Turbo: index 0 = Slot A, 1 = Slot B;
    /// BS-X: index 0 = the BS Memory slot) from a file path, to be inserted at
    /// the next LoadRom (whose base is the slot-carrying cartridge).
    class StageSlot: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let index = int(parameters["index"]) ?? 0
            guard let path = parameters["path"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "path is required")
            }
            guard FileManager.default.fileExists(atPath: path),
                  let rom = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                return operationalError(renderer, code: "ROM_NOT_FOUND", message: "slot ROM not found: \(path)")
            }
            renderer.stageSlot(index: index, rom: rom)
            return BridgeResponse.success(data: ["status": "staged", "index": index])
        }
    }

    class LoadRom: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let path = parameters["path"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "path is required")
            }
            guard FileManager.default.fileExists(atPath: path),
                  let romData = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                return operationalError(renderer, code: "ROM_NOT_FOUND", message: "ROM not found: \(path)")
            }

            let system = renderer.stagedSystemId
            guard !system.isEmpty else {
                return BridgeResponse.error(code: "SYSTEM_NOT_LOADED", message: "Call LoadSystem before LoadRom")
            }
            let extensions = renderer.systemExtensions(system)
            let ext = URL(fileURLWithPath: path).pathExtension.lowercased()
            guard extensions.contains(ext) else {
                return operationalError(
                    renderer,
                    code: "INVALID_ROM",
                    message: "'.\(ext)' is not a \(system) ROM — expected one of: "
                        + extensions.map { ".\($0)" }.joined(separator: ", ")
                )
            }

            // Battery saves live in app support, keyed by surface + ROM basename.
            // A savePath parameter overrides the prefix entirely.
            let savePrefix: String
            if let savePath = parameters["savePath"] as? String {
                savePrefix = savePath
            } else {
                let saveDir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                    .appendingPathComponent("saves/\(surfaceName(parameters))", isDirectory: true)
                try? FileManager.default.createDirectory(at: saveDir, withIntermediateDirectories: true)
                let base = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
                savePrefix = saveDir.appendingPathComponent(base).path
            }

            // Undo files belong to the outgoing game (ares clearUndoStates,
            // load.cpp:170) — a stale cross-game snapshot must not survive.
            let stateDir = documentsURL("states", surfaceName(parameters))
            try? FileManager.default.removeItem(at: stateDir.appendingPathComponent("undo_save.state"))
            try? FileManager.default.removeItem(at: stateDir.appendingPathComponent("undo_load.state"))
            undoLock.lock()
            undoSaveSlot[surfaceName(parameters)] = nil
            undoLock.unlock()

            guard renderer.loadRom(romData, path: path, savePrefix: savePrefix) else {
                // The renderer already dispatched the LOAD_FAILED EmulatorError
                // (category B) — report the operational outcome without a
                // bridge error so the fluent wrapper returns cleanly.
                return BridgeResponse.success(data: ["status": "failed", "code": "LOAD_FAILED"])
            }
            return BridgeResponse.success(data: ["status": "loading", "path": path])
        }
    }


    class Pause: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.pauseEmulation()
            return BridgeResponse.success(data: ["status": "paused"])
        }
    }

    class Resume: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.resumeEmulation()
            return BridgeResponse.success(data: ["status": "running"])
        }
    }

    /// Stop emulation and tear down the loop.
    class Stop: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.stopEmulation()
            return BridgeResponse.success(data: ["status": "stopped"])
        }
    }

    class StateSave: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let slot = parameters["slot"] ?? 1
            let dir = documentsURL("states", surfaceName(parameters))
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let slotURL = dir.appendingPathComponent("\(slot).state")
            let path = slotURL.path

            // The slot's previous file moves to the undo file before the new
            // state lands (ares stateSave, states.cpp:6-9), so undoStateSave
            // can revert the slot.
            let undoURL = dir.appendingPathComponent("undo_save.state")
            if FileManager.default.fileExists(atPath: path) {
                try? FileManager.default.removeItem(at: undoURL)
                if (try? FileManager.default.moveItem(at: slotURL, to: undoURL)) != nil {
                    undoLock.lock()
                    undoSaveSlot[surfaceName(parameters)] = "\(slot)"
                    undoLock.unlock()
                }
            }

            guard renderer.syncStateSave(path: path) else {
                return operationalError(renderer, code: "SAVE_FAILED", message: "State save failed for slot \(slot)")
            }
            return BridgeResponse.success(data: ["status": "saved", "slot": slot, "path": path])
        }
    }

    class StateLoad: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let slot = parameters["slot"] ?? 1
            let dir = documentsURL("states", surfaceName(parameters))
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let url = dir.appendingPathComponent("\(slot).state")

            // Snapshot the current state to the undo-load file before touching
            // the slot (ares stateLoad, states.cpp:26-30) — even when the slot
            // turns out to be empty, matching the reference order.
            _ = renderer.syncStateSave(path: dir.appendingPathComponent("undo_load.state").path)

            guard FileManager.default.fileExists(atPath: url.path) else {
                return operationalError(renderer, code: "SLOT_EMPTY", message: "No state in slot \(slot)")
            }
            guard renderer.syncStateLoad(path: url.path) else {
                return operationalError(renderer, code: "LOAD_FAILED", message: "State load failed for slot \(slot)")
            }
            return BridgeResponse.success(data: ["status": "loaded", "slot": slot])
        }
    }

    /// Undo most recent state save: the slot's previous file moves back over
    /// the slot (ares undoStateSave, states.cpp:46-59) — a file-level revert,
    /// the machine keeps running.
    class UndoStateSave: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let name = surfaceName(parameters)
            let dir = documentsURL("states", name)
            let undoURL = dir.appendingPathComponent("undo_save.state")
            undoLock.lock()
            let slot = undoSaveSlot[name]
            undoLock.unlock()
            guard let slot, FileManager.default.fileExists(atPath: undoURL.path) else {
                return BridgeResponse.success(data: ["status": "nothing_to_undo"])
            }
            let slotURL = dir.appendingPathComponent("\(slot).state")
            try? FileManager.default.removeItem(at: slotURL)
            guard (try? FileManager.default.moveItem(at: undoURL, to: slotURL)) != nil else {
                guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
                return operationalError(renderer, code: "UNDO_FAILED", message: "Unable to revert slot \(slot) to its previous state")
            }
            undoLock.lock()
            undoSaveSlot[name] = nil
            undoLock.unlock()
            return BridgeResponse.success(data: ["status": "undone", "slot": slot])
        }
    }

    /// Undo most recent state load: re-apply the pre-load snapshot, then drop
    /// it (ares undoStateLoad, states.cpp:61-81).
    class UndoStateLoad: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let url = documentsURL("states", surfaceName(parameters), "undo_load.state")
            guard FileManager.default.fileExists(atPath: url.path) else {
                return BridgeResponse.success(data: ["status": "nothing_to_undo"])
            }
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard renderer.syncStateLoad(path: url.path) else {
                return operationalError(renderer, code: "UNDO_FAILED", message: "Undo state load failed")
            }
            try? FileManager.default.removeItem(at: url)
            return BridgeResponse.success(data: ["status": "undone"])
        }
    }

    /// Synchronous WRAM read — returns bytes directly.
    class ReadMemory: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let address = uint32(parameters["address"]) else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "address is required")
            }
            let length = int(parameters["length"]) ?? 1
            guard let bytes = renderer.syncReadMemory(address: address, length: length) else {
                return BridgeResponse.error(
                    code: "READ_FAILED",
                    message: "Memory read failed — address 0x\(String(address, radix: 16, uppercase: true)) out of range or emulator not running"
                )
            }
            return BridgeResponse.success(data: ["address": Int(address), "bytes": bytes.map { Int($0) }])
        }
    }

    /// Asynchronous WRAM read — dispatches `MemoryRead` and returns immediately.
    class ReadMemoryAsync: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let address = uint32(parameters["address"]) else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "address is required")
            }
            let length = int(parameters["length"]) ?? 1

            DispatchQueue.global(qos: .userInitiated).async {
                if let bytes = renderer.syncReadMemory(address: address, length: length) {
                    renderer.eventListener?.onMemoryRead(address: address, bytes: bytes)
                }
            }
            return BridgeResponse.success(data: ["status": "queued", "address": Int(address)])
        }
    }

    /// Write bytes to WRAM.
    class WriteMemory: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let address = uint32(parameters["address"]) else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "address is required")
            }
            guard let byteList = parameters["bytes"] as? [Any] else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "bytes is required")
            }
            let bytes = byteList.compactMap { ($0 as? NSNumber)?.uint8Value }
            // Validate the target synchronously: a read-probe of the same window
            // returns nil for an out-of-range address or when no core is
            // running — exactly the cases a write must reject (WRITE_FAILED).
            guard renderer.syncReadMemory(address: address, length: max(bytes.count, 1)) != nil else {
                return BridgeResponse.error(
                    code: "WRITE_FAILED",
                    message: "Memory write failed — address 0x\(String(address, radix: 16, uppercase: true)) out of range or emulator not running"
                )
            }
            renderer.writeMemory(address: address, bytes: bytes)
            return BridgeResponse.success(data: ["status": "queued", "address": Int(address), "length": bytes.count])
        }
    }

    /// Register/merge memory address watches. Fires `MemoryChanged` on value change.
    class WatchMemory: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let addresses = parameters["addresses"] as? [Any] else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "addresses is required")
            }

            var count = 0
            for item in addresses {
                if let n = item as? NSNumber {
                    renderer.addWatch(address: n.uint32Value, length: 1)
                    count += 1
                } else if let dict = item as? [String: Any], let addr = uint32(dict["address"]) {
                    renderer.addWatch(address: addr, length: int(dict["length"]) ?? 1)
                    count += 1
                }
            }
            return BridgeResponse.success(data: ["status": "watching", "count": count])
        }
    }

    /// Remove specific address watches.
    class UnwatchMemory: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let addresses = parameters["addresses"] as? [Any] else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "addresses is required")
            }
            for item in addresses {
                if let n = item as? NSNumber { renderer.removeWatch(address: n.uint32Value) }
            }
            return BridgeResponse.success(data: ["status": "unwatched"])
        }
    }

    class ClearMemoryWatches: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.clearMemoryWatches()
            return BridgeResponse.success(data: ["status": "cleared"])
        }
    }

    /// Merge audio options. volume 0–100, balance −100 (left) … +100 (right),
    /// both whole percentages. Omitted knobs keep their current value.
    class SetAudio: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]
            let volume = percent(options, "volume", min: 0, max: 100)
            if let error = volume.error { return error }
            let balance = percent(options, "balance", min: -100, max: 100)
            if let error = balance.error { return error }

            renderer.setAudioOptions(volume: volume.scaled, balance: balance.scaled)
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Merge global display options — omitted options keep their current
    /// values, and the surface's options persist across ROM/system reloads
    /// (desktop reapplies its settings at load the same way). luminance/
    /// saturation 0–100 and gamma 50–200 are whole percentages (100 =
    /// unchanged); colorBleed/overscan are booleans, applied
    /// on the ares screen node; presentation settings output (scale/integer/
    /// integerFixed/stretch), fixedScale, and aspectCorrection (none/standard/
    /// anamorphic) mirror ares desktop's Video settings. overscan false
    /// (default) trims the borders like desktop. Per-system emulation toggles
    /// live on setSystemOptions(), not here.
    class SetVideo: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]
            let output = options["output"] as? String
            if let output, !["scale", "integer", "integerFixed", "stretch"].contains(output) {
                return BridgeResponse.error(
                    code: "INVALID_PARAMETERS",
                    message: "output must be scale, integer, integerFixed, or stretch — got '\(output)'"
                )
            }
            let aspectCorrection = options["aspectCorrection"] as? String
            if let aspectCorrection, !["none", "standard", "anamorphic"].contains(aspectCorrection) {
                return BridgeResponse.error(
                    code: "INVALID_PARAMETERS",
                    message: "aspectCorrection must be none, standard, or anamorphic — got '\(aspectCorrection)'"
                )
            }
            let luminance = percent(options, "luminance", min: 0, max: 100)
            if let error = luminance.error { return error }
            let saturation = percent(options, "saturation", min: 0, max: 100)
            if let error = saturation.error { return error }
            let gamma = percent(options, "gamma", min: 50, max: 200)
            if let error = gamma.error { return error }

            renderer.setVideoOptions(
                luminance:  luminance.scaled,
                saturation: saturation.scaled,
                gamma:      gamma.scaled,
                colorBleed: options["colorBleed"] as? Bool,
                overscan:   options["overscan"] as? Bool,
                output:     output,
                fixedScale: (options["fixedScale"] as? NSNumber)?.intValue,
                aspectCorrection: aspectCorrection
            )
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Merge general live options. speed (0.25–4.0) scales the tick budget.
    /// runAhead accepts 0 or 1 — ares supports exactly one hidden frame.
    /// rewind toggles snapshot capture; rewindBufferSeconds sizes the history.
    class Configure: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]

            if options["pixelAccuracy"] != nil {
                // Even upstream only honors this at the next load — reject
                // rather than silently defer.
                return BridgeResponse.error(
                    code: "BOOT_ONLY_OPTION",
                    message: "pixelAccuracy can only be set in the LoadSystem config — "
                        + "it picks the renderer at boot; reboot the system to change it"
                )
            }

            if let runAhead = (options["runAhead"] as? NSNumber)?.intValue {
                guard (0...1).contains(runAhead) else {
                    return BridgeResponse.error(
                        code: "INVALID_PARAMETERS",
                        message: "runAhead must be 0 or 1 — ares supports one hidden frame"
                    )
                }
                renderer.setRunAhead(enabled: runAhead == 1)
            }

            if let rewind = options["rewind"] as? Bool {
                renderer.configureRewind(
                    enabled: rewind,
                    bufferSeconds: (options["rewindBufferSeconds"] as? NSNumber)?.intValue ?? 0
                )
            }

            if let speed = (options["speed"] as? NSNumber)?.doubleValue {
                renderer.speedMultiplier = min(max(speed, 0.25), 4.0)
            }
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Enter/exit rewind playback (5× the capture rate, ares desktop
    /// semantics). Play resumes automatically when history runs out.
    /// Requires rewind capture enabled via LoadSystem config or Configure.
    class ToggleRewind: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            switch renderer.toggleRewind() {
            case 1: return BridgeResponse.success(data: ["status": "rewinding"])
            case 0: return BridgeResponse.success(data: ["status": "playing"])
            default: return BridgeResponse.error(
                code: "REWIND_DISABLED",
                message: "Rewind capture is off — enable it via configure(['rewind' => true]) first"
            )
            }
        }
    }

    /// Merge system-specific options (per-system emulation toggles).
    class SetSystemOptions: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]
            let toggles = coreToggles(from: options)
            if !toggles.isEmpty { renderer.setCoreOptions(toggles) }
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    class FastForward: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let enabled = parameters["enabled"] as? Bool ?? false
            renderer.fastForward = enabled
            return BridgeResponse.success(data: ["status": enabled ? "fast" : "normal"])
        }
    }

    /// Merge a per-port controller remap: each `emulated => source` pair sets the
    /// in-game button `emulated` to read the positional input of `source`. Names
    /// are validated natively against the staged system; an empty map resets the
    /// port to defaults. Validation failures are category-A programmer errors the
    /// PHP wrapper raises synchronously.
    class SetInputMapping: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let port = int(parameters["port"]) else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "port is required")
            }
            // PHP encodes an empty map as a JSON array ([]); accept it as the
            // reset-to-defaults case rather than a malformed map.
            let mappings: [String: Any]
            if let map = parameters["mappings"] as? [String: Any] {
                mappings = map
            } else if let list = parameters["mappings"] as? [Any], list.isEmpty {
                mappings = [:]
            } else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "mappings map is required")
            }

            let pairs = mappings.map { (key: $0.key, value: "\($0.value)") }
            let result = renderer.setInputMapping(
                port: port,
                emulated: pairs.map(\.key),
                source: pairs.map(\.value))
            if result.isEmpty {
                return BridgeResponse.success(data: ["status": "mapped", "count": pairs.count])
            }
            // Native returns "CODE" or "CODE:detail" — re-raise as a bridge error
            // (code held in a variable so it stays off the enum drift scan).
            let code = String(result.split(separator: ":", maxSplits: 1)[0])
            let detail = result.contains(":")
                ? String(result.split(separator: ":", maxSplits: 1)[1]) : ""
            let message: String
            switch code {
            case "SYSTEM_NOT_LOADED":  message = "no system is loaded"
            case "INVALID_PARAMETERS": message = "invalid port or mismatched mappings"
            case "UNKNOWN_BUTTON":     message = "Unknown button: \(detail)"
            default:                   message = result
            }
            return BridgeResponse.error(code: code, message: message)
        }
    }

    /// Register (or swap) the device on a port — controllers are explicit, never
    /// auto-allocated. An absent/empty device disconnects the port.
    class ConnectDevice: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let port = int(parameters["port"]) else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "port is required")
            }
            let device = parameters["device"] as? String ?? ""
            switch renderer.connectDevice(port: port, device: device) {
            case "":
                return BridgeResponse.success(data: [
                    "status": "connected", "port": port, "device": device,
                    // Logical ports this device occupies (4 for a multitap) — one
                    // Controller handle each on the PHP side.
                    "ports": renderer.devicePorts(port: port),
                ])
            case "SYSTEM_NOT_LOADED":
                return BridgeResponse.error(code: "SYSTEM_NOT_LOADED", message: "no system is loaded")
            case "UNSUPPORTED_DEVICE":
                return BridgeResponse.error(code: "UNSUPPORTED_DEVICE", message: "device not supported: \(device)")
            default:
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "invalid port for this system")
            }
        }
    }

    /// Accumulate a relative axis delta on a port (mouse / light-gun X/Y).
    class SetAxis: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = int(parameters["port"]) ?? 1
            guard let axis = parameters["axis"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "axis is required")
            }
            let value = int(parameters["value"]) ?? 0
            return statusResponse(
                renderer.setAxis(port: port, name: axis, value: value),
                success: ["status": "ok", "axis": axis, "value": value])
        }
    }

    /// Aim a light-gun at a normalized (0..1) screen position.
    class AimAt: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = int(parameters["port"]) ?? 1
            guard let x = (parameters["x"] as? NSNumber)?.floatValue else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "x is required")
            }
            guard let y = (parameters["y"] as? NSNumber)?.floatValue else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "y is required")
            }
            return statusResponse(
                renderer.aimAt(port: port, x: x, y: y),
                success: ["status": "ok", "x": x, "y": y])
        }
    }

    /// Gate rumble forwarding: while enabled, motor state published by the
    /// emulated hardware (SFC Rumble Gamepad, GB MBC5 rumble carts) drives
    /// CoreHaptics. The response reports whether this
    /// device supports haptics at all.
    class SetRumble: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let enabled = parameters["enabled"] as? Bool ?? false
            renderer.setRumbleEnabled(enabled)
            return BridgeResponse.success(data: [
                "status": enabled ? "enabled" : "disabled",
                "hasVibrator": renderer.hasHaptics,
            ])
        }
    }

    /// Apply a librashader `.slangp` preset by path; null/"none"/"" clears it
    /// (passthrough). The Metal filter chain is (re)built on the caller's
    /// thread. A preset that fails to load surfaces as an EmulatorError
    /// (SHADER_FAILED), so the fluent command still returns cleanly.
    class SetShader: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let raw = parameters["path"] as? String
            let path = (raw == nil || raw == "none" || raw!.isEmpty) ? nil : raw
            guard renderer.syncSetShader(path: path) else {
                return operationalError(
                    renderer, code: "SHADER_FAILED",
                    message: "Failed to load shader preset '\(path ?? "")'")
            }
            return BridgeResponse.success(data: ["status": path == nil ? "cleared" : "applied"])
        }
    }

    /// Register a cheat in ares' raw format: hex "ADDR:VALUE" pairs joined
    /// with '+' (e.g. "7E0010:01+7E0011:FF"). The value overrides every CPU
    /// read of the address while active. Re-adding a code replaces it; cheats
    /// clear automatically when a new ROM loads. Game Genie / GameShark
    /// formats are not parsed (not supported upstream in ares).
    class AddCheat: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let code = parameters["code"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "code is required")
            }
            guard renderer.addCheat(code: code) else {
                return operationalError(
                    renderer,
                    code: "INVALID_CHEAT",
                    message: "No valid ADDR:VALUE pairs in '\(code)' — expected hex pairs joined with '+'"
                )
            }
            return BridgeResponse.success(data: ["status": "added", "code": code])
        }
    }

    /// Remove a cheat by its exact code string. Removing an inactive code is not an error.
    class RemoveCheat: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let code = parameters["code"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "code is required")
            }
            let removed = renderer.removeCheat(code: code)
            return BridgeResponse.success(data: ["status": removed ? "removed" : "not_found", "code": code])
        }
    }

    /// Deactivate all cheats. Idempotent.
    class ClearCheats: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.clearCheats()
            return BridgeResponse.success(data: ["status": "cleared"])
        }
    }

    /// Set a single button pressed on a port, resolved against its device.
    class PressButton: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = int(parameters["port"]) ?? 1
            guard let button = parameters["button"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "button is required")
            }
            return statusResponse(
                renderer.pressButton(port: port, name: button, down: true),
                success: ["status": "pressed", "button": button])
        }
    }

    /// Release a single button on a port.
    class ReleaseButton: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = int(parameters["port"]) ?? 1
            guard let button = parameters["button"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "button is required")
            }
            return statusResponse(
                renderer.pressButton(port: port, name: button, down: false),
                success: ["status": "released", "button": button])
        }
    }

    /// Merge multiple button states on a port. Unknown names are skipped.
    class SetButtons: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = int(parameters["port"]) ?? 1
            guard let state = parameters["state"] as? [String: Any] else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "state map is required")
            }
            for (button, raw) in state {
                let pressed = (raw as? NSNumber)?.boolValue ?? (raw as? Bool ?? false)
                _ = renderer.pressButton(port: port, name: button, down: pressed)
            }
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Capture the current frame as a PNG saved to internal storage; returns its path.
    class Screenshot: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let png = renderer.syncScreenshot() else {
                return BridgeResponse.error(code: "SCREENSHOT_FAILED", message: "No frame available or emulator not running")
            }
            let dir = documentsURL("screenshots")
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let url = dir.appendingPathComponent("screenshot_\(Int(Date().timeIntervalSince1970 * 1000)).png")
            do {
                try png.write(to: url)
                return BridgeResponse.success(data: ["status": "captured", "path": url.path])
            } catch {
                return BridgeResponse.error(code: "WRITE_FAILED", message: error.localizedDescription)
            }
        }
    }

    /// Return the current emulator status: "stopped", "loading", "running", or "paused".
    class GetStatus: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let renderer = renderer(parameters)
            let status = renderer?.currentStatus ?? "stopped"

            var payload: [String: Any] = ["status": status]
            // Read back from the running core (which PPU is actually bound),
            // not from what was requested. Absent on cores with one renderer.
            switch renderer?.bootOption("Pixel Accuracy") {
            case "true": payload["accuracy"] = "accurate"
            case "false": payload["accuracy"] = "performance"
            default: break
            }
            return BridgeResponse.success(data: payload)
        }
    }

    /// Return the region of the loaded ROM ("NTSC", "PAL", or empty).
    class GetRegion: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            return BridgeResponse.success(data: ["region": renderer.getRegion()])
        }
    }

    /// Return controller ports and available button names for the loaded system.
    /// Buttons held on a port, by name — see Controller::pressed().
    class GetPressedButtons: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let port = (parameters["port"] as? Int) ?? 1
            let pressed = renderer.pressedButtons(port: port)
                .split(separator: ",")
                .map(String.init)

            return BridgeResponse.success(data: ["port": port, "buttons": pressed])
        }
    }

    class GetPorts: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let json = renderer.getPortsJson()
            let ports = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [Any] ?? []
            return BridgeResponse.success(data: ["ports": ports])
        }
    }

    /// Return all supported ares systems as rich objects (static list — no native call).
    class GetSystems: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let compiled = Set(EmulatorRenderer.supportedSystems)
            func system(_ id: String, _ name: String, stable: Bool) -> [String: Any] {
                ["id": id, "name": name, "stable": stable,
                 "supported": compiled.contains(id)]
            }
            let systems: [[String: Any]] = [
                system("fc",  "NES / Famicom",             stable: true),
                system("sfc", "SNES / Super Famicom",      stable: true),
                system("gb",  "Game Boy",                  stable: true),
                system("gbc", "Game Boy Color",            stable: true),
                system("gba", "Game Boy Advance",          stable: true),
                system("md",  "Sega Mega Drive / Genesis", stable: true),
            ]
            return BridgeResponse.success(data: ["systems": systems])
        }
    }

    /// Names of hardware controllers iOS currently reports as connected —
    /// paired MFi/Bluetooth gamepads (DualSense, Xbox, etc.). The on-screen
    /// overlay works whether or not this is empty.
    class GetInputDevices: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let devices = GCController.controllers().map { controller in
                controller.vendorName ?? "Controller"
            }
            return BridgeResponse.success(data: ["devices": devices])
        }
    }
}
