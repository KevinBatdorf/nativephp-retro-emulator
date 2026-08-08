package com.kevinbatdorf.plugins.retroemulator

import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.InputDevice
import androidx.fragment.app.FragmentActivity
import com.nativephp.mobile.bridge.BridgeFunction
import com.nativephp.mobile.bridge.BridgeResponse
import com.nativephp.mobile.utils.NativeActionCoordinator
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Bridge functions for the NativePHP Retro Emulator plugin.
 *
 * Each inner class handles one bridge function declared in nativephp.json.
 * The static registry ([registerSurface] / [unregisterSurface]) is populated
 * by the NativePHP component system when `<native:emulator name="..." />` is
 * rendered into the native layout.
 *
 * Threading model:
 *  - Bridge functions are called on an arbitrary NativePHP bridge thread.
 *  - render-thread-critical ares operations are posted to the render thread via [EmulatorRenderer].
 *  - NativePHP events are always dispatched on the main thread.
 */
object EmulatorFunctions {

    private const val TAG = "EmulatorFunctions"

    // Per-system emulation toggles carried on loadSystem() config and
    // setSystemOptions(). Native maps each to its ares node and no-ops where the
    // core doesn't declare it (see native/core_options.hpp).
    private val CORE_TOGGLE_KEYS = listOf("colorEmulation", "deepBlackBoost", "interframeBlending", "showIcons")

    private data class SurfaceEntry(
        val renderer: EmulatorRenderer,
        val activity: FragmentActivity,
    )

    private val surfaces = java.util.concurrent.ConcurrentHashMap<String, SurfaceEntry>()

    // Last slot each surface saved to — undoStateSave reverts that slot's file
    // (ares state.undoSlot, desktop-ui/program/states.cpp:8). In-memory like
    // ares; a fresh process forgets it and undo reports nothing_to_undo.
    private val undoSaveSlot = java.util.concurrent.ConcurrentHashMap<String, String>()

    /**
     * Register an [EmulatorRenderer] under [name] so bridge functions can locate it.
     * Called by the NativePHP component system after creating the renderer.
     */
    @JvmStatic
    fun registerSurface(name: String, renderer: EmulatorRenderer, activity: FragmentActivity) {
        renderer.eventListener = NativeEventForwarder(name, activity)
        surfaces[name] = SurfaceEntry(renderer, activity)
        Log.d(TAG, "Surface registered: $name")
    }

    /**
     * Remove a surface from the registry — but only if [renderer] still owns the
     * slot. During a fast remount (navigation / recomposition) the EDGE element
     * can instantiate a new renderer that re-registers under the same name
     * BEFORE the old instance's onRelease fires. A blind remove-by-name would
     * then wipe the live renderer's entry, stranding every bridge call with
     * SURFACE_NOT_FOUND. Compare identity so a superseded teardown leaves the
     * live entry in place.
     */
    @JvmStatic
    fun unregisterSurface(name: String, renderer: EmulatorRenderer) {
        renderer.eventListener = null
        var removed = false
        surfaces.computeIfPresent(name) { _, entry ->
            if (entry.renderer === renderer) {
                removed = true
                null
            } else {
                entry
            }
        }
        Log.d(TAG, if (removed) {
            "Surface unregistered: $name"
        } else {
            "Surface unregister skipped — '$name' owned by a newer instance"
        })
    }

    /**
     * The renderer driving [name], for sibling elements that feed input straight
     * into the core. Null until the emulator surface mounts, so an overlay laid
     * out beside it simply drops presses until there is a core to receive them.
     */
    @JvmStatic
    fun rendererFor(name: String): EmulatorRenderer? = surfaces[name]?.renderer

    /**
     * Apply a `<native:emulator>` element's declarative setup — the same
     * staging → boot path Emulator::loadSystem() / loadRom() drive imperatively,
     * routed through the real bridge appliers so behavior is identical by
     * construction. [configJson] is the merged effective config (global ⊕
     * system, inputCapture already hoisted onto the surface) as a JSON object
     * string. Runs off the caller's thread: LoadRom reads the ROM file inline
     * and the appliers block on the render thread.
     */
    @JvmStatic
    fun applyDeclarativeSetup(surface: String, system: String, configJson: String, rom: String) {
        if (system.isEmpty()) return
        val activity = surfaces[surface]?.activity ?: return

        Thread {
            val config = if (configJson.isNotEmpty()) {
                runCatching { JSONObject(configJson) }.getOrDefault(JSONObject())
            } else {
                JSONObject()
            }

            // Stage the system with its playback/system config — LoadSystem
            // reads the staged + core-toggle keys and ignores the presentation
            // keys, which go out on their own channels.
            LoadSystem(activity).execute(params(JSONObject()
                .put("surface", surface).put("system", system).put("config", config)))

            if (surfaces[surface]?.renderer?.stagedSystemId != system) {
                Log.e(TAG, "Declarative boot: system '$system' did not stage on '$surface'")
                return@Thread
            }

            // Fan the presentation/AV knobs out, mirroring applyPresentation().
            // SetVideo null-merges so it's safe to call unconditionally; SetAudio
            // defaults the missing key, so gate it on an actual audio knob.
            SetVideo(activity).execute(params(JSONObject()
                .put("surface", surface).put("options", config)))
            if (config.has("volume") || config.has("balance")) {
                SetAudio(activity).execute(params(JSONObject()
                    .put("surface", surface).put("options", config)))
            }
            if (config.has("rumble")) {
                SetRumble(activity).execute(params(JSONObject()
                    .put("surface", surface).put("enabled", config.getBoolean("rumble"))))
            }
            if (config.has("shader")) {
                SetShader(activity).execute(params(JSONObject()
                    .put("surface", surface).put("path", config.getString("shader"))))
            }

            // Boot the staged system with the ROM (every load is a fresh boot).
            if (rom.isNotEmpty()) {
                LoadRom(activity).execute(params(JSONObject()
                    .put("surface", surface).put("path", rom)))
            }
        }.also { it.name = "emu-declarative-boot-$surface"; it.start() }
    }

    // Unmarshal a payload exactly as the bridge router does, so the appliers
    // receive params identical to a real nativephp_call.
    private fun params(payload: JSONObject): Map<String, Any> =
        BridgeParams.unmarshalLikeBridgeRouter(payload.toString())

    private fun surface(parameters: Map<String, Any>): String =
        parameters["surface"] as? String ?: "main"

