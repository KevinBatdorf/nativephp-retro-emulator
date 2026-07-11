package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Save-state and AV-option coverage the Phase 13/14 suites skipped: a full
 * state snapshot must round-trip WRAM contents across the save point, and
 * the option setters must accept their documented ranges while emulation
 * keeps ticking.
 */
@RunWith(AndroidJUnit4::class)
class StateAndOptionsTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    /** WRAM the synthetic ROM's idle loop never touches. */
    private val scratch = 0x7E1F00

    @Test
    fun stateSaveRoundTripsWramContents() {
        val statePath = File(context.cacheDir, "state-${System.nanoTime()}.bst").absolutePath

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null))
            repeat(5) { core.tick() }

            core.writeMemory(scratch, byteArrayOf(0x5A, 0x3C))
            core.tick()
            assert(core.stateSave(statePath)) { "stateSave must succeed while running" }
            assert(File(statePath).length() > 0) { "state file must not be empty" }

            core.writeMemory(scratch, byteArrayOf(0x00, 0x00))
            core.tick()
            assert(core.stateLoad(statePath)) { "stateLoad must succeed" }

            val bytes = core.readMemory(scratch, 2)
            assert(bytes != null && bytes[0] == 0x5A.toByte() && bytes[1] == 0x3C.toByte()) {
                "WRAM must match the snapshot, got ${bytes?.joinToString()}"
            }
        } finally {
            core.destroy()
            File(statePath).delete()
        }
    }

    @Test
    fun stateLoadFailsForMissingFile() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null))
            core.tick()
            assert(!core.stateLoad(File(context.cacheDir, "does-not-exist.bst").absolutePath)) {
                "loading a missing state must fail, not crash"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun audioAndVideoOptionsApplyWhileRunning() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null))
            core.tick()

            core.setAudio(volume = 0.5f, balance = -1.0f)
            core.setAudio(volume = 1.0f, balance = 1.0f)
            core.setVideo(
                luminance = 0.5f,
                saturation = 0.5f,
                gamma = 1.5f,
                colorBleed = true,
                interframeBlending = true,
            )
            repeat(5) { core.tick() }

            assert(core.getFrameWidth() > 0) { "frame must still be produced with options applied" }
        } finally {
            core.destroy()
        }
    }

    /** Same synthetic LoROM as Phase13SaveTest, without the battery RAM. */
    private fun makeLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES STATE TEST      ".toByteArray()
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
