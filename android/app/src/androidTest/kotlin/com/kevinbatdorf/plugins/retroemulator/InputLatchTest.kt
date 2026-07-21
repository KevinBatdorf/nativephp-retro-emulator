package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Press-latch regression: a touch tap's press+release arrive between two core
 * input polls (<5ms apart), and before the latch the pulse was lost entirely —
 * on-screen buttons "didn't work" for real fingers while 600ms injected test
 * holds passed. The latch defers a release that lands before its press was
 * ever sampled until right after the first sample.
 *
 * The synthetic ROM enables SNES auto-joypad polling ($4200 = $01) so the
 * core genuinely samples the pad every frame — the plain idle-loop ROM never
 * polls and would never clear the latch.
 */
@RunWith(AndroidJUnit4::class)
class InputLatchTest {

    @Test
    fun subFrameTapIsHeldUntilSampledThenReleased() {
        val core = AresCore()
        try {
            assertTrue(core.init())
            assertTrue(core.loadSystem("sfc"))
            assertEquals(AresCore.LOAD_OK, core.loadRom(makePollingRom(), null))
            repeat(3) { core.tick() }   // boot far enough that auto-poll runs

            val bit = core.getButtonBit(1, "Start")
            assertTrue("Start must resolve on the default pad", bit > 0)

            // A real finger tap: press + release with NO tick between.
            assertEquals("", core.pressButton(1, "Start", true))
            assertEquals("", core.pressButton(1, "Start", false))

            // Release must be deferred — the core hasn't sampled the press.
            assertNotEquals(0, core.getInputState(1) and bit)

            // One frame samples the pad (auto-poll) and applies the release.
            core.tick()
            assertEquals(0, core.getInputState(1) and bit)
        } finally {
            core.destroy()
        }
    }

    @Test
    fun sampledPressReleasesImmediately() {
        val core = AresCore()
        try {
            assertTrue(core.init())
            assertTrue(core.loadSystem("sfc"))
            assertEquals(AresCore.LOAD_OK, core.loadRom(makePollingRom(), null))
            repeat(3) { core.tick() }

            val bit = core.getButtonBit(1, "Start")
            assertTrue(bit > 0)

            // Held across a frame (the pad was sampled) — release is instant.
            assertEquals("", core.pressButton(1, "Start", true))
            core.tick()
            assertEquals("", core.pressButton(1, "Start", false))
            assertEquals(0, core.getInputState(1) and bit)
        } finally {
            core.destroy()
        }
    }

    /** Idle-loop LoROM that turns on auto-joypad polling ($4200 = $01). */
    private fun makePollingRom(): ByteArray {
        val rom = ByteArray(0x8000)
        val code = byteArrayOf(
            0x78,                                  // sei
            0xA9.toByte(), 0x01,                   // lda #$01
            0x8D.toByte(), 0x00, 0x42,             // sta $4200 (auto-joypad on)
            0x80.toByte(), 0xFE.toByte(),          // bra * (idle)
        )
        code.copyInto(rom)

        val hb = 0x7FB0
        val title = "ARES LATCH TEST      ".toByteArray()
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
