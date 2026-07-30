package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Backend selection through the seam: an unnamed boot runs the built-in
 * engine (ares) — every other engine is an explicit choice, never a silent
 * substitution — and the alternates boot the same ROM and answer the same
 * API when named.
 *
 * Needs /data/local/tmp/test-gb.rom (see Phase11MultiSystemTest's push list).
 */
@RunWith(AndroidJUnit4::class)
class BackendSelectionTest {

    private val romPath = "/data/local/tmp/test-gb.rom"

    private fun romOrSkip(): ByteArray? {
        val file = File(romPath)
        if (!file.exists()) {
            android.util.Log.w("BackendSelectionTest", "Skipping: no ROM at $romPath")
            return null
        }
        return file.readBytes()
    }

    @Test
    fun gbUnnamedBootsTheBuiltInEngine() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb")) { "loadSystem failed" }
            assert(core.backendName() == "ares") {
                "an unnamed boot runs the built-in engine — got '${core.backendName()}'"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun explicitSameBoyServesGb() {
        val rom = romOrSkip() ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = "sameboy")) { "loadSystem failed" }
            assert(core.backendName() == "sameboy") {
                "explicit sameboy must be honored — got '${core.backendName()}'"
            }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK) { "loadRom failed on sameboy" }
            repeat(120) { core.tick() }

            assert(core.getFrameWidth() == 160 && core.getFrameHeight() == 144) {
                "sameboy frame must be 160x144 — got ${core.getFrameWidth()}x${core.getFrameHeight()}"
            }
            val bytes = core.readMemory(0xC000, 64)
            assert(bytes != null && bytes.size == 64) { "readMemory failed on sameboy" }

            // Save states round-trip on the fast core too.
            val state = File.createTempFile("sameboy", ".state")
            try {
                assert(core.stateSave(state.absolutePath)) { "stateSave failed" }
                repeat(30) { core.tick() }
                assert(core.stateLoad(state.absolutePath)) { "stateLoad failed" }
            } finally {
                state.delete()
            }

            // The option contract is explicit per engine: sameboy declares
            // colorEmulation and nothing else from the toggle set.
            assert(core.toggleSupported("colorEmulation")) {
                "sameboy must support colorEmulation"
            }
            assert(!core.toggleSupported("interframeBlending")) {
                "sameboy must NOT claim interframeBlending — loud, not a silent no-op"
            }
            core.setCoreBoolean("colorEmulation", false)
            assert(core.getCoreBoolean("colorEmulation") == 0) { "toggle must apply" }
            core.setCoreBoolean("colorEmulation", true)
            assert(core.getCoreBoolean("colorEmulation") == 1) { "toggle must re-apply" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun explicitAresStillServesGb() {
        val rom = romOrSkip() ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = "ares")) { "loadSystem(ares) failed" }
            assert(core.backendName() == "ares") {
                "explicit ares must be honored — got '${core.backendName()}'"
            }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK) { "loadRom failed on ares" }
            repeat(120) { core.tick() }
            assert(core.getFrameWidth() == 160 && core.getFrameHeight() == 144) {
                "ares gb frame must be 160x144"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun unknownBackendFailsToStage() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(!core.loadSystem("gb", backend = "quicknes")) {
                "a backend that does not serve gb must fail to stage, not fall back"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun gbaUnnamedBootsTheBuiltInEngine() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gba")) { "loadSystem failed" }
            assert(core.backendName() == "ares") {
                "an unnamed boot runs the built-in engine — got '${core.backendName()}'"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun explicitMgbaServesGba() {
        val romFile = File("/data/local/tmp/test-gba.rom")
        if (!romFile.exists()) {
            android.util.Log.w("BackendSelectionTest", "Skipping gba: no ROM")
            return
        }
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gba", backend = "mgba")) { "loadSystem failed" }
            assert(core.backendName() == "mgba") {
                "explicit mgba must be honored — got '${core.backendName()}'"
            }
            assert(core.loadRom(romFile.readBytes()) == EmulatorCore.LOAD_OK) {
                "loadRom failed on mgba"
            }
            repeat(120) { core.tick() }

            assert(core.getFrameWidth() == 240 && core.getFrameHeight() == 160) {
                "mgba frame must be 240x160 — got ${core.getFrameWidth()}x${core.getFrameHeight()}"
            }
            val bytes = core.readMemory(0x02000000, 64)
            assert(bytes != null && bytes.size == 64) { "EWRAM read failed on mgba" }

            val state = File.createTempFile("mgba", ".state")
            try {
                assert(core.stateSave(state.absolutePath)) { "stateSave failed" }
                repeat(30) { core.tick() }
                assert(core.stateLoad(state.absolutePath)) { "stateLoad failed" }
            } finally {
                state.delete()
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun explicitAresStillServesGba() {
        val romFile = File("/data/local/tmp/test-gba.rom")
        if (!romFile.exists()) {
            android.util.Log.w("BackendSelectionTest", "Skipping gba/ares: no ROM")
            return
        }
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gba", backend = "ares")) { "loadSystem(ares) failed" }
            assert(core.backendName() == "ares")
            assert(core.loadRom(romFile.readBytes()) == EmulatorCore.LOAD_OK)
            repeat(120) { core.tick() }
            assert(core.getFrameWidth() == 240 && core.getFrameHeight() == 160)
        } finally {
            core.destroy()
        }
    }

    @Test
    fun backendsJsonListsClaimantsAndSuggestsNothing() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            val json = org.json.JSONObject(core.backendsJson())

            // Registered engines, claimant or not — the loader claims no
            // system, so this is the only proof its module loaded.
            val engines = json.getJSONArray("engines").let { list ->
                (0 until list.length()).map { list.getString(it) }
            }
            assert("libretro" in engines) {
                "the BYO loader must be registered — got $engines"
            }
            assert(listOf("ares", "sameboy", "mgba").all { it in engines }) {
                "all bundled engines must be registered — got $engines"
            }

            val gb = json.getJSONObject("gb")
            val gbBackends = gb.getJSONArray("backends").let { list ->
                (0 until list.length()).map { list.getString(it) }
            }
            assert("ares" in gbBackends && "sameboy" in gbBackends) {
                "gb must be claimed by both engines — got $gbBackends"
            }
            // Availability only — no crowned pick anywhere in the discovery
            // surface; unnamed boots run the built-in engine.
            assert(!gb.has("default")) { "backendsJson must not suggest a default" }
            assert(!json.getJSONObject("gba").has("default"))
            assert(!json.getJSONObject("sfc").has("default"))
        } finally {
            core.destroy()
        }
    }
}
