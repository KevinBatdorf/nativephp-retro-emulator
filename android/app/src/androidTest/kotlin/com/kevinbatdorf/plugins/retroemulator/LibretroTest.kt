package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Bring-your-own libretro cores through the seam: an unregistered backend
 * name is offered to the loader, which adopts the core by probing its .so,
 * boots ROMs from memory, and answers the same host API as bundled engines.
 *
 * Cores are never committed. They ride the TEST APK (SELinux allows dlopen
 * only from installed APK lib dirs, not /data/local/tmp) — stage them into
 * the gitignored androidTest/jniLibs before running:
 *   mkdir -p android/app/src/androidTest/jniLibs/arm64-v8a
 *   cp vendor-engines/cores/snes9x_libretro_android.so \
 *      android/app/src/androidTest/jniLibs/arm64-v8a/libsnes9x_libretro_android.so
 * Tests skip when a core (or its test ROM) is absent.
 */
@RunWith(AndroidJUnit4::class)
class LibretroTest {

    private fun corePathOrSkip(name: String): String? {
        val info = InstrumentationRegistry.getInstrumentation().context.applicationInfo
        val lib = "lib${name}_libretro_android.so"
        val extracted = File("${info.nativeLibraryDir}/$lib")
        if (extracted.exists()) return extracted.absolutePath
        // extractNativeLibs=false keeps libs zipped in the APK; bionic
        // dlopens them via the "apk!/lib/..." path form.
        java.util.zip.ZipFile(info.sourceDir).use { zip ->
            if (zip.getEntry("lib/arm64-v8a/$lib") != null) {
                return "${info.sourceDir}!/lib/arm64-v8a/$lib"
            }
        }
        android.util.Log.w("LibretroTest", "Skipping: $lib not packaged in the test APK")
        return null
    }

    private fun romOrSkip(path: String, label: String): ByteArray? {
        val file = File(path)
        if (!file.exists()) {
            android.util.Log.w("LibretroTest", "Skipping: no $label at $path")
            return null
        }
        return file.readBytes()
    }

