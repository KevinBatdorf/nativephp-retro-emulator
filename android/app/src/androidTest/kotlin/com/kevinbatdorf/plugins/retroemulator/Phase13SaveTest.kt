package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Phase 13 instrumented test — battery-save persistence round-trip.
 *
 * Uses a synthetic 32 KB LoROM with 8 KB battery RAM (no Nintendo IP): the
 * LOROM-RAM board reads save.ram from the pak at connect, and System::save()
 * writes it back. Seeding a pattern from disk and flushing must round-trip
 * the bytes through the actual board memory.
 */
@RunWith(AndroidJUnit4::class)
class Phase13SaveTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun batterySaveRoundTrips() {
        val prefix = File(context.cacheDir, "phase13-${System.nanoTime()}").absolutePath
        val saveFile = File("$prefix.save.ram")
        val pattern = ByteArray(8192) { 0xAB.toByte() }
        saveFile.writeBytes(pattern)

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc")) { "loadSystem failed" }
            assert(core.loadRom(makeSramLoRom(), prefix)) { "SRAM LoROM must load" }
            core.tick()
            assert(core.flushSaves()) { "flush must succeed with a save prefix" }

            val flushed = saveFile.readBytes()
            assert(flushed.size == 8192) { "expected 8192 bytes, got ${flushed.size}" }
            assert(flushed.contentEquals(pattern)) { "seeded SRAM must survive the round-trip" }
        } finally {
            core.destroy()
            saveFile.delete()
        }
    }

    @Test
    fun flushWithoutPrefixReturnsFalse() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeSramLoRom(), null))
            assert(!core.flushSaves()) { "no prefix → nothing persisted" }
        } finally {
            core.destroy()
        }
    }

    /**
     * Minimal valid LoROM with battery RAM — mirrors the iOS BootTests helper.
     * SEI at the reset vector, LoROM header at $7FB0, ROM type $02
     * (ROM+RAM+battery), RAM size byte $04 (SfcPakBuilder: 1 << (v-1) KB = 8 KB).
     */
    private fun makeSramLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78          // SEI — scores +8 in header detection
        rom[0x0001] = 0x80.toByte() // BRA
        rom[0x0002] = 0xFD.toByte() // -3 → loops forever

        val hb = 0x7FB0
        val title = "ARES SAVE TEST       ".toByteArray()
        for (i in 0 until 21) rom[hb + 0x10 + i] = title[i]

        rom[hb + 0x25] = 0x20 // LoROM map mode
        rom[hb + 0x26] = 0x02 // ROM+RAM+battery
        rom[hb + 0x27] = 0x05 // 32 KB
        rom[hb + 0x28] = 0x04 // 8 KB SRAM
        rom[hb + 0x29] = 0x01 // USA → NTSC

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
