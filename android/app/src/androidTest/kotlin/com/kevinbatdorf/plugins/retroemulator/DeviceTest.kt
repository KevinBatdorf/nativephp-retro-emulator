package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Device selection + the axis input channel (plan.md step 1.5). Controllers are
 * explicit: nothing is connected until connectDevice, and each device exposes
 * its own buttons/axes. The SNES Mouse is the reference axis device.
 */
@RunWith(AndroidJUnit4::class)
class DeviceTest {

    @Test
    fun mouseConnectsExposesButtonsAndAxes() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)

            // Nothing plugged in yet — no cached inputs.
            core.tick()
            assert(core.getButtonBit(1, "A") == -1) { "no device → no button" }

            // Connect a mouse; its buttons are Left/Right, axes X/Y.
            assert(core.connectDevice("sfc", 1, "Mouse").isEmpty()) { "connect mouse" }
            core.tick()
            assert(core.getButtonBit(1, "Left") == (1 shl 0)) { "mouse Left bit" }
            assert(core.getButtonBit(1, "Right") == (1 shl 1)) { "mouse Right bit" }
            assert(core.getButtonBit(1, "A") == -1) { "mouse has no A button" }

            // Software press resolves against the mouse's own buttons.
            assert(core.pressButton(1, "Left", true).isEmpty()) { "press mouse Left" }
            assert(core.getInputState(1) and (1 shl 0) != 0) { "Left is held" }
            assert(core.pressButton(1, "A", true) == "UNKNOWN_BUTTON:A") { "mouse has no A" }

            // Axis channel: relative deltas accumulate, unknown axes rejected.
            assert(core.setAxis(1, "X", 10).isEmpty()) { "set X" }
            assert(core.setAxis(1, "X", 5).isEmpty()) { "add to X" }
            assert(core.getAxisAccum(1, "X") == 15) { "X accumulates" }
            assert(core.setAxis(1, "Z", 1) == "INVALID_PARAMETERS") { "mouse has no Z axis" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun connectRejectsUnsupportedAndSwapsDevices() {
        val core = AresCore()
        try {
            assert(core.init())

            // Before a system is staged (empty staged id).
            assert(core.connectDevice("", 1, "Mouse") == "SYSTEM_NOT_LOADED") { "no system" }

            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)

            // A device the system doesn't (yet) support.
            assert(core.connectDevice("sfc", 1, "Super Scope") == "UNSUPPORTED_DEVICE") { "not in table" }
            assert(core.connectDevice("sfc", 3, "Gamepad") == "INVALID_PARAMETERS") { "bad port" }

            // Swap gamepad → mouse on the same port; caches follow.
            assert(core.connectDevice("sfc", 1, "Gamepad").isEmpty())
            core.tick()
            assert(core.getButtonBit(1, "A") == (1 shl 8)) { "gamepad A" }
            assert(core.connectDevice("sfc", 1, "Mouse").isEmpty())
            core.tick()
            assert(core.getButtonBit(1, "A") == -1) { "mouse has no A after swap" }
            assert(core.getButtonBit(1, "Left") == (1 shl 0)) { "mouse Left after swap" }
        } finally {
            core.destroy()
        }
    }

    /** Same synthetic ROM-only LoROM as CoreOptionsTest / RemapTest. */
    private fun makeLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES DEVICE TEST     ".toByteArray()
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
