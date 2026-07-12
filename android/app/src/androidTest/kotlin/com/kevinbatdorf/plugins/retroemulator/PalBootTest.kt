package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs

/**
 * ROM-first boot + region resolution (plan 4b) — the audit item that PAL ROMs
 * booted NTSC cores. loadSystem stages; loadRom analyzes the ROM, resolves the
 * region like desktop-ares (Emulator::region(), emulator.cpp:40-60), and boots
 * the region-correct system variant.
 *
 * PAL SFC refresh = cpuFrequency(PAL colorburst · 4.8) / (1364 · 312) ≈ 50.0070
 * (sfc/ppu/ppu.cpp, 312-line PAL frame); NTSC ≈ 60.09848.
 */
@RunWith(AndroidJUnit4::class)
class PalBootTest {

    private lateinit var core: AresCore

    @Before
    fun setUp() {
        core = AresCore()
        assertTrue(core.init())
    }

    @After
    fun tearDown() {
        core.destroy()
    }

    @Test
    fun palRomBootsPalCore() {
        assertTrue(core.loadSystem("sfc"))
        assertEquals(0.0, core.refreshRateHint(), 0.0)
        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = true)))
        assertEquals("PAL", core.getRegion())
        assertTrue(
            "PAL boot must run PAL timing, got ${core.refreshRateHint()}",
            abs(core.refreshRateHint() - 50.0070) < 0.001,
        )
    }

    @Test
    fun ntscRomStillBootsNtsc() {
        assertTrue(core.loadSystem("sfc"))
        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = false)))
        assertEquals("NTSC", core.getRegion())
        assertTrue(abs(core.refreshRateHint() - 60.09848) < 0.001)
    }

    @Test
    fun regionOverrideWinsOverAnalysis() {
        assertTrue(core.loadSystem("sfc"))
        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = false), region = "PAL"))
        assertEquals("PAL", core.getRegion())
        assertTrue(abs(core.refreshRateHint() - 50.0070) < 0.001)
    }

    @Test
    fun romSwapAcrossRegionsRebootsTheRightVariant() {
        assertTrue(core.loadSystem("sfc"))
        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = false)))
        repeat(5) { core.tick() }
        assertEquals("NTSC", core.getRegion())

        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = true)))
        repeat(5) { core.tick() }
        assertEquals("PAL", core.getRegion())
        assertTrue(abs(core.refreshRateHint() - 50.0070) < 0.001)
    }

    @Test
    fun portsJsonAnswersFromStagingBeforeAnyBoot() {
        assertTrue(core.loadSystem("sfc"))
        val json = core.getPortsJson()
        assertTrue("expected buttons in $json", json.contains("buttons"))
        assertTrue("expected two ports in $json", json.contains("\"port\":2"))
    }

    @Test
    fun rejectedRomLeavesTheRunningGameUntouched() {
        assertTrue(core.loadSystem("sfc"))
        assertEquals(AresCore.LOAD_OK, core.loadRom(makeLoRom(pal = false)))
        repeat(5) { core.tick() }

        // Garbage that fails the sfc analyzer — pre-teardown rejection.
        assertEquals(AresCore.LOAD_REJECTED, core.loadRom(ByteArray(100)))
        assertEquals("NTSC", core.getRegion())
        core.tick()
        assertTrue(core.refreshRateHint() > 0.0)
    }

    /** Same synthetic LoROM as VideoGeometryTest; country byte picks the region. */
    private fun makeLoRom(pal: Boolean): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES PAL BOOT TEST   ".toByteArray()
        for (i in 0 until 21) rom[hb + 0x10 + i] = title[i]

        rom[hb + 0x25] = 0x20
        rom[hb + 0x26] = 0x00
        rom[hb + 0x27] = 0x05
        // Country byte: $01 USA = NTSC, $02 Europe = PAL (sfc analyzer).
        rom[hb + 0x29] = if (pal) 0x02 else 0x01

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
