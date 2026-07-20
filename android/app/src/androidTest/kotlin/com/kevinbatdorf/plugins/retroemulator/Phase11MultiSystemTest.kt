package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Phase 11 instrumented test — each compiled system loads its core, boots a
 * homebrew ROM headlessly (no GL surface — the video callback is a no-op
 * without a bound texture), ticks 120 frames, and answers a work-RAM read.
 *
 * Test ROMs come from scripts/fetch_test_roms.sh and must be pushed first:
 *   adb push tests/roms/nestest.nes        /data/local/tmp/test-fc.rom
 *   adb push tests/roms/helloworld.sfc     /data/local/tmp/test-sfc.rom
 *   adb push tests/roms/dmg-acid2.gb       /data/local/tmp/test-gb.rom
 *   adb push tests/roms/helloworld.md      /data/local/tmp/test-md.rom
 *   adb push tests/roms/cgb-acid2.gbc      /data/local/tmp/test-gbc.rom
 *
 * All ares calls stay on the test thread — libco requires the same thread
 * from loadSystem() onward.
 */
@RunWith(AndroidJUnit4::class)
class Phase11MultiSystemTest {

    private data class SystemCase(
        val id: String,
        val romPath: String,
        // A work-RAM bus address inside the system's readMemory window.
        val probeAddress: Int,
        // True when the test ROM is known to write this window early: catches
        // "booted but executing garbage" (an empty cartridge read leaves RAM
        // untouched — how the cross-DSO vfs bug hid behind green boots).
        val expectRamActivity: Boolean,
    )

    private val cases = listOf(
        SystemCase("fc",  "/data/local/tmp/test-fc.rom",  0x0000,   true),
        // helloworld.sfc renders without touching low WRAM, dmg-acid2 works
        // out of VRAM/HRAM, helloworld.md may skip the probe window — no
        // activity guarantee for those three (sfc has the conformance suite).
        SystemCase("sfc", "/data/local/tmp/test-sfc.rom", 0x7E0000, false),
        SystemCase("gb",  "/data/local/tmp/test-gb.rom",  0xC000,   false),
        SystemCase("gbc", "/data/local/tmp/test-gbc.rom", 0xC000,   false),
        SystemCase("md",  "/data/local/tmp/test-md.rom",  0xFF0000, false),
    )

    @Test
    fun everyCompiledSystemBootsARom() {
        for (case in cases) {
            val romFile = File(case.romPath)
            if (!romFile.exists()) {
                android.util.Log.w("Phase11MultiSystemTest",
                    "Skipping ${case.id}: no ROM at ${case.romPath}")
                continue
            }

            val core = AresCore()
            try {
                assert(core.init()) { "${case.id}: init failed" }
                assert(core.loadSystem(case.id)) { "${case.id}: loadSystem failed" }
                assert(core.loadRom(romFile.readBytes()) == AresCore.LOAD_OK) { "${case.id}: loadRom failed" }

                repeat(120) { core.tick() }

                val bytes = core.readMemory(case.probeAddress, 64)
                assert(bytes != null && bytes.size == 64) {
                    "${case.id}: readMemory(0x${case.probeAddress.toString(16)}) failed"
                }
                if (case.expectRamActivity) {
                    assert(bytes!!.any { it != 0.toByte() }) {
                        "${case.id}: work RAM untouched after 120 frames — ROM not executing"
                    }
                }

                val ports = core.getPortsJson()
                assert(ports.startsWith("[") && ports.contains("buttons")) {
                    "${case.id}: unexpected ports JSON: $ports"
                }
            } finally {
                core.destroy()
            }
            android.util.Log.i("Phase11MultiSystemTest", "${case.id}: OK")
        }
    }

    @Test
    fun unknownSystemIsRejected() {
        val core = AresCore()
        try {
            assert(core.init())
            // "pce" is a valid ares id but not compiled into this build.
            assert(!core.loadSystem("pce")) { "pce is not compiled — loadSystem must fail" }
        } finally {
            core.destroy()
        }
    }
}
