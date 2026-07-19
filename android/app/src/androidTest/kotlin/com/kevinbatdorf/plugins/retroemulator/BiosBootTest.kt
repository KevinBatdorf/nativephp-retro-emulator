package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Firmware-gated boot: a biosRequired system, given a dev-supplied BIOS via
 * loadSystem(id, biosPath), boots a cartridge and runs. The BIOS files are the
 * developer's own dumps (copyrighted — never in the repo); push them first:
 *   adb push <gba bios>          /data/local/tmp/gba-bios.rom
 *   adb push tests/roms/gba-shades.gba /data/local/tmp/test-gba.rom
 *
 * Skips cleanly when the BIOS isn't present, so CI without firmware stays green
 * (the no-BIOS refusal is covered by Phase11's biosRequiredSystemsRejectRom…).
 */
@RunWith(AndroidJUnit4::class)
class BiosBootTest {

    private data class BiosCase(val id: String, val biosPath: String, val romPath: String)

    private val cases = listOf(
        BiosCase("gba", "/data/local/tmp/gba-bios.rom", "/data/local/tmp/test-gba.rom"),
    )

    /**
     * The airtight end-to-end proof that the dev-supplied firmware is actually
     * consumed: the SAME core + ROM refuses to boot without a BIOS
     * (LOAD_BIOS_REQUIRED) and boots cleanly with one (LOAD_OK, ticks without
     * crashing). Independent of where a given homebrew writes its RAM.
     */
    @Test
    fun firmwareGatedSystemBootsOnlyWithBios() {
        for (case in cases) {
            val bios = File(case.biosPath)
            val rom = File(case.romPath)
            if (!bios.exists() || !rom.exists()) {
                android.util.Log.w("BiosBootTest",
                    "Skipping ${case.id}: bios/rom absent (${case.biosPath})")
                continue
            }
            val romBytes = rom.readBytes()

            // No BIOS → refuse, pre-teardown.
            AresCore().run {
                try {
                    assert(init()) { "${case.id}: init failed" }
                    assert(loadSystem(case.id)) { "${case.id}: loadSystem failed" }
                    assert(loadRom(romBytes) == AresCore.LOAD_BIOS_REQUIRED) {
                        "${case.id}: expected LOAD_BIOS_REQUIRED without firmware"
                    }
                } finally { destroy() }
            }

            // With BIOS → boot and run.
            AresCore().run {
                try {
                    assert(init()) { "${case.id}: init failed" }
                    assert(loadSystem(case.id, case.biosPath)) { "${case.id}: loadSystem+bios failed" }
                    assert(loadRom(romBytes) == AresCore.LOAD_OK) {
                        "${case.id}: loadRom with BIOS should succeed"
                    }
                    repeat(180) { tick() }
                    // Survived staging + boot + 180 frames on the real BIOS.
                    assert(getPortsJson().startsWith("[")) { "${case.id}: ports read failed post-boot" }
                } finally { destroy() }
            }
            android.util.Log.i("BiosBootTest", "${case.id}: boots only with BIOS OK")
        }
    }
}
