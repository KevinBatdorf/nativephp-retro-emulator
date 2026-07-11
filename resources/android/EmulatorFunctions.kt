package com.kevinbatdorf.plugins.retroemulator

import android.os.Handler
import android.os.Looper
import android.util.Log
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
 *  - GL-critical ares operations are posted to the GL thread via [EmulatorRenderer].
 *  - NativePHP events are always dispatched on the main thread.
 */
object EmulatorFunctions {

    private const val TAG = "EmulatorFunctions"

    // -------------------------------------------------------------------------
    // Surface registry
    // -------------------------------------------------------------------------

    private data class SurfaceEntry(
        val renderer: EmulatorRenderer,
        val activity: FragmentActivity,
    )

    private val surfaces = java.util.concurrent.ConcurrentHashMap<String, SurfaceEntry>()

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

    /** Remove a surface from the registry (called when the component is torn down). */
    @JvmStatic
    fun unregisterSurface(name: String) {
        surfaces.remove(name)?.renderer?.eventListener = null
        Log.d(TAG, "Surface unregistered: $name")
    }

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    private fun surface(parameters: Map<String, Any>): String =
        parameters["surface"] as? String ?: "main"

    private fun paramList(parameters: Map<String, Any>, key: String): List<Any?>? =
        BridgeParams.list(parameters, key)

    private fun paramMap(parameters: Map<String, Any>, key: String): Map<String, Any>? =
        BridgeParams.map(parameters, key)

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

    // -------------------------------------------------------------------------
    // Event forwarder — translates EmulatorEventListener callbacks to NativePHP events
    // -------------------------------------------------------------------------

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

