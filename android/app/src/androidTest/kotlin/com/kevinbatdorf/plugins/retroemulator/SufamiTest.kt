package com.kevinbatdorf.plugins.retroemulator

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Slotted media (plan.md step 1.10) — SuFami Turbo. Needs copyrighted base
 * firmware + a slot title staged on the device (never in the repo):
 *   /data/local/tmp/sufami-bios.sfc  — Sufami Turbo (Japan) base BIOS
 *   /data/local/tmp/poipoi.st        — a Sufami Turbo slot game
 * Tests skip cleanly if the media is absent.
 */
@RunWith(AndroidJUnit4::class)
class SufamiTest {

    @Test
    fun sufamiBaseBootsToMenu() {
        val bios = File("/data/local/tmp/sufami-bios.sfc")
        if (!bios.exists()) { Log.w("SufamiTest", "no base BIOS; skipping"); return }

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            // Base BIOS is a normal .sfc cartridge; detectHeader sees the BANDAI
            // signature → ST-LOROM → ares creates the two slots and the BIOS boots.
            assert(core.loadRom(bios.readBytes(), null) == AresCore.LOAD_OK) { "base should load" }
            repeat(10) { core.tick() }
            assert(core.getFrameWidth() > 0) { "SuFami base should render its insert-cartridge menu" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun sufamiSlotGameBoots() {
        val bios = File("/data/local/tmp/sufami-bios.sfc")
        val slot = File("/data/local/tmp/poipoi.st")
        if (!bios.exists() || !slot.exists()) { Log.w("SufamiTest", "no media; skipping"); return }

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            core.stageSlot(0, slot.readBytes())   // Slot A
            assert(core.loadRom(bios.readBytes(), null) == AresCore.LOAD_OK) { "base + slot should load" }
            repeat(30) { core.tick() }
            // getFrameWidth alone is a weak check (the base menu also renders) —
            // assert the slot cartridge actually connected (the game is present).
            assert(core.isSlotConnected(0)) { "slot A cartridge should be connected" }
            assert(!core.isSlotConnected(1)) { "slot B left empty" }
            assert(core.getFrameWidth() > 0) { "slot game should render through the base" }
        } finally {
            core.destroy()
        }
    }
}