    private fun paramList(parameters: Map<String, Any>, key: String): List<Any?>? =
        BridgeParams.list(parameters, key)

    private fun paramMap(parameters: Map<String, Any>, key: String): Map<String, Any>? =
        BridgeParams.map(parameters, key)

    /**
     * Scale a whole-percent option to the native 0..1 range. Out of range fails
     * rather than clamps: a gamma of 1, mistaken for ares' 1.0-2.0 exponent,
     * renders an almost black screen, and accepting that silently is worse.
     */
    private sealed class Percent {
        class Value(val scaled: Float?) : Percent()
        class Failure(val response: Map<String, Any>) : Percent()
    }

    private fun percent(
        options: Map<String, Any>,
        key: String,
        min: Int,
        max: Int,
    ): Percent {
        val raw = options[key] as? Number ?: return Percent.Value(null)
        val value = raw.toDouble()
        if (value < min || value > max) {
            return Percent.Failure(BridgeResponse.error(
                "INVALID_PARAMETERS",
                "$key is a whole percentage ($min-$max, 100 = unchanged) — got $value",
            ))
        }

        return Percent.Value(value.toFloat() / 100f)
    }

    // Native input calls return "" on success or "CODE"/"CODE:detail" on a
    // category-A error. Map that to a bridge response; the code stays in a
    // variable so it stays off PluginTest's error-code drift scan.
    private fun statusResponse(result: String, success: Map<String, Any>): Map<String, Any> {
        if (result.isEmpty()) return BridgeResponse.success(success)
        val code = result.substringBefore(':')
        val detail = result.substringAfter(':', "")
        val message = when (code) {
            "SYSTEM_NOT_LOADED"  -> "no system is loaded"
            "UNKNOWN_BUTTON"     -> "Unknown button: $detail"
            else -> "invalid parameters"
        }
        return BridgeResponse.error(code, message)
    }

    private fun entry(parameters: Map<String, Any>): Pair<SurfaceEntry?, Map<String, Any>?> {
        val name = surface(parameters)

        // A screen's mount() runs before Compose has rendered the emulator
        // node, so the first Boot/LoadSystem arrives just ahead of
        // registerSurface. Briefly await registration instead of failing.
        val deadline = System.currentTimeMillis() + 3_000
        var entry = surfaces[name]
        while (entry == null && System.currentTimeMillis() < deadline) {
            Thread.sleep(25)
            entry = surfaces[name]
        }

        return if (entry != null) {
            entry to null
        } else {
            null to BridgeResponse.error("SURFACE_NOT_FOUND", "No surface '$name' registered")
        }
    }

    private fun dispatchEvent(activity: FragmentActivity, fqcn: String, payload: JSONObject) {
        Handler(Looper.getMainLooper()).post {
            NativeActionCoordinator.dispatchEvent(activity, fqcn, payload.toString())
        }
    }

    /**
     * Report an operational outcome (category B): a valid call that failed
     * at runtime — a missing ROM, an empty slot, a failed save. These surface as an
     * EmulatorError event, never as a bridge error, so the PHP wrapper returns
     * fluently and the event carries the detail. The "failed" status keeps the
     * response off the wrapper's throw path (which fires on "error").
     */
    private fun operationalError(entry: SurfaceEntry, code: String, message: String): Map<String, Any> {
        entry.renderer.eventListener?.onError(code, message)
        return BridgeResponse.success(mapOf("status" to "failed", "code" to code, "message" to message))
    }

    private class NativeEventForwarder(
        private val surface: String,
        private val activity: FragmentActivity,
    ) : EmulatorEventListener {

        override fun onStarted(system: String, romPath: String) {
            val payload = JSONObject().apply {
                put("surface", surface)
                put("system", system)
                put("romPath", romPath)
            }
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\EmulatorStarted", payload)
        }

        override fun onStopped() {
            val payload = JSONObject().put("surface", surface)
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\EmulatorStopped", payload)
        }

        override fun onPaused() {
            val payload = JSONObject().put("surface", surface)
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\EmulatorPaused", payload)
        }

        override fun onResumed() {
            val payload = JSONObject().put("surface", surface)
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\EmulatorResumed", payload)
        }

        override fun onMemoryChanged(address: Int, oldValue: Int, newValue: Int) {
            val payload = JSONObject().apply {
                put("surface", surface)
                put("address", address)
                put("oldValue", oldValue)
                put("newValue", newValue)
            }
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\MemoryChanged", payload)
        }

        override fun onMemoryRead(address: Int, bytes: ByteArray) {
            val payload = JSONObject().apply {
                put("surface", surface)
                put("address", address)
                put("bytes", JSONArray(bytes.map { it.toInt() and 0xFF }))
            }
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\MemoryRead", payload)
        }

        override fun onError(code: String, message: String) {
            val payload = JSONObject().apply {
                put("surface", surface)
                put("code", code)
                put("message", message)
            }
            dispatchEvent(activity, "KevinBatdorf\\RetroEmulator\\Events\\EmulatorError", payload)
        }
    }

