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

    @Test
    fun sufamiDualCartBoots() {
        // A+B dual-cart linking: two SD Gundam Generation carts staged into
        // both nested SuFami slots. Closes outstanding #SuFami-slot-B.
        //   /data/local/tmp/sufami-bios.sfc — Sufami Turbo (Japan) base BIOS
        //   /data/local/tmp/sufami-a.st     — linkable cart in Slot A
        //   /data/local/tmp/sufami-b.st     — linkable cart in Slot B
        val bios  = File("/data/local/tmp/sufami-bios.sfc")
        val slotA = File("/data/local/tmp/sufami-a.st")
        val slotB = File("/data/local/tmp/sufami-b.st")
        if (!bios.exists() || !slotA.exists() || !slotB.exists()) {
            Log.w("SufamiTest", "no dual-cart media; skipping"); return
        }

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            core.stageSlot(0, slotA.readBytes())   // Slot A
            core.stageSlot(1, slotB.readBytes())   // Slot B
            assert(core.loadRom(bios.readBytes(), null) == AresCore.LOAD_OK) {
                "base + both slots should load"
            }
            repeat(30) { core.tick() }
            // Both nested SuFami slots must have materialized a connected
            // cartridge — the dual-cart link path, not just Slot A.
            assert(core.isSlotConnected(0)) { "slot A cartridge should be connected" }
            assert(core.isSlotConnected(1)) { "slot B cartridge should be connected" }
            assert(core.getFrameWidth() > 0) { "dual-cart base should render" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun bsxMemoryCartBoots() {
        val bios = File("/data/local/tmp/bsx-bios.sfc")
        val bs = File("/data/local/tmp/satella.bs")
        if (!bios.exists() || !bs.exists()) { Log.w("SufamiTest", "no BS-X media; skipping"); return }

        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            // BS-X base is a normal .sfc (serial ZBSJ → BS-MCC board → BS Memory
            // slot); the .bs cassette is staged into that single slot (index 0).
            core.stageSlot(0, bs.readBytes())
            assert(core.loadRom(bios.readBytes(), null) == AresCore.LOAD_OK) { "BS-X base + cassette should load" }
            // The BIOS reads its MCC download PSRAM well after the first frames —
            // a short run false-passes. Run long enough to fault if the base pak
            // failed to allocate the Save/Download writable memories.
            repeat(600) { core.tick() }
            assert(core.isSlotConnected(0)) { "BS Memory cassette should be connected" }
            assert(core.getFrameWidth() > 0) { "BS-X should render" }
        } finally {
            core.destroy()
        }
    }
}