    // =========================================================================
    // Bridge functions
    // =========================================================================

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
     * Initialise the ares core for a system and queue a system load.
     * Supported systems are the ones compiled into the native library —
     * reported by [GetSystems] with `supported: true`. System firmware
     * (SFC ipl.rom + boards.bml, GB boot ROM, MD TMSS) is embedded; no
     * biosPath is needed for these systems.
     * config keys: biosPath (String?), autoSave (Bool), speed (Float), runAhead (Int),
     *              rewind (Bool), rewindBufferSeconds (Int).
     */
    class LoadSystem(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val system = parameters["system"] as? String ?: "sfc"
            val supported = AresCore().supportedSystems().split(",")
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
            renderer.queueSetRunAhead(runAhead == 1)
            renderer.queueConfigureRewind(
                config["rewind"] as? Boolean ?: false,
                (config["rewindBufferSeconds"] as? Number)?.toInt() ?: 0,
            )
            (config["speed"] as? Number)?.toDouble()?.let { speed ->
                renderer.speedMultiplier = speed.coerceIn(0.25, 4.0)
            }
            renderer.queueSystemLoad(system)

            Log.d(TAG, "LoadSystem: queued system=$system")
            return BridgeResponse.success(mapOf("status" to "loading", "system" to system))
        }
    }

    /**
     * Load a ROM from [path] and start emulation. Fire-and-forget — PHP receives
     * `{"status":"loading"}` immediately; [EmulatorStarted] fires on the first frame.
     */
    class LoadRom(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val path = parameters["path"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "path is required")

            val file = File(path)
            if (!file.exists()) {
                return BridgeResponse.error("ROM_NOT_FOUND", "ROM not found: $path")
            }

            return try {
                val romBytes = file.readBytes()
                val system   = entry!!.renderer.loadedSystem.ifEmpty { "sfc" }
                // Battery saves live in app storage, keyed by surface + ROM
                // basename: "<filesDir>/saves/<surface>/<rom>.save.ram", etc.
                val saveDir = File(activity.filesDir, "saves/${surface(parameters)}")
                saveDir.mkdirs()
                val savePrefix = File(saveDir, file.nameWithoutExtension).absolutePath
                entry.renderer.queueRomLoad(romBytes, system, path, savePrefix)
                Log.d(TAG, "LoadRom: queued $path (${romBytes.size} bytes, saves=$savePrefix)")
                BridgeResponse.success(mapOf("status" to "loading", "path" to path))
            } catch (e: Exception) {
                Log.e(TAG, "LoadRom failed: ${e.message}")
                BridgeResponse.error("READ_FAILED", e.message ?: "Failed to read ROM")
            }
        }
    }

    /** Pause emulation. */
    class Pause(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.pauseEmulation()
            return BridgeResponse.success(mapOf("status" to "paused"))
        }
    }

    /** Resume emulation. */
    class Resume(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.resumeEmulation()
            return BridgeResponse.success(mapOf("status" to "running"))
        }
    }

    /** Stop emulation and tear down the emulator core. */
    class Stop(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            entry!!.renderer.stopEmulation()
            return BridgeResponse.success(mapOf("status" to "stopped"))
        }
    }

    /** Save state to slot. */
    class StateSave(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val slot = parameters["slot"] ?: 1
            val stateDir = File(activity.filesDir, "states/${surface(parameters)}")
            stateDir.mkdirs()
            val path = File(stateDir, "$slot.state").absolutePath

            val ok = entry!!.renderer.syncStateSave(path)
            return if (ok) {
                BridgeResponse.success(mapOf("status" to "saved", "slot" to slot, "path" to path))
            } else {
                BridgeResponse.error("SAVE_FAILED", "State save failed for slot $slot")
            }
        }
    }

    /** Load state from slot. */
    class StateLoad(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val slot = parameters["slot"] ?: 1
            val statePath = File(activity.filesDir, "states/${surface(parameters)}/$slot.state")

            if (!statePath.exists()) {
                return BridgeResponse.error("SLOT_EMPTY", "No state in slot $slot")
            }

            val ok = entry!!.renderer.syncStateLoad(statePath.absolutePath)
            return if (ok) {
                BridgeResponse.success(mapOf("status" to "loaded", "slot" to slot))
            } else {
                BridgeResponse.error("LOAD_FAILED", "State load failed for slot $slot")
            }
        }
    }

    /** Undo most recent state save (removes the undo-slot file). */
    class UndoStateSave(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val undoFile = File(activity.filesDir, "states/${surface(parameters)}/undo_save.state")
            val deleted = undoFile.delete()
            return BridgeResponse.success(mapOf("status" to if (deleted) "undone" else "nothing_to_undo"))
        }
    }

    /** Undo most recent state load (re-applies from the undo-load backup slot). */
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
                BridgeResponse.success(mapOf("status" to "undone"))
            } else {
                BridgeResponse.error("UNDO_FAILED", "Undo state load failed")
            }
        }
    }

    /**
     * Synchronous WRAM read. Returns bytes immediately via the bridge response.
     * Blocks the bridge thread until the GL thread services the request (≤2 s).
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
            entry!!.renderer.queueWriteMemory(address, bytes)

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

    /** Remove specific address watches. */
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

    /** Clear all memory watches. */
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
            val volume  = ((options["volume"]  as? Number)?.toFloat() ?: 100f) / 100f
            val balance = ((options["balance"] as? Number)?.toFloat() ?: 0f) / 100f
            entry!!.renderer.setAudioOptions(volume, balance)
            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /**
     * Merge video options — luminance/saturation 0–100, gamma 1.0–2.0,
     * colorBleed/interframeBlending booleans, applied on the ares screen node.
     * Options ares has no post-processing hook for (colorEmulation,
     * deepBlackBoost, overscan, pixelAccuracy) are reported back as ignored.
     */
    class SetVideo(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            @Suppress("UNCHECKED_CAST")
            val options = paramMap(parameters, "options") ?: emptyMap()
            entry!!.renderer.queueVideoOptions(
                luminance  = ((options["luminance"]  as? Number)?.toFloat() ?: 100f) / 100f,
                saturation = ((options["saturation"] as? Number)?.toFloat() ?: 100f) / 100f,
                gamma      = (options["gamma"] as? Number)?.toFloat() ?: 1.0f,
                colorBleed = options["colorBleed"] as? Boolean ?: false,
                interframeBlending = options["interframeBlending"] as? Boolean ?: false,
            )
            val ignored = options.keys.filter {
                it in setOf("colorEmulation", "deepBlackBoost", "overscan", "pixelAccuracy")
            }
            return BridgeResponse.success(
                if (ignored.isEmpty()) mapOf("status" to "ok")
                else mapOf("status" to "ok", "ignored" to ignored)
            )
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

            return BridgeResponse.success(mapOf("status" to "ok"))
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
                else -> BridgeResponse.error("REWIND_FAILED", "Emulator not running")
            }
        }
    }

    /** Merge system-specific options (e.g. expansionPak for N64). Stubbed. */
    class SetSystemOptions(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
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

    /** Custom controller mappings are NOT implemented in v1 — hardware mappings are hardwired in EmulatorInput. */
    class SetInputMapping(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.error("NOT_IMPLEMENTED", "custom input mappings are not supported in v1")
    }

    /**
     * Gate rumble forwarding: while enabled, motor state published by the
     * emulated hardware (SFC Rumble Gamepad, GB MBC5 rumble carts, N64
     * Rumble Pak) drives the device vibrator. The response reports whether
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
     * Shaders (librashader) are NOT implemented in v1. Passing null/"none"
     * (a clear) succeeds — there is never an active shader to remove.
     */
    class SetShader(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val path = parameters["path"] as? String
            if (path == null || path == "none") {
                return BridgeResponse.success(mapOf("status" to "cleared"))
            }
            return BridgeResponse.error("NOT_IMPLEMENTED", "shaders are not supported in v1")
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
                false -> BridgeResponse.error(
                    "INVALID_CHEAT",
                    "No valid ADDR:VALUE pairs in '$code' — expected hex pairs joined with '+'",
                )
                null  -> BridgeResponse.error("CHEAT_FAILED", "Emulator not running")
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
                null  -> BridgeResponse.error("CHEAT_FAILED", "Emulator not running")
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

    /** Set a single button to pressed in the software input state map. */
    class PressButton(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port   = (parameters["port"]   as? Number)?.toInt() ?: 1
            val button = parameters["button"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "button is required")
            val bit = EmulatorInput.buttonNameToBit(button)
                ?: return BridgeResponse.error("UNKNOWN_BUTTON", "Unknown button: $button")
            entry!!.renderer.input.pressSoftwareButton(port, bit)
            return BridgeResponse.success(mapOf("status" to "pressed", "button" to button))
        }
    }

    /** Release a single button in the software input state map. */
    class ReleaseButton(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port   = (parameters["port"]   as? Number)?.toInt() ?: 1
            val button = parameters["button"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "button is required")
            val bit = EmulatorInput.buttonNameToBit(button)
                ?: return BridgeResponse.error("UNKNOWN_BUTTON", "Unknown button: $button")
            entry!!.renderer.input.releaseSoftwareButton(port, bit)
            return BridgeResponse.success(mapOf("status" to "released", "button" to button))
        }
    }

    /** Atomically merge multiple button states into the software input state map. */
    class SetButtons(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err
            val port = (parameters["port"] as? Number)?.toInt() ?: 1

            @Suppress("UNCHECKED_CAST")
            val state = paramMap(parameters, "state")
                ?.entries?.associate { it.key to (it.value == true) }
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "state map is required")

            for ((button, pressed) in state) {
                val bit = EmulatorInput.buttonNameToBit(button) ?: continue
                if (pressed) {
                    entry!!.renderer.input.pressSoftwareButton(port, bit)
                } else {
                    entry!!.renderer.input.releaseSoftwareButton(port, bit)
                }
            }

            return BridgeResponse.success(mapOf("status" to "ok"))
        }
    }

    /**
     * Capture the current frame as a PNG file saved to internal storage.
     * Returns the file path. Blocks until the GL thread delivers the frame (≤5 s).
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
                BridgeResponse.error("WRITE_FAILED", e.message ?: "Failed to save screenshot")
            }
        }
    }

    /** Return the current emulator status: "stopped", "loading", "running", or "paused". */
    class GetStatus(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val name   = surface(parameters)
            val status = surfaces[name]?.renderer?.currentStatus ?: "stopped"
            return BridgeResponse.success(mapOf("status" to status))
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

    /**
     * Return controller ports and available button names for the loaded system.
     * Parsed from the JSON built during [AresCore.loadSystem].
     */
    class GetPorts(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val json = entry!!.renderer.syncGetPortsJson()
                ?: return BridgeResponse.error("SYSTEM_NOT_LOADED", "Call LoadSystem before GetPorts")
            val ports = buildList {
                val arr = JSONArray(json)
                for (i in 0 until arr.length()) {
                    val obj = arr.getJSONObject(i)
                    val buttons = buildList {
                        val btnArr = obj.getJSONArray("buttons")
                        for (j in 0 until btnArr.length()) add(btnArr.getString(j))
                    }
                    add(mapOf("port" to obj.getInt("port"), "buttons" to buttons))
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
            val compiled = AresCore().supportedSystems().split(",").toSet()
            val systems = listOf(
                system("fc",  "NES / Famicom",               biosRequired = false, stable = true,  compiled),
                system("sfc", "SNES / Super Famicom",         biosRequired = false, stable = true,  compiled),
                system("n64", "Nintendo 64",                  biosRequired = false, stable = true,  compiled),
                system("gb",  "Game Boy",                     biosRequired = false, stable = true,  compiled),
                system("gbc", "Game Boy Color",               biosRequired = false, stable = true,  compiled),
                system("gba", "Game Boy Advance",             biosRequired = false, stable = true,  compiled),
                system("sg",  "Sega SG-1000",                 biosRequired = false, stable = true,  compiled),
                system("ms",  "Sega Master System",           biosRequired = false, stable = true,  compiled),
                system("md",  "Sega Mega Drive / Genesis",    biosRequired = false, stable = true,  compiled),
                system("pce", "PC Engine / TurboGrafx-16",    biosRequired = false, stable = true,  compiled),
                system("ngp", "Neo Geo Pocket",               biosRequired = false, stable = true,  compiled),
                system("ws",  "WonderSwan",                   biosRequired = false, stable = true,  compiled),
                system("ps1", "PlayStation",                  biosRequired = true,  stable = false, compiled),
                system("ng",  "Neo Geo AES / MVS",            biosRequired = true,  stable = false, compiled),
                system("a26", "Atari 2600",                   biosRequired = false, stable = false, compiled),
                system("msx", "MSX / MSX2",                   biosRequired = true,  stable = false, compiled),
            )
            return BridgeResponse.success(mapOf("systems" to systems))
        }

        private fun system(id: String, name: String, biosRequired: Boolean, stable: Boolean, compiled: Set<String>) =
            mapOf(
                "id" to id, "name" to name, "biosRequired" to biosRequired,
                "stable" to stable, "supported" to (id in compiled),
            )
    }
}
