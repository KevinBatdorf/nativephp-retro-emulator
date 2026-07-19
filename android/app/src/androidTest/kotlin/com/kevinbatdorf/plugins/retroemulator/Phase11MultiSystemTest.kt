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
 *   adb push tests/roms/cherilperils.sg    /data/local/tmp/test-sg.rom
 *   adb push tests/roms/sprite.sms         /data/local/tmp/test-ms.rom
 *   adb push tests/roms/cgb-acid2.gbc      /data/local/tmp/test-gbc.rom
 *   adb push tests/roms/pipes.a26          /data/local/tmp/test-a26.rom
 *   adb push tests/roms/helloworld.pce     /data/local/tmp/test-pce.rom
 *   adb push tests/roms/spritepriority.ws  /data/local/tmp/test-ws.rom
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
        SystemCase("sg",  "/data/local/tmp/test-sg.rom",  0xC000,   true),
        SystemCase("ms",  "/data/local/tmp/test-ms.rom",  0xC000,   false),
        // helloworld.pce runs stackless out of registers — no RAM guarantee
        // (0x2100 is the HuC6280 stack page; a real game would write it).
        SystemCase("pce", "/data/local/tmp/test-pce.rom", 0x2100,   false),
        // The WS boot splash outlasts 120 frames before the game touches iram.
        SystemCase("ws",  "/data/local/tmp/test-ws.rom",  0x0000,   false),
        // 2600 zero page IS the RIOT RAM — any running game writes it.
        SystemCase("a26", "/data/local/tmp/test-a26.rom", 0x80,     true),
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
    fun biosRequiredSystemsRejectRomWithoutFirmware() {
        // gba/ngp/msx ship as biosPath-honest defs: staging works, but LoadRom
        // must refuse pre-teardown until a dev supplies firmware.
        for (id in listOf("gba", "ngp", "msx")) {
            val core = AresCore()
            try {
                assert(core.init()) { "$id: init failed" }
                assert(core.loadSystem(id)) { "$id: loadSystem failed" }
                val rc = core.loadRom(ByteArray(0x40000))
                assert(rc == AresCore.LOAD_BIOS_REQUIRED) {
                    "$id: expected LOAD_BIOS_REQUIRED, got $rc"
                }
            } finally {
                core.destroy()
            }
            android.util.Log.i("Phase11MultiSystemTest", "$id: BIOS gate OK")
        }
    }

    @Test
    fun unknownSystemIsRejected() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(!core.loadSystem("n64")) { "n64 is not compiled — loadSystem must fail" }
        } finally {
            core.destroy()
        }
    }
}
