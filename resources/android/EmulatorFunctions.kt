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

    private fun entry(parameters: Map<String, Any>): Pair<SurfaceEntry?, Map<String, Any>?> {
        val name = surface(parameters)
        val entry = surfaces[name]
            ?: return null to BridgeResponse.error("SURFACE_NOT_FOUND", "No surface '$name' registered")
        return entry to null
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
     * Only "sfc" (Super Famicom) is implemented in the Android layer today.
     * config keys: biosPath (String?), autoSave (Bool), speed (Float), runAhead (Int),
     *              rewind (Bool), rewindBufferSeconds (Int).
     */
    class LoadSystem(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            val system = parameters["system"] as? String ?: "sfc"
            if (system != "sfc") {
                return BridgeResponse.error(
                    "UNSUPPORTED_SYSTEM",
                    "System '$system' is not yet supported on Android — only 'sfc' is available in this build",
                )
            }

            @Suppress("UNCHECKED_CAST")
            val config = parameters["config"] as? Map<String, Any> ?: emptyMap()
            val biosPath = config["biosPath"] as? String
            val renderer = entry!!.renderer

            val boardsBml = activity.resources
                .openRawResource(R.raw.super_famicom_boards)
                .use { it.readBytes() }

            val iplRom: ByteArray? = biosPath?.let { path ->
                val file = File(path)
                if (file.exists()) file.readBytes() else null.also {
                    Log.w(TAG, "LoadSystem: biosPath not found — $path")
                }
            }

            renderer.fastForward = false
            renderer.queueSystemLoad(iplRom, boardsBml)

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
                entry.renderer.queueRomLoad(romBytes, system, path)
                Log.d(TAG, "LoadRom: queued $path (${romBytes.size} bytes)")
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

            @Suppress("UNCHECKED_CAST")
            val byteList = parameters["bytes"] as? List<*>
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

            @Suppress("UNCHECKED_CAST")
            val addresses = parameters["addresses"] as? List<*>
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

            @Suppress("UNCHECKED_CAST")
            val addresses = (parameters["addresses"] as? List<*>)
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

    /** Merge audio options (volume, balance). Stubbed — full audio control is a future phase. */
    class SetAudio(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /** Merge video options (luminance, saturation, gamma, etc.). Stubbed — requires shader support. */
    class SetVideo(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /** Merge general live options (speed, runAhead, rewind, rewindBufferSeconds). */
    class Configure(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val (entry, err) = entry(parameters)
            if (err != null) return err

            @Suppress("UNCHECKED_CAST")
            val options = parameters["options"] as? Map<String, Any> ?: emptyMap()
            val speed = (options["speed"] as? Number)?.toFloat()
            if (speed != null) {
                entry!!.renderer.fastForward = speed >= 1.5f
            }

            return BridgeResponse.success(mapOf("status" to "ok"))
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

    /** Merge controller input mappings. Stubbed — hardware mappings are hardwired in EmulatorInput. */
    class SetInputMapping(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /** Enable/disable controller rumble. Stubbed. */
    class SetRumble(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /**
     * Load a librashader-compatible shader preset. Stubbed — librashader integration
     * is a future phase. Passing null clears any active shader.
     */
    class SetShader(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /** Add a cheat code. Stubbed — ares cheat injection is a future phase. */
    class AddCheat(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val code = parameters["code"] as? String
                ?: return BridgeResponse.error("INVALID_PARAMETERS", "code is required")
            Log.d(TAG, "AddCheat: $code (stub — not applied to ares yet)")
            return BridgeResponse.success(mapOf("status" to "ok", "code" to code))
        }
    }

    /** Remove a cheat code. Stubbed. */
    class RemoveCheat(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "ok"))
    }

    /** Clear all cheat codes. Stubbed. */
    class ClearCheats(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> =
            BridgeResponse.success(mapOf("status" to "cleared"))
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
            val state = parameters["state"] as? Map<String, Boolean>
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

            val json = entry!!.renderer.getPortsJson()
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
     * Return all supported ares systems as rich objects.
     * This is a static list — no native call required.
     */
    class GetSystems(private val activity: FragmentActivity) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            val systems = listOf(
                system("fc",  "NES / Famicom",               biosRequired = false, stable = true),
                system("sfc", "SNES / Super Famicom",         biosRequired = false, stable = true),
                system("n64", "Nintendo 64",                  biosRequired = false, stable = true),
                system("gb",  "Game Boy",                     biosRequired = false, stable = true),
                system("gbc", "Game Boy Color",               biosRequired = false, stable = true),
                system("gba", "Game Boy Advance",             biosRequired = false, stable = true),
                system("sg",  "Sega SG-1000",                 biosRequired = false, stable = true),
                system("ms",  "Sega Master System",           biosRequired = false, stable = true),
                system("md",  "Sega Mega Drive / Genesis",    biosRequired = false, stable = true),
                system("pce", "PC Engine / TurboGrafx-16",    biosRequired = false, stable = true),
                system("ngp", "Neo Geo Pocket",               biosRequired = false, stable = true),
                system("ws",  "WonderSwan",                   biosRequired = false, stable = true),
                system("ps1", "PlayStation",                  biosRequired = true,  stable = false),
                system("ng",  "Neo Geo AES / MVS",            biosRequired = true,  stable = false),
                system("a26", "Atari 2600",                   biosRequired = false, stable = false),
                system("msx", "MSX / MSX2",                   biosRequired = true,  stable = false),
            )
            return BridgeResponse.success(mapOf("systems" to systems))
        }

        private fun system(id: String, name: String, biosRequired: Boolean, stable: Boolean) =
            mapOf("id" to id, "name" to name, "biosRequired" to biosRequired, "stable" to stable)
    }
}
