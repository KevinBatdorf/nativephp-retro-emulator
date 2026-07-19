package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.abs

/**
 * Platform::refreshRateHint port — each core reports its true region-aware
 * refresh rate when its PPU/VDP screen node loads, and the pacing loop
 * consumes it instead of a hardcoded 60.0988 (EmulatorRenderer).
 *
 * Under the ROM-first boot model (plan 4b) the screen node loads inside
 * loadRom — loadSystem only stages, so the hint must stay 0 until a ROM
 * boots the core. Test ROMs come from scripts/fetch_test_roms.sh and must
 * be pushed first (same files as Phase11MultiSystemTest):
 *   adb push tests/roms/nestest.nes    /data/local/tmp/test-fc.rom
 *   adb push tests/roms/helloworld.sfc /data/local/tmp/test-sfc.rom
 *   adb push tests/roms/dmg-acid2.gb   /data/local/tmp/test-gb.rom
 *   adb push tests/roms/helloworld.md  /data/local/tmp/test-md.rom
 *
 * Expected values derive from the core formulas at the pinned submodule
 * (colorburst = 315/88 MHz, ares.hpp Constants::Colorburst::NTSC):
 *   fc  NTSC: colorburst·6/4 / (341·262)  = 60.09848   (fc/ppu/ppu.cpp:27)
 *   sfc NTSC: colorburst·6   / (1364·262) = 60.09848   (sfc/ppu/ppu.cpp:47)
 *   gb      : 4·1024·1024    / (456·154)  = 59.72750   (gb/ppu/ppu.cpp:30)
 *   md  NTSC: colorburst·15  / (3420·262) = 59.92275   (md/vdp/vdp.cpp:42)
 *
 * All ares calls stay on the test thread — libco requires the same thread
 * from the boot onward.
 */
@RunWith(AndroidJUnit4::class)
class RefreshRateHintTest {

    private data class HintCase(val id: String, val romPath: String, val expected: Double)

    private val cases = listOf(
        HintCase("fc",  "/data/local/tmp/test-fc.rom",  60.09848),
        HintCase("sfc", "/data/local/tmp/test-sfc.rom", 60.09848),
        HintCase("gb",  "/data/local/tmp/test-gb.rom",  59.72750),
        HintCase("md",  "/data/local/tmp/test-md.rom",  59.92275),
    )

    @Test
    fun everySystemReportsItsTrueRefreshRate() {
        for (case in cases) {
            val romFile = File(case.romPath)
            if (!romFile.exists()) {
                android.util.Log.w("RefreshRateHintTest",
                    "Skipping ${case.id}: no ROM at ${case.romPath}")
                continue
            }

            val core = AresCore()
            try {
                assert(core.init()) { "${case.id}: init failed" }
                assert(core.refreshRateHint() == 0.0) {
                    "${case.id}: hint must be 0 before anything loads"
                }
                assert(core.loadSystem(case.id)) { "${case.id}: loadSystem failed" }
                assert(core.refreshRateHint() == 0.0) {
                    "${case.id}: staging must not boot a core — hint still 0"
                }
                assert(core.loadRom(romFile.readBytes()) == AresCore.LOAD_OK) {
                    "${case.id}: loadRom failed"
                }
                val hint = core.refreshRateHint()
                assert(abs(hint - case.expected) < 0.001) {
                    "${case.id}: expected refresh hint ≈ ${case.expected}, got $hint"
                }
            } finally {
                core.destroy()
            }
        }
    }
}
