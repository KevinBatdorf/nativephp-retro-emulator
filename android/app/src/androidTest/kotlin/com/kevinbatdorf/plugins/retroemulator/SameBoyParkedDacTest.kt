package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs

/**
 * A wave DAC parked while the game writes NR50 must come back silent. SameBoy
 * stored its 0x10 refresh marker as the parked sample, so re-enabling the DAC
 * faded in an out-of-range level: the pop under Link's Awakening's dialog.
 */
@RunWith(AndroidJUnit4::class)
class SameBoyParkedDacTest {

    /** A wave table of 8s is exactly zero-mean, so every legitimate output is silence. */
    private fun rom(): ByteArray {
        val rom = ByteArray(0x8000)
        byteArrayOf(0x00, 0xC3.toByte(), 0x50, 0x01).copyInto(rom, 0x100)
        "PARKEDDAC".toByteArray().copyInto(rom, 0x134)
        fun w(reg: Int, value: Int) = listOf(0x3E, value, 0xE0, reg)
        val delay = listOf(0xCD, 0x00, 0x02)
        val program = buildList {
            addAll(w(0x26, 0x80)); addAll(w(0x24, 0x77)); addAll(w(0x25, 0xFF)); addAll(w(0x1A, 0x00))
            add(0x3E); add(0x88)
            for (i in 0 until 16) { add(0xE0); add(0x30 + i) }
            addAll(w(0x1A, 0x80)); addAll(w(0x1C, 0x20)); addAll(w(0x1D, 0x00)); addAll(w(0x1E, 0x87)); addAll(delay)
            addAll(w(0x1A, 0x00)); addAll(delay)
            addAll(w(0x24, 0x77)); addAll(delay)
            addAll(w(0x1A, 0x80))
            addAll(listOf(0x3E, 0x42, 0xEA, 0x00, 0xC0, 0x18, 0xFE))
        }
        program.forEachIndexed { i, b -> rom[0x150 + i] = b.toByte() }
        intArrayOf(0x0E, 0x40, 0x06, 0x00, 0x05, 0x20, 0xFD, 0x0D, 0x20, 0xF8, 0xC9)
            .forEachIndexed { i, b -> rom[0x200 + i] = b.toByte() }
        var checksum = 0
        for (i in 0x134 until 0x14D) checksum = (checksum - rom[i] - 1) and 0xFF
        rom[0x14D] = checksum.toByte()
        return rom
    }

    @Test
    fun reenablingAParkedWaveDacAfterAnNr50WriteStaysSilent() {
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = "sameboy")) { "loadSystem failed" }
            assert(core.loadRom(rom()) == EmulatorCore.LOAD_OK) { "loadRom failed" }

            var peak = 0f
            val buf = FloatArray(8192)
            repeat(120) {
                core.tick()
                while (true) {
                    val n = core.readAudio(buf)
                    if (n <= 0) break
                    for (i in 0 until n) peak = maxOf(peak, abs(buf[i]))
                }
            }

            assert(core.readMemory(0xC000, 1)?.get(0) == 0x42.toByte()) { "the test ROM never reached its last write" }
            assert(peak < 0.01f) { "parked wave DAC came back at $peak — the 0x10 refresh marker leaked into the mix" }
        } finally {
            core.destroy()
        }
    }
}
