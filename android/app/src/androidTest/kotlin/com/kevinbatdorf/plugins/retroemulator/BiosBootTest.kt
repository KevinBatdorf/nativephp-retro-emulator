package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * GBA boots with no dev-supplied firmware — the embedded open BIOS
 * (Cult-of-GBA, MIT) is baked into the native library — and a dev-supplied
 * biosPath (a real dump, for accuracy) still overrides it. Push a GBA ROM:
 *   adb push tests/roms/gba-shades.gba /data/local/tmp/test-gba.rom
 * and, to exercise the override, a real BIOS:
 *   adb push <gba bios> /data/local/tmp/gba-bios.rom
 *
 * Skips cleanly when the ROM isn't present, so CI without media stays green.
 */
@RunWith(AndroidJUnit4::class)
class BiosBootTest {

    @Test
    fun gbaBootsOnEmbeddedBiosAndHonorsOverride() {
        val rom = File("/data/local/tmp/test-gba.rom")
        if (!rom.exists()) {
            android.util.Log.w("BiosBootTest", "Skipping gba: ROM absent (${rom.path})")
            return
        }
        val romBytes = rom.readBytes()

        // No biosPath → the embedded open BIOS boots and runs.
        EmulatorCore().run {
            try {
                assert(init()) { "gba: init failed" }
                assert(loadSystem("gba")) { "gba: loadSystem failed" }
                assert(loadRom(romBytes) == EmulatorCore.LOAD_OK) {
                    "gba: embedded BIOS should boot with no biosPath"
                }
                repeat(180) { tick() }
                assert(getPortsJson().startsWith("[")) { "gba: ports read failed post-boot" }
            } finally { destroy() }
        }
        android.util.Log.i("BiosBootTest", "gba: boots on embedded BIOS OK")

        // A dev-supplied BIOS overrides the embedded one — still boots.
        val bios = File("/data/local/tmp/gba-bios.rom")
        if (bios.exists()) {
            EmulatorCore().run {
                try {
                    assert(init()) { "gba: init failed (override)" }
                    assert(loadSystem("gba", bios.absolutePath)) { "gba: loadSystem+bios failed" }
                    assert(loadRom(romBytes) == EmulatorCore.LOAD_OK) {
                        "gba: override BIOS should boot"
                    }
                    repeat(180) { tick() }
                } finally { destroy() }
            }
            android.util.Log.i("BiosBootTest", "gba: biosPath override boots OK")
        }
    }
}
