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
