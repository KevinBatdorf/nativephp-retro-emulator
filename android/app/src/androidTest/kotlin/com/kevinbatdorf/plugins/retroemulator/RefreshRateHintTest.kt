package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs

/**
 * Platform::refreshRateHint port — each core reports its true region-aware
 * refresh rate when its PPU/VDP screen node loads (at loadSystem), and the
 * pacing loop consumes it instead of a hardcoded 60.0988 (EmulatorRenderer).
 *
 * Expected values derive from the core formulas at the pinned submodule
 * (colorburst = 315/88 MHz, ares.hpp Constants::Colorburst::NTSC):
 *   fc  NTSC: colorburst·6/4 / (341·262)  = 60.09848   (fc/ppu/ppu.cpp:27)
 *   sfc NTSC: colorburst·6   / (1364·262) = 60.09848   (sfc/ppu/ppu.cpp:47)
 *   gb      : 4·1024·1024    / (456·154)  = 59.72750   (gb/ppu/ppu.cpp:30)
 *   md  NTSC: colorburst·15  / (3420·262) = 59.92275   (md/vdp/vdp.cpp:42)
 *
 * All ares calls stay on the test thread — libco requires the same thread
 * from loadSystem() onward.
 *
 * This test's loadSystem-without-power lifecycle doubles as the regression
 * trigger for the stale-EntryPoints workaround
 * (SystemRegistry::clearStaleEntryPoints, see system_registry.hpp): before
 * that workaround, these four no-power destroy cycles stranded dangling
 * Thread entry points that wedged the NEXT core booted in this process —
 * the full suite hung in SystemSwitchTest while every class passed solo.
 * Keep this class running BEFORE SystemSwitchTest (alphabetical order does
 * it) so the pair keeps guarding the workaround.
 */
@RunWith(AndroidJUnit4::class)
class RefreshRateHintTest {

    private val cases = listOf(
        "fc"  to 60.09848,
        "sfc" to 60.09848,
        "gb"  to 59.72750,
        "md"  to 59.92275,
    )

    @Test
    fun everySystemReportsItsTrueRefreshRate() {
        for ((id, expected) in cases) {
            val core = AresCore()
            try {
                assert(core.init()) { "$id: init failed" }
                assert(core.refreshRateHint() == 0.0) {
                    "$id: hint must be 0 before a system loads"
                }
                assert(core.loadSystem(id)) { "$id: loadSystem failed" }
                val hint = core.refreshRateHint()
                assert(abs(hint - expected) < 0.001) {
                    "$id: expected refresh hint ≈ $expected, got $hint"
                }
            } finally {
                core.destroy()
            }
        }
    }
}
