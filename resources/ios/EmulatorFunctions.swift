import Foundation

/// iOS-side `Emulator.*` bridge functions for the NativePHP Retro Emulator plugin.
///
/// Mirrors the Android registration in `resources/android/EmulatorFunctions.kt`:
/// each nested class handles one bridge function declared in `nativephp.json`, and a
/// `NativeEventForwarder` turns renderer callbacks into NativePHP events. The surface
/// registry maps the `name` from `<native-emulator name="…" />` to its `EmulatorRenderer`.
///
/// Threading:
///  - Bridge functions run on an arbitrary NativePHP bridge thread.
///  - `ctx`-critical ares operations are serialised inside `EmulatorRenderer` (`emuLock`).
///  - Events are dispatched to PHP on the main thread via `NativeElementBridge`.
enum EmulatorFunctions {

    // MARK: - Surface registry

    private static let registryLock = NSLock()
    private static var surfaces: [String: EmulatorRenderer] = [:]

    /// Register an `EmulatorRenderer` under `name` and install the event forwarder.
    /// Called by the renderer when it is added to the native layout tree.
    static func register(name: String, renderer: EmulatorRenderer) {
        renderer.eventListener = NativeEventForwarder(surface: name)
        registryLock.lock()
        surfaces[name] = renderer
        registryLock.unlock()
    }

    /// Remove a surface from the registry (called when the component is torn down).
    static func unregister(name: String) {
        registryLock.lock()
        let renderer = surfaces.removeValue(forKey: name)
        registryLock.unlock()
        renderer?.eventListener = nil
    }

    // MARK: - Internal helpers

    private static func surfaceName(_ parameters: [String: Any]) -> String {
        parameters["surface"] as? String ?? "main"
    }

