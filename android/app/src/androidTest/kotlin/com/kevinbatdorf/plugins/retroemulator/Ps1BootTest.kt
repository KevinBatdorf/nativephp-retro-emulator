package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * PlayStation disc boot — the media path end to end: BIOS via loadSystem's
 * biosPath, a .cue/.bin homebrew disc via loadRomPath, frames rendered, and a
 * live disc swap. The BIOS is the developer's own dump (copyrighted — never
 * in the repo); push the media first:
 *   adb push <ps1 bios>                       /data/local/tmp/ps1-bios.rom
 *   adb push "<PadTest 1.1>/PadTest.cue"      /data/local/tmp/ps1-a.cue
 *   adb push "<PadTest 1.1>/PadTest.bin"      /data/local/tmp/PadTest.bin
 *   adb push "<240p>/240pTestSuitePS1.cue"    /data/local/tmp/ps1-b.cue
 *   adb push "<240p>/240pTestSuitePS1.bin"    /data/local/tmp/240pTestSuitePS1.bin
 * (The .bin filenames must match the cues' FILE lines — they resolve relative
 * to the cue.)
 *
 * Skips cleanly when the media isn't present, so CI without firmware stays
 * green.
 */
@RunWith(AndroidJUnit4::class)
class Ps1BootTest {

    private val bios = "/data/local/tmp/ps1-bios.rom"
    private val discA = "/data/local/tmp/ps1-a.cue"
    private val discB = "/data/local/tmp/ps1-b.cue"

    private fun mediaPresent(vararg paths: String) = paths.all { File(it).exists() }

    @Test
    fun discSystemReportsMediaPathAndGatesOnBios() {
        if (!mediaPresent(discA)) {
            android.util.Log.w("Ps1BootTest", "Skipping: no disc at $discA")
            return
        }
        AresCore().run {
            try {
                assert(init()) { "init failed" }
                assert(loadSystem("ps1")) { "loadSystem(ps1) failed" }
                assert(usesMediaPath()) { "ps1 must load media by path" }
                // No BIOS → refuse, pre-teardown.
                assert(loadRomPath(discA) == AresCore.LOAD_BIOS_REQUIRED) {
                    "expected LOAD_BIOS_REQUIRED without firmware"
                }
            } finally { destroy() }
        }
    }

    @Test
    fun discBootsRendersAndSwaps() {
        if (!mediaPresent(bios, discA, discB)) {
            android.util.Log.w("Ps1BootTest", "Skipping: bios/discs absent")
            return
        }
        val core = AresCore()
        try {
            assert(core.init()) { "init failed" }
            assert(core.loadSystem("ps1", bios)) { "loadSystem(ps1)+bios failed" }
            assert(core.loadRomPath(discA) == AresCore.LOAD_OK) { "loadRomPath(discA) failed" }
            // Skip the BIOS boot animation so the render probe lands on the
            // game, not the Sony logo. Settings live on the booted core; the
            // BIOS reads Fast Boot a few frames in, so post-boot is in time.
            core.setCoreBoolean("fastBoot", true)

            repeat(600) { core.tick() }
            // The BIOS writes the exception vectors at phys 0 within its
            // first frames — untouched RAM means the CPU never executed.
            val ram = core.readMemory(0x0000, 64)
            assert(ram != null && ram.any { it != 0.toByte() }) {
                "work RAM untouched after 600 ticks — BIOS not executing"
            }
            // The video callback stamps the frame dimensions even headlessly
            // (screenshotRGBA needs the Vulkan renderer, absent here); the
            // pixel eyes-on runs through the demo app on the Thor screen.
            assert(core.getFrameWidth() > 0 && core.getFrameHeight() > 0) {
                "video callback never fired after 600 ticks"
            }

            // Live swap: tray opens, disc B stages, drive reconnects at 180
            // frames — the game must keep running through the empty-drive gap.
            assert(core.swapDisc(discB)) { "swapDisc(discB) rejected" }
            repeat(400) { core.tick() }
            assert(core.getPortsJson().startsWith("[")) { "core died across the disc swap" }
        } finally {
            core.destroy()
        }
        android.util.Log.i("Ps1BootTest", "PS1 boot + render + swap OK")
    }
}
