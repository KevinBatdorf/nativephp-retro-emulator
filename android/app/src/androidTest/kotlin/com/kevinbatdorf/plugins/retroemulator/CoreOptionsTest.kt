package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Per-system emulation toggles (Color Emulation, Deep Black Boost, Interframe
 * Blending) reach the core through the generic setBoolean scan, and only on
 * cores that declare the node. Deep Black Boost is SNES's — the user-visible
 * change this phase ships (see plan.md item 6): ares' sfc core defaults it on,
 * and the wrapper turns it off unless the dev opts in.
 */
@RunWith(AndroidJUnit4::class)
class CoreOptionsTest {

    @Test
    fun deepBlackBoostTogglesOnSnesAndIsAbsentElsewhere() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == EmulatorCore.LOAD_OK)
            core.tick()

            // ares' sfc core registers Deep Black Boost defaulting on.
            assert(core.getCoreBoolean("deepBlackBoost") == 1) {
                "sfc core should boot with Deep Black Boost on (ares default)"
            }
            core.setCoreBoolean("deepBlackBoost", false)
            assert(core.getCoreBoolean("deepBlackBoost") == 0) { "false must apply" }
            core.setCoreBoolean("deepBlackBoost", true)
            assert(core.getCoreBoolean("deepBlackBoost") == 1) { "true must apply" }

            // Keys the sfc core doesn't declare, and an unknown key, all report
            // absent so the applier can no-op instead of erroring.
            assert(core.getCoreBoolean("colorEmulation") == -1) { "sfc has no Color Emulation node" }
            assert(core.getCoreBoolean("interframeBlending") == -1) { "sfc has no Interframe Blending node" }
            assert(core.getCoreBoolean("bogusKey") == -1) { "unknown key is absent" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun interframeBlendingTogglesOnGameBoyButColorEmulationIsAString() {
        val romFile = File("/data/local/tmp/test-gb.rom")
        if (!romFile.exists()) {
            android.util.Log.w("CoreOptionsTest", "Skipping GB: no ROM at ${romFile.path}")
            return
        }

        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb"))
            assert(core.loadRom(romFile.readBytes()) == EmulatorCore.LOAD_OK)
            core.tick()

            // Interframe Blending is a Boolean node on the Game Boy core.
            assert(core.getCoreBoolean("interframeBlending") != -1) { "gb declares Interframe Blending" }
            core.setCoreBoolean("interframeBlending", true)
            assert(core.getCoreBoolean("interframeBlending") == 1) { "true must apply" }
            core.setCoreBoolean("interframeBlending", false)
            assert(core.getCoreBoolean("interframeBlending") == 0) { "false must apply" }

            // On original Game Boy, Color Emulation is a String palette node, not
            // a Boolean — the Boolean scan correctly finds nothing (palette is
            // separate work, see plan.md).
            assert(core.getCoreBoolean("colorEmulation") == -1) {
                "DMG Color Emulation is a String, not a Boolean toggle"
            }
        } finally {
            core.destroy()
        }
    }

    /** Same synthetic ROM-only LoROM as StateAndOptionsTest. */
    private fun makeLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES OPTIONS TEST    ".toByteArray()
        for (i in 0 until 21) rom[hb + 0x10 + i] = title[i]

        rom[hb + 0x25] = 0x20
        rom[hb + 0x26] = 0x00 // ROM only
        rom[hb + 0x27] = 0x05
        rom[hb + 0x29] = 0x01

        rom[0x7FFC] = 0x00
        rom[0x7FFD] = 0x80.toByte()

        var sum = 0
        for (b in rom) sum = (sum + (b.toInt() and 0xFF)) and 0xFFFF
        rom[hb + 0x2E] = (sum and 0xFF).toByte()
        rom[hb + 0x2F] = (sum shr 8).toByte()
        val comp = sum.inv() and 0xFFFF
        rom[hb + 0x2C] = (comp and 0xFF).toByte()
        rom[hb + 0x2D] = (comp shr 8).toByte()
        return rom
    }
}