    /**
     * Bind to the named surface declared in the component tree.
     * The renderer is created by the NativePHP component system and registered
     * via [registerSurface] before PHP calls Boot().
     */
    class Boot(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            Log.d(TAG, "Boot: surface=${surface(parameters)}")
            return BridgeResponse.success(mapOf("status" to "bound", "surface" to surface(parameters)))
        }
    }

    /**
     * STAGE a system declaration — no core boots until LoadRom arrives with a
     * ROM (every boot is ROM-first so the region variant is always
     * right). Supported systems are the ones compiled into the native
     * library — reported by [GetSystems] with `supported: true`. System
     * firmware (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS) is embedded;
     * no biosPath is needed for these systems.
     * config keys: biosPath (String?), autoSave (Bool), speed (Float), runAhead (Int),
     *              rewind (Bool), rewindBufferSeconds (Int), dynamicRateControl (Bool),
     *              pixelAccuracy (Bool — boot-only renderer choice, see Configure),
     *              region (String, e.g. "PAL" — overrides ROM analysis),
     *              preferredRegions (List<String> — preference order for
     *              multi-region ROMs; default matches desktop's "NTSC-U").
     */
    class LoadSystem(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val system = parameters["system"] as? String ?: "sfc"
            val supported = EmulatorCore().supportedSystems().split(",")
            if (system !in supported) {
                return BridgeResponse.error(
                    "UNSUPPORTED_SYSTEM",
                    "System '$system' is not supported in this build — available: ${supported.joinToString(", ")}",
                )
            }

            val config = paramMap(parameters, "config") ?: emptyMap()
            val runAhead = (config["runAhead"] as? Number)?.toInt() ?: 0
            if (runAhead !in 0..1) {
                return BridgeResponse.error(
                    "INVALID_PARAMETERS",
                    "runAhead must be 0 or 1 — ares supports one hidden frame",
                )
            }

            val renderer = entry!!.renderer
            renderer.fastForward = false
            renderer.autoSave = config["autoSave"] as? Boolean ?: true
            renderer.stagedRegion = config["region"] as? String ?: ""
            renderer.stagedPreferredRegions =
                (paramList(config, "preferredRegions") ?: emptyList())
                    .filterIsInstance<String>().joinToString(",")
            renderer.queueSetRunAhead(runAhead == 1)
            renderer.queueSetDynamicRateControl(config["dynamicRateControl"] as? Boolean ?: true)
            renderer.queueConfigureRewind(
                config["rewind"] as? Boolean ?: false,
                (config["rewindBufferSeconds"] as? Number)?.toInt() ?: 0,
            )
            (config["speed"] as? Number)?.toDouble()?.let { speed ->
                renderer.speedMultiplier = speed.coerceIn(0.25, 4.0)
            }
            // An explicitly requested engine must exist for this system —
            // a dev asking for one never silently gets another. The config
            // map arrives as backendPreferences instead: try each in order,
            // skip whatever isn't present, land on the built-in engine.
            val backend = config["backend"] as? String
            val preferences = if (backend == null) {
                (config["backendPreferences"] as? List<*>)?.map { it.toString() }.orEmpty()
            } else {
                emptyList()
            }
            var availableForSystem = emptyList<String>()
            var bundled = emptySet<String>()
            if (backend != null || preferences.isNotEmpty()) {
                val engines = JSONObject(EmulatorCore().backendsJson())
                availableForSystem = engines.optJSONObject(system)?.optJSONArray("backends")
                    ?.let { list -> (0 until list.length()).map { list.getString(it) } }
                    ?: emptyList()
                bundled = engines.keys().asSequence()
                    .mapNotNull { engines.optJSONObject(it)?.optJSONArray("backends") }
                    .flatMap { list -> (0 until list.length()).map { list.getString(it) } }
                    .toSet()
                // A bundled engine that doesn't claim this system is a firm
                // no. Any other name continues to native, which probes it as
                // a bring-your-own libretro core.
                if (backend != null && backend in bundled && backend !in availableForSystem) {
                    return BridgeResponse.error(
                        "UNSUPPORTED_BACKEND",
                        "Backend '$backend' does not serve '$system' in this build — available: ${availableForSystem.joinToString(", ")}",
                    )
                }
            }

            // Boot-only: picks the renderer implementation before load. Cores
            // without a choice (fc, gb, md) accept and ignore it.
            val bootOptions = mapOf(
                "Pixel Accuracy" to (config["pixelAccuracy"] as? Boolean ?: false),
                // GB/GBC only; other backends ignore an unknown boot option.
                "rawAudio" to (config["rawAudio"] as? Boolean ?: false),
            )

            // Staging is synchronous so later validation sees the engine
            // that will actually serve this system.
            val biosPath = config["biosPath"] as? String
            var staged = false
            if (backend != null) {
                staged = renderer.stageSystem(system, biosPath, bootOptions, backend) == true
                if (!staged) {
                    return if (backend !in availableForSystem) {
                        BridgeResponse.error(
                            "UNSUPPORTED_BACKEND",
                            "No libretro core named '$backend' could be loaded for '$system' — package it under resources/emulator-cores/android/<abi>/ or use a bundled engine: ${availableForSystem.joinToString(", ")}",
                        )
                    } else {
                        BridgeResponse.error(
                            "UNSUPPORTED_SYSTEM",
                            "System '$system' failed to stage — no engine claimed it",
                        )
                    }
                }
            } else {
                for (candidate in preferences) {
                    if (candidate !in bundled || candidate in availableForSystem) {
                        if (renderer.stageSystem(system, biosPath, bootOptions, candidate) == true) {
                            staged = true
                            break
                        }
                    }
                    Log.w(
                        TAG,
                        "config prefers '$candidate' for '$system' but it isn't available — " +
                            "falling through. Run: php artisan retro-emulator:fetch-core $candidate",
                    )
                }
                if (!staged) {
                    Log.w(TAG, "'$system' is running the built-in engine (ares) — no preferred engine was available")
                    if (renderer.stageSystem(system, biosPath, bootOptions, null) != true) {
                        return BridgeResponse.error(
                            "UNSUPPORTED_SYSTEM",
                            "System '$system' failed to stage — no engine claimed it",
                        )
                    }
                }
            }

            // Engine-declared options validate against the schema the staged
            // core itself declared — a typo'd key or undeclared value errors
            // here at the call site, never a silent no-op.
            val engineOptions = (config["engineOptions"] as? Map<*, *>).orEmpty()
            for ((key, value) in engineOptions) {
                val refusal = renderer.syncSetEngineOption(
                    key.toString(), value.toString(), staged = true,
                )
                if (refusal == null || refusal.isNotEmpty()) {
                    return BridgeResponse.error(
                        "UNSUPPORTED_OPTION",
                        refusal ?: "engine option '$key' timed out applying",
                    )
                }
            }

            val coreToggles = CORE_TOGGLE_KEYS
                .mapNotNull { key -> (config[key] as? Boolean)?.let { key to it } }
                .toMap()
            // Only ENABLING an absent feature is an error; disabling one the
            // engine never had is vacuously satisfied (configs routinely send
            // explicit false defaults).
            for ((key, value) in coreToggles) {
                if (value && !EmulatorCore().toggleSupported(key)) {
                    return BridgeResponse.error(
                        "UNSUPPORTED_OPTION",
                        "Backend '${EmulatorCore().backendName()}' does not support '$key' on '$system'",
                    )
                }
            }
            if (coreToggles.isNotEmpty()) renderer.queueCoreOptions(coreToggles)

            Log.d(TAG, "LoadSystem: staged system=$system backend=${EmulatorCore().backendName()}")
            return BridgeResponse.success(
                mapOf(
                    "status" to "staged",
                    "system" to system,
                    "backend" to EmulatorCore().backendName(),
                ),
            )
        }
    }

    /**
     * Boot the staged system with the ROM at [path] — the one boot path, first
     * load and every swap alike. The ROM's extension is gated
     * against the staged system's list (desktop's file-dialog filters); a
     * wrong-family ROM errors, it never auto-switches systems. Fire-and-forget —
     * PHP receives `{"status":"loading"}` immediately; [EmulatorStarted] fires
     * on the first frame.
     */
    class LoadRom(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val path = parameters["path"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "path is required")

            val file = File(path)
            if (!file.exists()) {
                return operationalError(entry!!, "ROM_NOT_FOUND", "ROM not found: $path")
            }

            val system = entry!!.renderer.stagedSystemId
            if (system.isEmpty()) {
                return BridgeResponse.error("SYSTEM_NOT_LOADED", "Call LoadSystem before LoadRom")
            }
            val extensions = EmulatorCore().systemExtensions(system).split(",")
            val ext = file.extension.lowercase()
            if (ext !in extensions) {
                return operationalError(
                    entry,
                    "INVALID_ROM",
                    "'.$ext' is not a $system ROM — expected one of: " +
                        extensions.joinToString(", ") { ".$it" },
                )
            }

            return try {
                val romBytes = file.readBytes()
                // Battery saves live in app storage, keyed by surface + ROM
                // basename: "<filesDir>/saves/<surface>/<rom>.save.ram", etc.
                // A savePath parameter overrides the prefix entirely.
                val savePrefix = (parameters["savePath"] as? String) ?: run {
                    val saveDir = File(activity.filesDir, "saves/${surface(parameters)}")
                    saveDir.mkdirs()
                    File(saveDir, file.nameWithoutExtension).absolutePath
                }
                // Undo files belong to the outgoing game (ares clearUndoStates,
                // load.cpp:170) — a stale cross-game snapshot must not survive.
                val stateDir = File(activity.filesDir, "states/${surface(parameters)}")
                File(stateDir, "undo_save.state").delete()
                File(stateDir, "undo_load.state").delete()
                undoSaveSlot.remove(surface(parameters))

                entry.renderer.queueRomLoad(romBytes, system, path, savePrefix)
                Log.d(TAG, "LoadRom: queued $path (${romBytes.size} bytes, saves=$savePrefix)")
                BridgeResponse.success(mapOf("status" to "loading", "path" to path))
            } catch (e: Exception) {
                Log.e(TAG, "LoadRom failed: ${e.message}")
                BridgeResponse.error("READ_FAILED", e.message ?: "Failed to read ROM")
            }
        }
    }

    /**
     * Stage a Sufami Turbo slot ROM (index 0 = Slot A, 1 = Slot B) from a file
     * path, to be inserted at the next LoadRom (whose base is the SuFami BIOS).
     */
    class StageSlot(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val index = (parameters["index"] as? Number)?.toInt() ?: 0
            val path = parameters["path"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "path is required")
            val file = File(path)
            if (!file.exists()) return operationalError(entry!!, "ROM_NOT_FOUND", "slot ROM not found: $path")
            return try {
                entry!!.renderer.stageSlot(index, file.readBytes())
                BridgeResponse.success(mapOf("status" to "staged", "index" to index))
            } catch (e: Exception) {
                BridgeResponse.error("READ_FAILED", e.message ?: "Failed to read slot ROM")
            }
        }
    }

    class Pause(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.pauseEmulation()
            return BridgeResponse.success(mapOf("status" to "paused"))
        }
    }

    class Resume(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.resumeEmulation()
            return BridgeResponse.success(mapOf("status" to "running"))
        }
    }

    class Stop(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.stopEmulation()
            return BridgeResponse.success(mapOf("status" to "stopped"))
        }
    }

    class StateSave(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val slot = parameters["slot"] ?: 1
            val stateDir = File(activity.filesDir, "states/${surface(parameters)}")
            stateDir.mkdirs()
            val slotFile = File(stateDir, "$slot.state")
            val path = slotFile.absolutePath

            // The slot's previous file moves to the undo file before the new
            // state lands (ares stateSave, states.cpp:6-9), so undoStateSave
            // can revert the slot.
            if (slotFile.exists() && slotFile.renameTo(File(stateDir, "undo_save.state"))) {
                undoSaveSlot[surface(parameters)] = slot.toString()
            }

            val ok = entry!!.renderer.syncStateSave(path)
            return if (ok) {
                BridgeResponse.success(mapOf("status" to "saved", "slot" to slot, "path" to path))
            } else {
                operationalError(entry, "SAVE_FAILED", "State save failed for slot $slot")
            }
        }
    }

    class StateLoad(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val slot = parameters["slot"] ?: 1
            val stateDir = File(activity.filesDir, "states/${surface(parameters)}")
            stateDir.mkdirs()
            val statePath = File(stateDir, "$slot.state")

            // Snapshot the current state to the undo-load file before touching
            // the slot (ares stateLoad, states.cpp:26-30) — even when the slot
            // turns out to be empty, matching the reference order.
            entry!!.renderer.syncStateSave(File(stateDir, "undo_load.state").absolutePath)

            if (!statePath.exists()) {
                return operationalError(entry, "SLOT_EMPTY", "No state in slot $slot")
            }

            val ok = entry!!.renderer.syncStateLoad(statePath.absolutePath)
            return if (ok) {
                BridgeResponse.success(mapOf("status" to "loaded", "slot" to slot))
            } else {
                operationalError(entry, "LOAD_FAILED", "State load failed for slot $slot")
            }
        }
    }

    /**
     * Undo most recent state save: the slot's previous file moves back over
     * the slot (ares undoStateSave, states.cpp:46-59) — a file-level revert,
     * the machine keeps running.
     */
    class UndoStateSave(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val name = surface(parameters)
            val stateDir = File(activity.filesDir, "states/$name")
            val undoFile = File(stateDir, "undo_save.state")
            val slot = undoSaveSlot[name]
            if (slot == null || !undoFile.exists()) {
                return BridgeResponse.success(mapOf("status" to "nothing_to_undo"))
            }
            if (!undoFile.renameTo(File(stateDir, "$slot.state"))) {
                val (entry, err) = entry(parameters)
                if (err != null) return err
                return operationalError(entry!!, "UNDO_FAILED", "Unable to revert slot $slot to its previous state")
            }
            undoSaveSlot.remove(name)
            return BridgeResponse.success(mapOf("status" to "undone", "slot" to slot))
        }
    }

    /**
     * Undo most recent state load: re-apply the pre-load snapshot, then drop
     * it (ares undoStateLoad, states.cpp:61-81).
     */
    class UndoStateLoad(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val undoFile = File(activity.filesDir, "states/${surface(parameters)}/undo_load.state")
            if (!undoFile.exists()) {
                return BridgeResponse.success(mapOf("status" to "nothing_to_undo"))
            }
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val ok = entry!!.renderer.syncStateLoad(undoFile.absolutePath)
            return if (ok) {
                undoFile.delete()
                BridgeResponse.success(mapOf("status" to "undone"))
            } else {
                operationalError(entry, "UNDO_FAILED", "Undo state load failed")
            }
        }
    }

    /**
     * Synchronous WRAM read. Returns bytes immediately via the bridge response.
     * Blocks the bridge thread until the render thread services the request (≤2 s).
     */
    class ReadMemory(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val address = (parameters["address"] as? Number)?.toInt()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "address is required")
            val length = (parameters["length"] as? Number)?.toInt() ?: 1

            val bytes = entry!!.renderer.syncReadMemory(address, length)
                ?: return BridgeResponse.error(
                    "READ_FAILED",
                    "Memory read failed — address 0x${address.toString(16).uppercase()} out of range or emulator not running",
                )

            return BridgeResponse.success(mapOf(
                "address" to address,
                "bytes"   to bytes.map { it.toInt() and 0xFF },
            ))
        }
    }

    /**
     * Asynchronous WRAM read. Dispatches [MemoryRead] event with the result.
     * Returns immediately — PHP does not block.
     */
    class ReadMemoryAsync(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val address = (parameters["address"] as? Number)?.toInt()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "address is required")
            val length = (parameters["length"] as? Number)?.toInt() ?: 1
            val surfaceName = surface(parameters)

            Thread {
                val bytes = entry!!.renderer.syncReadMemory(address, length)
                if (bytes != null) {
                    entry.renderer.eventListener?.onMemoryRead(address, bytes)
                } else {
                    Log.w(TAG, "ReadMemoryAsync: read failed for 0x${address.toString(16).uppercase()}")
                }
            }.also { it.name = "emu-mem-read-$surfaceName"; it.start() }

            return BridgeResponse.success(mapOf("status" to "queued", "address" to address))
        }
    }

    /** Write bytes to WRAM. Applied before the next frame tick. */
    class WriteMemory(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val address = (parameters["address"] as? Number)?.toInt()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "address is required")

            val byteList = paramList(parameters, "bytes")
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "bytes is required")

            val bytes = byteList.mapNotNull { (it as? Number)?.toInt()?.toByte() }.toByteArray()

            // Validate the target synchronously: a read-probe of the same window
            // returns null for an out-of-range address or when no core is
            // running — exactly the cases a write must reject (WRITE_FAILED).
            if (entry!!.renderer.syncReadMemory(address, bytes.size.coerceAtLeast(1)) == null) {
                return BridgeResponse.error(
                    "WRITE_FAILED",
                    "Memory write failed — address 0x${address.toString(16).uppercase()} out of range or emulator not running",
                )
            }
            entry.renderer.queueWriteMemory(address, bytes)

            return BridgeResponse.success(mapOf("status" to "queued", "address" to address, "length" to bytes.size))
        }
    }

    /** Register/merge memory address watches. Fires [MemoryChanged] on value change. */
    class WatchMemory(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val addresses = paramList(parameters, "addresses")
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "addresses is required")

            val coerced = addresses.mapNotNull { item ->
                when (item) {
                    is Number -> item.toInt()
                    is Map<*, *> -> item
                    else -> null
                }
            }

            entry!!.renderer.addWatches(coerced)
            return BridgeResponse.success(mapOf("status" to "watching", "count" to coerced.size))
        }
    }

    class UnwatchMemory(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val addresses = paramList(parameters, "addresses")
                ?.mapNotNull { (it as? Number)?.toInt() }
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "addresses is required")

            entry!!.renderer.removeWatches(addresses)
            return BridgeResponse.success(mapOf("status" to "unwatched"))
        }
    }

    class ClearMemoryWatches(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.clearMemoryWatches()
            return BridgeResponse.success(mapOf("status" to "cleared"))
        }
    }

    /**
     * Merge audio options. volume 0–100 (default 100), balance −100 (left) …
     * +100 (right, default 0). Applied in the native mixer.
     */
    class SetAudio(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            @Suppress("UNCHECKED_CAST")
            val options = paramMap(parameters, "options") ?: emptyMap()
            // Only the knobs actually sent update; the rest keep their value.
            val volume = percent(options, "volume", 0, 100)
            if (volume is Percent.Failure) return volume.response
            val balance = percent(options, "balance", -100, 100)
            if (balance is Percent.Failure) return balance.response

            entry!!.renderer.setAudioOptions(
                (volume as Percent.Value).scaled,
                (balance as Percent.Value).scaled,
            )
            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /**
     * Merge GLOBAL display options — omitted options keep their current values,
     * and the surface's options persist across ROM/system reloads (desktop
     * reapplies its settings at load the same way). luminance/saturation
     * 0–100 and gamma 50–200 are whole percentages (100 = unchanged);
     * colorBleed/overscan are booleans, applied on the ares
     * screen node; presentation settings output (scale/integer/integerFixed/
     * stretch), fixedScale, and aspectCorrection (none/standard/anamorphic)
     * mirror ares desktop's Video settings. overscan false (default) trims the
     * borders like desktop. Per-system emulation toggles live on
     * setSystemOptions(), not here.
     */
    class SetVideo(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            @Suppress("UNCHECKED_CAST")
            val options = paramMap(parameters, "options") ?: emptyMap()
            // output/aspectCorrection are renderer-side presentation and work
            // on every engine; the screen-node settings only exist where
            // the engine has that door. Only a CHANGE is gated — 100 means
            // "unchanged" in the whole-percent contract and false is the
            // default, so neutral values are vacuously satisfied (configs
            // send explicit defaults).
            val screenChanges = buildList {
                for (key in listOf("luminance", "saturation", "gamma")) {
                    val value = (options[key] as? Number)?.toInt()
                    if (value != null && value != 100) add(key)
                }
                if (options["colorBleed"] == true) add("colorBleed")
                if (options["overscan"] == true) add("overscan")
            }
            if (screenChanges.isNotEmpty() && !EmulatorCore().videoSettingsSupported()) {
                return BridgeResponse.error(
                    "UNSUPPORTED_OPTION",
                    "Backend '${EmulatorCore().backendName()}' does not support "
                        + screenChanges.joinToString(", "),
                )
            }
            val output = options["output"] as? String
            if (output != null && output !in setOf("scale", "integer", "integerFixed", "stretch")) {
                return BridgeResponse.error(
                    "INVALID_PARAMETERS",
                    "output must be scale, integer, integerFixed, or stretch — got '$output'",
                )
            }
            val aspectCorrection = options["aspectCorrection"] as? String
            if (aspectCorrection != null && aspectCorrection !in setOf("none", "standard", "anamorphic")) {
                return BridgeResponse.error(
                    "INVALID_PARAMETERS",
                    "aspectCorrection must be none, standard, or anamorphic — got '$aspectCorrection'",
                )
            }
            val luminance = percent(options, "luminance", 0, 100)
            if (luminance is Percent.Failure) return luminance.response
            val saturation = percent(options, "saturation", 0, 100)
            if (saturation is Percent.Failure) return saturation.response
            val gamma = percent(options, "gamma", 50, 200)
            if (gamma is Percent.Failure) return gamma.response

            entry!!.renderer.queueVideoOptions(
                luminance  = (luminance as Percent.Value).scaled,
                saturation = (saturation as Percent.Value).scaled,
                gamma      = (gamma as Percent.Value).scaled,
                colorBleed = options["colorBleed"] as? Boolean,
                overscan   = options["overscan"] as? Boolean,
                output     = output,
                fixedScale = (options["fixedScale"] as? Number)?.toInt(),
                aspectCorrection = aspectCorrection,
            )
            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /**
     * Merge general live options. speed (0.25–4.0) scales the tick budget.
     * runAhead accepts 0 or 1 — ares supports exactly one hidden frame.
     * rewind toggles snapshot capture; rewindBufferSeconds sizes the history.
     */
    class Configure(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            @Suppress("UNCHECKED_CAST")
            val options = paramMap(parameters, "options") ?: emptyMap()

            if (options.containsKey("pixelAccuracy")) {
                // Even upstream only honors this at the next load — reject
                // rather than silently defer.
                return BridgeResponse.error(
                    "BOOT_ONLY_OPTION",
                    "pixelAccuracy can only be set in the LoadSystem config — " +
                        "it picks the renderer at boot; reboot the system to change it",
                )
            }

            (options["runAhead"] as? Number)?.toInt()?.let { frames ->
                if (frames !in 0..1) {
                    return BridgeResponse.error(
                        "INVALID_PARAMETERS",
                        "runAhead must be 0 or 1 — ares supports one hidden frame",
                    )
                }
                entry!!.renderer.queueSetRunAhead(frames == 1)
            }

            (options["rewind"] as? Boolean)?.let { enabled ->
                entry!!.renderer.queueConfigureRewind(
                    enabled,
                    (options["rewindBufferSeconds"] as? Number)?.toInt() ?: 0,
                )
            }

            (options["speed"] as? Number)?.toDouble()?.let { speed ->
                entry!!.renderer.speedMultiplier = speed.coerceIn(0.25, 4.0)
            }

            // Runtime engine-option changes: cores re-read declared options
            // between frames, so a running game picks these up next tick.
            val engineOptions = (options["engineOptions"] as? Map<*, *>).orEmpty()
            for ((key, value) in engineOptions) {
                val refusal = entry!!.renderer.syncSetEngineOption(
                    key.toString(), value.toString(), staged = false,
                )
                if (refusal == null || refusal.isNotEmpty()) {
                    return BridgeResponse.error(
                        "UNSUPPORTED_OPTION",
                        refusal ?: "engine option '$key' timed out applying",
                    )
                }
            }

            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /**
     * The engine-declared option schema of the staged/active engine —
     * empty for bundled engines, whose settings are the typed config. Each
     * entry: key, choices, default, current.
     */
    class GetEngineOptions(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val list = org.json.JSONArray(EmulatorCore().engineOptionsJson())
            val options = (0 until list.length()).map { i ->
                val entry = list.getJSONObject(i)
                mapOf(
                    "key" to entry.getString("key"),
                    "choices" to entry.getJSONArray("choices").let { choices ->
                        (0 until choices.length()).map { choices.getString(it) }
                    },
                    "default" to entry.getString("default"),
                    "current" to entry.getString("current"),
                )
            }
            return BridgeResponse.success(mapOf("options" to options))
        }
    }

    /**
     * Enter/exit rewind playback (5× the capture rate, ares desktop
     * semantics). Play resumes automatically when history runs out.
     * Requires rewind capture enabled via LoadSystem config or Configure.
     */
    class ToggleRewind(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            return when (entry!!.renderer.syncToggleRewind()) {
                1 -> BridgeResponse.success(mapOf("status" to "rewinding"))
                0 -> BridgeResponse.success(mapOf("status" to "playing"))
                -1 -> BridgeResponse.error(
                    "REWIND_DISABLED",
                    "Rewind capture is off — enable it via configure(['rewind' => true]) first",
                )
                else -> operationalError(entry, "REWIND_FAILED", "Emulator not running")
            }
        }
    }

    class SetSystemOptions(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val options = paramMap(parameters, "options") ?: emptyMap()
            val toggles = CORE_TOGGLE_KEYS
                .mapNotNull { key -> (options[key] as? Boolean)?.let { key to it } }
                .toMap()
            // Enable-only gate — see LoadSystem's toggle validation.
            for ((key, value) in toggles) {
                if (value && !EmulatorCore().toggleSupported(key)) {
                    return BridgeResponse.error(
                        "UNSUPPORTED_OPTION",
                        "Backend '${EmulatorCore().backendName()}' does not support '$key'",
                    )
                }
            }
            if (toggles.isNotEmpty()) entry!!.renderer.queueCoreOptions(toggles)
            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /** Toggle fast-forward mode (4× emulation speed). */
    class FastForward(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val enabled = parameters["enabled"] as? Boolean ?: false
            entry!!.renderer.fastForward = enabled
            return BridgeResponse.success(mapOf("status" to if (enabled) "fast" else "normal"))
        }
    }

    /**
     * Merge a per-port controller remap: each `emulated => source` pair sets the
     * in-game button `emulated` to read the positional input of `source`. Names
     * are validated natively against the staged system; an empty map resets the
     * port to defaults. Validation failures are category-A programmer errors the
     * PHP wrapper raises synchronously.
     */
    class SetInputMapping(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "port is required")
            // PHP encodes an empty map as a JSON array ([]); accept it as the
            // reset-to-defaults case rather than a malformed map. (The raw value
            // is an org.json JSONArray, so read it through paramList, not a cast.)
            val emptyReset = paramList(parameters, "mappings")?.isEmpty() == true
            val mappings = paramMap(parameters, "mappings")
                ?: if (emptyReset) emptyMap()
                   else return BridgeResponse.error("INVALID_PARAMETERS", "mappings map is required")

            val pairs = mappings.entries.toList()
            val emulated = pairs.map { it.key }.toTypedArray()
            val source = pairs.map { it.value.toString() }.toTypedArray()

            val result = entry!!.renderer.setInputMapping(port, emulated, source)
            if (result.isEmpty()) {
                return BridgeResponse.success(mapOf("status" to "mapped", "count" to emulated.size))
            }
            // Native returns "CODE" or "CODE:detail" — re-raise as a bridge error.
            val code = result.substringBefore(':')
            val detail = result.substringAfter(':', "")
            val message = when (code) {
                "SYSTEM_NOT_LOADED" -> "no system is loaded"
                "INVALID_PARAMETERS" -> "invalid port or mismatched mappings"
                "UNKNOWN_BUTTON" -> "Unknown button: $detail"
                else -> result
            }
            return BridgeResponse.error(code, message)
        }
    }

    /**
     * Gate rumble forwarding: while enabled, motor state published by the
     * emulated hardware (SFC Rumble Gamepad, GB MBC5 rumble carts) drives
     * the device vibrator. The response reports whether
     * this device can rumble at all.
     */
    class SetRumble(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val enabled = parameters["enabled"] as? Boolean ?: false
            entry!!.renderer.setRumbleEnabled(enabled)
            return BridgeResponse.success(mapOf(
                "status" to if (enabled) "enabled" else "disabled",
                "hasVibrator" to entry.renderer.hasVibrator(),
            ))
        }
    }

    /**
     * Apply a librashader `.slangp` preset by path; null/"none"/"" clears it
     * (passthrough). The Vulkan filter chain is (re)built on the render thread.
     * A preset that fails to load surfaces as an EmulatorError (SHADER_FAILED),
     * so the fluent command still returns cleanly.
     */
    class SetShader(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val raw = parameters["path"] as? String
            val path = if (raw == null || raw == "none" || raw.isEmpty()) null else raw
            return when (entry!!.renderer.syncSetShader(path)) {
                true -> BridgeResponse.success(
                    mapOf("status" to if (path == null) "cleared" else "applied"))
                false -> operationalError(
                    entry, "SHADER_FAILED", "Failed to load shader preset '${path ?: ""}'")
                null -> operationalError(
                    entry, "SHADER_FAILED", "Renderer not ready or shader load timed out")
            }
        }
    }

    /**
     * Register a cheat in ares' raw format: hex "ADDR:VALUE" pairs joined with
     * '+' (e.g. "7E0010:01+7E0011:FF"). The value overrides every CPU read of
     * the address while active. Re-adding a code replaces it; cheats are
     * cleared automatically when a new ROM loads. Game Genie / GameShark
     * formats are not parsed (not supported upstream in ares).
     */
    class AddCheat(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val code = parameters["code"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "code is required")

            return when (entry!!.renderer.syncAddCheat(code)) {
                true  -> BridgeResponse.success(mapOf("status" to "added", "code" to code))
                false -> operationalError(
                    entry,
                    "INVALID_CHEAT",
                    "No valid ADDR:VALUE pairs in '$code' — expected hex pairs joined with '+'",
                )
                null  -> operationalError(entry, "CHEAT_FAILED", "Emulator not running")
            }
        }
    }

    /** Remove a cheat by its exact code string. Removing an inactive code is not an error. */
    class RemoveCheat(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val code = parameters["code"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "code is required")

            return when (entry!!.renderer.syncRemoveCheat(code)) {
                true  -> BridgeResponse.success(mapOf("status" to "removed", "code" to code))
                false -> BridgeResponse.success(mapOf("status" to "not_found", "code" to code))
                null  -> operationalError(entry, "CHEAT_FAILED", "Emulator not running")
            }
        }
    }

    /** Deactivate all cheats. Idempotent. */
    class ClearCheats(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.queueClearCheats()
            return BridgeResponse.success(mapOf("status" to "cleared"))
        }
    }

    /**
     * Register (or swap) the device on a port — controllers are explicit, never
     * auto-allocated. An absent/empty device disconnects the port.
     */
    class ConnectDevice(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "port is required")
            val device = parameters["device"] as? String ?: ""
            return when (val result = entry!!.renderer.connectDevice(port, device)) {
                "" -> BridgeResponse.success(mapOf(
                    "status" to "connected", "port" to port, "device" to device,
                    // Logical ports this device occupies (4 for a multitap) — one
                    // Controller handle each on the PHP side.
                    "ports" to entry.renderer.devicePorts(port).toList()))
                "SYSTEM_NOT_LOADED" -> BridgeResponse.error("SYSTEM_NOT_LOADED", "no system is loaded")
                "UNSUPPORTED_DEVICE" -> BridgeResponse.error("UNSUPPORTED_DEVICE", "device not supported: $device")
                else -> BridgeResponse.error("INVALID_PARAMETERS", "invalid port for this system")
            }
        }
    }

    /** Set a single button pressed on a port, resolved against its device. */
    class PressButton(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port   = (parameters["port"]   as? Number)?.toInt() ?: 1
            val button = parameters["button"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "button is required")
            return statusResponse(
                entry!!.renderer.pressButton(port, button, true),
                mapOf("status" to "pressed", "button" to button))
        }
    }

    class ReleaseButton(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port   = (parameters["port"]   as? Number)?.toInt() ?: 1
            val button = parameters["button"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "button is required")
            return statusResponse(
                entry!!.renderer.pressButton(port, button, false),
                mapOf("status" to "released", "button" to button))
        }
    }

    /** Merge multiple button states on a port. Unknown names are skipped. */
    class SetButtons(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt() ?: 1

            val state = paramMap(parameters, "state")
                ?.entries?.associate { it.key to (it.value == true) }
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "state map is required")

            for ((button, pressed) in state) {
                entry!!.renderer.pressButton(port, button, pressed)
            }
            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /** Accumulate a relative axis delta (mouse X/Y). */
    class SetAxis(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt() ?: 1
            val axis = parameters["axis"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "axis is required")
            val value = (parameters["value"] as? Number)?.toInt() ?: 0
            return statusResponse(
                entry!!.renderer.setAxis(port, axis, value),
                mapOf("status" to "ok", "axis" to axis, "value" to value))
        }
    }

    /** Aim an axis device at a normalized (0..1) screen position. */
    class AimAt(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt() ?: 1
            val x = (parameters["x"] as? Number)?.toFloat()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "x is required")
            val y = (parameters["y"] as? Number)?.toFloat()
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "y is required")
            return statusResponse(
                entry!!.renderer.aimAt(port, x, y),
                mapOf("status" to "ok", "x" to x, "y" to y))
        }
    }

    /**
     * Capture the current frame as a PNG file saved to internal storage.
     * Returns the file path. Blocks until the render thread delivers the frame (≤5 s).
     */
    class Screenshot(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val png = entry!!.renderer.syncScreenshot()
                ?: return BridgeResponse.error(
                    "SCREENSHOT_FAILED",
                    "No frame available or emulator not running",
                )

            return try {
                val screenshotsDir = File(activity.filesDir, "screenshots")
                screenshotsDir.mkdirs()
                val file = File(screenshotsDir, "screenshot_${System.currentTimeMillis()}.png")
                file.writeBytes(png)
                BridgeResponse.success(mapOf("status" to "captured", "path" to file.absolutePath))
            } catch (e: Exception) {
                Log.e(TAG, "Screenshot write failed: ${e.message}")
                BridgeResponse.error("SCREENSHOT_FAILED", e.message ?: "Failed to save screenshot")
            }
        }
    }

    /** Return the current emulator status: "stopped", "loading", "running", or "paused". */
    class GetStatus(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val name     = surface(parameters)
            val renderer = surfaces[name]?.renderer
            val status   = renderer?.currentStatus ?: "stopped"

            // Read back from the running core (which PPU is actually bound),
            // not from what was requested. Absent on cores with one renderer.
            val accuracy = when (renderer?.bootOption("Pixel Accuracy")) {
                "true" -> "accurate"
                "false" -> "performance"
                else -> null
            }

            val payload = mutableMapOf<String, Any>("status" to status)
            accuracy?.let { payload["accuracy"] = it }
            EmulatorCore().backendName().takeIf { it.isNotEmpty() }?.let {
                payload["backend"] = it
            }
            return BridgeResponse.success(payload)
        }
    }

    /** Return the region of the loaded ROM ("NTSC", "PAL", or empty). */
    class GetRegion(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val region = entry!!.renderer.getRegion()
            return BridgeResponse.success(mapOf("region" to region))
        }
    }

    /** Buttons held on a port, by name — see Controller::pressed(). */
    class GetPressedButtons(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt() ?: 1

            val pressed = entry!!.renderer.pressedButtons(port)
                .split(',')
                .filter { it.isNotEmpty() }

            return BridgeResponse.success(mapOf("port" to port, "buttons" to pressed))
        }
    }

    /**
     * Return controller ports and available button names for the loaded system.
     * Parsed from the JSON built during [EmulatorCore.loadSystem].
     */
    class GetPorts(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val json = entry!!.renderer.syncGetPortsJson()
                ?: return BridgeResponse.error("SYSTEM_NOT_LOADED", "Call LoadSystem before GetPorts")
            fun strings(obj: JSONObject, key: String) = buildList {
                val a = obj.optJSONArray(key) ?: JSONArray()
                for (j in 0 until a.length()) add(a.getString(j))
            }
            val ports = buildList {
                val arr = JSONArray(json)
                for (i in 0 until arr.length()) {
                    val obj = arr.getJSONObject(i)
                    add(mapOf(
                        "port" to obj.getInt("port"),
                        "device" to if (obj.isNull("device")) null else obj.getString("device"),
                        "buttons" to strings(obj, "buttons"),
                        "axes" to strings(obj, "axes"),
                        "supported" to strings(obj, "supported"),
                    ))
                }
            }

            return BridgeResponse.success(mapOf("ports" to ports))
        }
    }

    /**
     * Return all ares systems as rich objects. `supported` reflects whether the
     * system's core is compiled into this build's native library — only those
     * systems can be passed to [LoadSystem].
     */
    class GetSystems(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val compiled = EmulatorCore().supportedSystems().split(",").toSet()
            val backends = JSONObject(EmulatorCore().backendsJson())
            val systems = listOf(
                system("fc",  "NES / Famicom",             stable = true,  compiled, backends),
                system("sfc", "SNES / Super Famicom",      stable = true,  compiled, backends),
                system("gb",  "Game Boy",                  stable = true,  compiled, backends),
                system("gbc", "Game Boy Color",            stable = true,  compiled, backends),
                system("gba", "Game Boy Advance",          stable = true,  compiled, backends),
                system("md",  "Sega Mega Drive / Genesis", stable = true,  compiled, backends),
            )
            return BridgeResponse.success(mapOf("systems" to systems))
        }

        private fun system(
            id: String,
            name: String,
            stable: Boolean,
            compiled: Set<String>,
            backends: JSONObject,
        ): Map<String, Any> {
            val entry = backends.optJSONObject(id)
            val engines = entry?.optJSONArray("backends")
                ?.let { list -> (0 until list.length()).map { list.getString(it) } }
                ?: emptyList()
            return mapOf(
                "id" to id, "name" to name,
                "stable" to stable, "supported" to (id in compiled),
                "backends" to engines,
            )
        }
    }

    /**
     * Names of hardware controllers the OS currently reports — a handheld's
     * built-in pad, paired Bluetooth gamepads. Filtered to devices that
     * report gamepad/joystick sources; keyboards and the virtual device are
     * excluded. The on-screen overlay works whether or not this is empty.
     */
    class GetInputDevices(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val devices = mutableListOf<String>()
            for (id in InputDevice.getDeviceIds()) {
                val dev = InputDevice.getDevice(id) ?: continue
                if (dev.isVirtual) continue
                // Exact source-mask matches — a plain `and != 0` also passes
                // gpio-keys/touchscreens because SOURCE_GAMEPAD shares its
                // class bit with every button-class source.
                val gamepad = (dev.sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                val joystick = (dev.sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                if (!gamepad && !joystick) continue
                if (dev.name !in devices) devices.add(dev.name)
            }
            return BridgeResponse.success(mapOf("devices" to devices))
        }
    }
}