    @Test
    fun snes9xAdoptsBootsAndRoundTrips() {
        val corePath = corePathOrSkip("snes9x") ?: return
        val rom = romOrSkip("/data/local/tmp/test-sfc.rom", "sfc ROM") ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc", backend = corePath)) {
                "snes9x adoption by absolute path failed"
            }
            // Identity is the core, path-independent — status responses and
            // save-state tags say what actually serves.
            assert(core.backendName() == "snes9x") {
                "adopted identity must be 'snes9x' — got '${core.backendName()}'"
            }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK) { "loadRom failed on snes9x" }
            repeat(120) { core.tick() }

            assert(core.getFrameWidth() == 256) {
                "snes9x frame width must be 256 — got ${core.getFrameWidth()}"
            }
            assert(core.getFrameHeight() == 224 || core.getFrameHeight() == 239) {
                "snes9x frame height must be 224/239 — got ${core.getFrameHeight()}"
            }

            // WRAM window at the catalog base, like every other engine.
            val bytes = core.readMemory(0x7E0000, 64)
            assert(bytes != null && bytes.size == 64) { "readMemory failed on snes9x" }

            val state = File.createTempFile("snes9x", ".state")
            try {
                assert(core.stateSave(state.absolutePath)) { "stateSave failed" }
                repeat(30) { core.tick() }
                assert(core.stateLoad(state.absolutePath)) { "stateLoad failed" }
            } finally {
                state.delete()
            }

            // The option door stays loud: nothing from the wrapper toggle
            // set exists on a BYO core.
            assert(!core.toggleSupported("interframeBlending")) {
                "BYO cores must not claim wrapper toggles"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun swapsCoresAcrossSystemsWhileRunning() {
        val sfcCore = corePathOrSkip("snes9x") ?: return
        val fcCore = corePathOrSkip("fceumm") ?: return
        val sfcRom = romOrSkip("/data/local/tmp/test-sfc.rom", "sfc ROM") ?: return
        val fcRom = romOrSkip("/data/local/tmp/test-fc.rom", "fc ROM") ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc", backend = sfcCore))
            assert(core.loadRom(sfcRom) == EmulatorCore.LOAD_OK)
            repeat(30) { core.tick() }

            // Adopting a different core while a game runs stages it; the
            // swap lands at the next boot, never under the running game.
            assert(core.loadSystem("fc", backend = fcCore)) {
                "fceumm adoption while snes9x runs must stage"
            }
            // fceumm reads media by path (need_fullpath): a save prefix must
            // be in play, exactly as the bridge always provides one.
            val prefix = InstrumentationRegistry.getInstrumentation()
                .targetContext.cacheDir.absolutePath + "/libretro-swap"
            assert(core.loadRom(fcRom, savePrefix = prefix) == EmulatorCore.LOAD_OK) {
                "loadRom failed after core swap"
            }
            repeat(30) { core.tick() }
            assert(core.backendName() == "fceumm") {
                "post-swap identity must be 'fceumm' — got '${core.backendName()}'"
            }
            assert(core.getFrameWidth() == 256) { "fceumm frame width must be 256" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun engineOptionFlipsARealSnes9xOption() {
        val corePath = corePathOrSkip("snes9x") ?: return
        val rom = romOrSkip("/data/local/tmp/test-sfc.rom", "sfc ROM") ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc", backend = corePath))

            // The schema exists at stage time (retro_init runs at adoption);
            // enumerate rather than trust remembered key names.
            val schema = org.json.JSONArray(core.engineOptionsJson())
            assert(schema.length() > 0) { "snes9x must declare options at stage time" }
            val region = (0 until schema.length())
                .map { schema.getJSONObject(it) }
                .firstOrNull { it.getString("key") == "snes9x_region" }
            if (region == null) {
                android.util.Log.w("LibretroTest", "Skipping flip: no snes9x_region option")
                return
            }
            val choices = region.getJSONArray("choices").let { list ->
                (0 until list.length()).map { list.getString(it) }
            }
            assert("pal" in choices) { "snes9x_region must declare 'pal' — got $choices" }

            assert(core.setEngineOption("snes9x_region", "pal", staged = true) == "") {
                "declared key + declared value must apply"
            }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK)
            repeat(30) { core.tick() }
            val palHint = core.refreshRateHint()
            assert(palHint in 49.0..51.0) {
                "forcing snes9x_region=pal must produce a ~50 Hz core — got $palHint"
            }

            // The introspection reflects the applied value.
            val current = org.json.JSONArray(core.engineOptionsJson()).let { list ->
                (0 until list.length()).map { list.getJSONObject(it) }
                    .first { it.getString("key") == "snes9x_region" }
                    .getString("current")
            }
            assert(current == "pal") { "current must reflect the applied value — got $current" }

            // Runtime path: flip back while running; the core re-reads
            // between frames, and the next boot returns to ~60 Hz NTSC.
            assert(core.setEngineOption("snes9x_region", "ntsc", staged = false) == "")
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK)
            repeat(30) { core.tick() }
            val ntscHint = core.refreshRateHint()
            assert(ntscHint in 59.0..61.0) {
                "snes9x_region=ntsc must return the core to ~60 Hz — got $ntscHint"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun typoKeyAndIllegalValueErrorLoudly() {
        val corePath = corePathOrSkip("snes9x") ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc", backend = corePath))

            val typo = core.setEngineOption("snes9x_definitely_not_real", "enabled", staged = true)
            assert(typo.isNotEmpty() && "not an option" in typo && "snes9x" in typo) {
                "a typo'd key must error naming the core — got '$typo'"
            }

            val schema = org.json.JSONArray(core.engineOptionsJson())
            val firstKey = schema.getJSONObject(0).getString("key")
            val illegal = core.setEngineOption(firstKey, "not_a_declared_value_zzz", staged = true)
            assert(illegal.isNotEmpty() && "not a value" in illegal) {
                "an undeclared value must error echoing the legal list — got '$illegal'"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun verifiedCoreTableBootsEveryListedCore() {
        // The README's verified table: every listed core must adopt, boot a
        // real ROM, render, and round-trip a state — or it doesn't get
        // listed. Perf/accuracy pairs per system with no bundled fast core.
        val table = listOf(
            Triple("mesen", "fc", "/data/local/tmp/test-fc.rom"),
            Triple("bsnes", "sfc", "/data/local/tmp/test-sfc.rom"),
            Triple("picodrive", "md", "/data/local/tmp/test-md.rom"),
            Triple("genesis_plus_gx", "md", "/data/local/tmp/test-md.rom"),
        )
        val prefix = InstrumentationRegistry.getInstrumentation()
            .targetContext.cacheDir.absolutePath + "/verified-core"
        for ((name, system, romPath) in table) {
            val corePath = corePathOrSkip(name) ?: continue
            val rom = romOrSkip(romPath, "$system ROM") ?: continue
            val core = EmulatorCore()
            try {
                assert(core.init())
                assert(core.loadSystem(system, backend = corePath)) { "$name failed to stage" }
                assert(core.backendName() == name) { "$name identity drifted" }
                assert(core.loadRom(rom, savePrefix = "$prefix-$name") == EmulatorCore.LOAD_OK) {
                    "$name rejected the $system ROM"
                }
                repeat(90) { core.tick() }
                assert(core.getFrameWidth() > 0 && core.getFrameHeight() > 0) {
                    "$name produced no frame"
                }
                val state = File.createTempFile(name, ".state")
                try {
                    assert(core.stateSave(state.absolutePath)) { "$name stateSave failed" }
                    repeat(15) { core.tick() }
                    assert(core.stateLoad(state.absolutePath)) { "$name stateLoad failed" }
                } finally {
                    state.delete()
                }
            } finally {
                core.destroy()
            }
        }
    }

    @Test
    fun bundledEnginesRefuseEngineOptions() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb"))   // sameboy by default
            val refusal = core.setEngineOption("some_key", "some_value", staged = true)
            assert(refusal.isNotEmpty() && "typed config" in refusal) {
                "bundled engines must refuse engine options — got '$refusal'"
            }
            assert(core.engineOptionsJson() == "[]") {
                "bundled engines declare no engine options"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun missingCoreFailsLoudly() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(!core.loadSystem("sfc", backend = "no_such_core_xyz")) {
                "a name no engine answers and no core satisfies must fail to stage"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun bundledEngineIsNeverAdoptionProbed() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            // sameboy is a registered engine that doesn't claim sfc: the
            // request must fail as unsupported, not fall through to a
            // dlopen probe for "libsameboy_libretro_android.so".
            assert(!core.loadSystem("sfc", backend = "sameboy")) {
                "a bundled engine that doesn't claim the system must fail"
            }
        } finally {
            core.destroy()
        }
    }
}