    private static func renderer(_ parameters: [String: Any]) -> EmulatorRenderer? {
        registryLock.lock()
        defer { registryLock.unlock() }
        return surfaces[surfaceName(parameters)]
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

    // =========================================================================
    // Bridge functions
    // =========================================================================

    /// Bind to the named surface declared in the component tree.
    class Boot: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard renderer(parameters) != nil else { return surfaceNotFound(parameters) }
            return BridgeResponse.success(data: ["status": "bound", "surface": surfaceName(parameters)])
        }
    }

    /// Initialise the ares core for a system. Supported systems are the ones
    /// compiled into the native library — reported by GetSystems with
    /// `supported: true`. System firmware (SFC ipl.rom + boards.bml, GB boot
    /// ROM, MD TMSS) is embedded; no biosPath is needed for these systems.
    /// config keys: biosPath (String?), autoSave, speed, runAhead, rewind, rewindBufferSeconds.
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
            renderer.setRunAhead(enabled: runAhead == 1)
            renderer.configureRewind(
                enabled: config["rewind"] as? Bool ?? false,
                bufferSeconds: (config["rewindBufferSeconds"] as? NSNumber)?.intValue ?? 0
            )
            if let speed = (config["speed"] as? NSNumber)?.doubleValue {
                renderer.speedMultiplier = min(max(speed, 0.25), 4.0)
            }
            guard renderer.loadSystem(system) else {
                return BridgeResponse.error(code: "LOAD_FAILED", message: "ares_load_system failed for '\(system)'")
            }

            return BridgeResponse.success(data: ["status": "loading", "system": system])
        }
    }

    /// Load a ROM from `path` and start emulation. Fire-and-forget — PHP receives
    /// `{"status":"loading"}`; `EmulatorStarted` fires on the first rendered frame.
    class LoadRom: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let path = parameters["path"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "path is required")
            }
            guard FileManager.default.fileExists(atPath: path),
                  let romData = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                return BridgeResponse.error(code: "ROM_NOT_FOUND", message: "ROM not found: \(path)")
            }

            // Battery saves live in app support, keyed by surface + ROM basename.
            let saveDir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("saves/\(surfaceName(parameters))", isDirectory: true)
            try? FileManager.default.createDirectory(at: saveDir, withIntermediateDirectories: true)
            let base = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
            let savePrefix = saveDir.appendingPathComponent(base).path

            guard renderer.loadRom(romData, path: path, savePrefix: savePrefix) else {
                return BridgeResponse.error(code: "LOAD_FAILED", message: "ares_load_rom rejected \(path)")
            }
            return BridgeResponse.success(data: ["status": "loading", "path": path])
        }
    }

    /// Pause emulation.
    class Pause: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.pauseEmulation()
            return BridgeResponse.success(data: ["status": "paused"])
        }
    }

    /// Resume emulation.
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

    /// Save state to slot.
    class StateSave: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let slot = parameters["slot"] ?? 1
            let dir = documentsURL("states", surfaceName(parameters))
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let path = dir.appendingPathComponent("\(slot).state").path

            guard renderer.syncStateSave(path: path) else {
                return BridgeResponse.error(code: "SAVE_FAILED", message: "State save failed for slot \(slot)")
            }
            return BridgeResponse.success(data: ["status": "saved", "slot": slot, "path": path])
        }
    }

    /// Load state from slot.
    class StateLoad: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let slot = parameters["slot"] ?? 1
            let url = documentsURL("states", surfaceName(parameters), "\(slot).state")
            guard FileManager.default.fileExists(atPath: url.path) else {
                return BridgeResponse.error(code: "SLOT_EMPTY", message: "No state in slot \(slot)")
            }
            guard renderer.syncStateLoad(path: url.path) else {
                return BridgeResponse.error(code: "LOAD_FAILED", message: "State load failed for slot \(slot)")
            }
            return BridgeResponse.success(data: ["status": "loaded", "slot": slot])
        }
    }

    /// Undo most recent state save (removes the undo-slot file).
    class UndoStateSave: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let url = documentsURL("states", surfaceName(parameters), "undo_save.state")
            let deleted = (try? FileManager.default.removeItem(at: url)) != nil
            return BridgeResponse.success(data: ["status": deleted ? "undone" : "nothing_to_undo"])
        }
    }

    /// Undo most recent state load (re-applies the undo-load backup slot).
    class UndoStateLoad: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let url = documentsURL("states", surfaceName(parameters), "undo_load.state")
            guard FileManager.default.fileExists(atPath: url.path) else {
                return BridgeResponse.success(data: ["status": "nothing_to_undo"])
            }
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard renderer.syncStateLoad(path: url.path) else {
                return BridgeResponse.error(code: "UNDO_FAILED", message: "Undo state load failed")
            }
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

    /// Clear all memory watches.
    class ClearMemoryWatches: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            renderer.clearMemoryWatches()
            return BridgeResponse.success(data: ["status": "cleared"])
        }
    }

    /// Merge audio options. volume 0–100 (default 100), balance −100 … +100
    /// (default 0). Applied in the native mixer.
    class SetAudio: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]
            let volume  = ((options["volume"]  as? NSNumber)?.floatValue ?? 100) / 100
            let balance = ((options["balance"] as? NSNumber)?.floatValue ?? 0) / 100
            renderer.setAudioOptions(volume: volume, balance: balance)
            return BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Merge video options — luminance/saturation 0–100, gamma 1.0–2.0,
    /// colorBleed/interframeBlending booleans, applied on the ares screen node.
    /// Options ares has no post-processing hook for are reported back as ignored.
    class SetVideo: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]
            renderer.setVideoOptions(
                luminance:  ((options["luminance"]  as? NSNumber)?.floatValue ?? 100) / 100,
                saturation: ((options["saturation"] as? NSNumber)?.floatValue ?? 100) / 100,
                gamma:      (options["gamma"] as? NSNumber)?.floatValue ?? 1.0,
                colorBleed: options["colorBleed"] as? Bool ?? false,
                interframeBlending: options["interframeBlending"] as? Bool ?? false
            )
            let ignored = options.keys.filter {
                ["colorEmulation", "deepBlackBoost", "overscan", "pixelAccuracy"].contains($0)
            }
            return ignored.isEmpty
                ? BridgeResponse.success(data: ["status": "ok"])
                : BridgeResponse.success(data: ["status": "ok", "ignored": ignored])
        }
    }

    /// Merge general live options. speed (0.25–4.0) scales the tick budget.
    /// runAhead accepts 0 or 1 — ares supports exactly one hidden frame.
    /// rewind toggles snapshot capture; rewindBufferSeconds sizes the history.
    class Configure: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let options = parameters["options"] as? [String: Any] ?? [:]

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

    /// Merge system-specific options. Stubbed.
    class SetSystemOptions: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            BridgeResponse.success(data: ["status": "ok"])
        }
    }

    /// Toggle fast-forward mode.
    class FastForward: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            let enabled = parameters["enabled"] as? Bool ?? false
            renderer.fastForward = enabled
            return BridgeResponse.success(data: ["status": enabled ? "fast" : "normal"])
        }
    }

    /// Custom controller mappings are NOT implemented in v1 — hardware
    /// mappings are hardwired in EmulatorInput.
    class SetInputMapping: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            BridgeResponse.error(code: "NOT_IMPLEMENTED", message: "custom input mappings are not supported in v1")
        }
    }

    /// Rumble is NOT implemented in v1.
    class SetRumble: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            BridgeResponse.error(code: "NOT_IMPLEMENTED", message: "rumble is not supported in v1")
        }
    }

    /// Shaders (librashader) are NOT implemented in v1. Passing nil/"none"
    /// (a clear) succeeds — there is never an active shader to remove.
    class SetShader: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            let path = parameters["path"] as? String
            if path == nil || path == "none" {
                return BridgeResponse.success(data: ["status": "cleared"])
            }
            return BridgeResponse.error(code: "NOT_IMPLEMENTED", message: "shaders are not supported in v1")
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
                return BridgeResponse.error(
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

    /// Set a single button pressed in the software input state map.
    class PressButton: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let button = parameters["button"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "button is required")
            }
            guard renderer.pressButton(button) else {
                return BridgeResponse.error(code: "UNKNOWN_BUTTON", message: "Unknown button: \(button)")
            }
            return BridgeResponse.success(data: ["status": "pressed", "button": button])
        }
    }

    /// Release a single button in the software input state map.
    class ReleaseButton: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let button = parameters["button"] as? String else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "button is required")
            }
            guard renderer.releaseButton(button) else {
                return BridgeResponse.error(code: "UNKNOWN_BUTTON", message: "Unknown button: \(button)")
            }
            return BridgeResponse.success(data: ["status": "released", "button": button])
        }
    }

    /// Atomically merge multiple button states into the software input state map.
    class SetButtons: BridgeFunction {
        func execute(parameters: [String: Any]) throws -> [String: Any] {
            guard let renderer = renderer(parameters) else { return surfaceNotFound(parameters) }
            guard let state = parameters["state"] as? [String: Any] else {
                return BridgeResponse.error(code: "INVALID_PARAMETERS", message: "state map is required")
            }
            let coerced = state.mapValues { ($0 as? NSNumber)?.boolValue ?? ($0 as? Bool ?? false) }
            renderer.setButtons(coerced)
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
            let status = renderer(parameters)?.currentStatus ?? "stopped"
            return BridgeResponse.success(data: ["status": status])
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
            func system(_ id: String, _ name: String, biosRequired: Bool, stable: Bool) -> [String: Any] {
                ["id": id, "name": name, "biosRequired": biosRequired, "stable": stable,
                 "supported": compiled.contains(id)]
            }
            let systems: [[String: Any]] = [
                system("fc",  "NES / Famicom",            biosRequired: false, stable: true),
                system("sfc", "SNES / Super Famicom",      biosRequired: false, stable: true),
                system("n64", "Nintendo 64",               biosRequired: false, stable: true),
                system("gb",  "Game Boy",                  biosRequired: false, stable: true),
                system("gbc", "Game Boy Color",            biosRequired: false, stable: true),
                system("gba", "Game Boy Advance",          biosRequired: false, stable: true),
                system("sg",  "Sega SG-1000",              biosRequired: false, stable: true),
                system("ms",  "Sega Master System",        biosRequired: false, stable: true),
                system("md",  "Sega Mega Drive / Genesis", biosRequired: false, stable: true),
                system("pce", "PC Engine / TurboGrafx-16", biosRequired: false, stable: true),
                system("ngp", "Neo Geo Pocket",            biosRequired: false, stable: true),
                system("ws",  "WonderSwan",                biosRequired: false, stable: true),
                system("ps1", "PlayStation",               biosRequired: true,  stable: false),
                system("ng",  "Neo Geo AES / MVS",         biosRequired: true,  stable: false),
                system("a26", "Atari 2600",                biosRequired: false, stable: false),
                system("msx", "MSX / MSX2",                biosRequired: true,  stable: false),
            ]
            return BridgeResponse.success(data: ["systems": systems])
        }
    }
}
