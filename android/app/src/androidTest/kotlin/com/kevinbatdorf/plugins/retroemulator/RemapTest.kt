package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Per-port controller remapping (plan.md step 1.4). setInputMapping repoints a
 * core button at a different positional slot; the change is applied on the
 * render thread, so each assertion ticks once after mapping to consume it.
 * getButtonBit reports the slot a core button currently reads.
 */
@RunWith(AndroidJUnit4::class)
class RemapTest {

    @Test
    fun remapSwapsMergesAndResets() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)
            // Controllers are explicit — register a gamepad on port 1 first.
            assert(core.connectDevice("sfc", 1, "Gamepad").isEmpty()) { "connect gamepad" }
            core.tick()

            // Defaults: A on face-east (bit 8), B on face-south (bit 0).
            assert(core.getButtonBit(1, "A") == EmulatorInput.BTN_A) { "A defaults to its own slot" }
            assert(core.getButtonBit(1, "B") == EmulatorInput.BTN_B) { "B defaults to its own slot" }

            // Swap A and B on port 1.
            assert(core.setInputMapping(1, arrayOf("A", "B"), arrayOf("B", "A")).isEmpty()) {
                "valid swap should succeed"
            }
            core.tick()
            assert(core.getButtonBit(1, "A") == EmulatorInput.BTN_B) { "A now reads B's slot" }
            assert(core.getButtonBit(1, "B") == EmulatorInput.BTN_A) { "B now reads A's slot" }

            // Merge: repoint only A → Y, leaving B's swap untouched.
            assert(core.setInputMapping(1, arrayOf("a"), arrayOf("y")).isEmpty()) { "merge should succeed" }
            core.tick()
            assert(core.getButtonBit(1, "A") == EmulatorInput.BTN_Y) { "A now reads Y's slot" }
            assert(core.getButtonBit(1, "B") == EmulatorInput.BTN_A) { "B keeps its earlier remap" }

            // Port 2 is independent — register a gamepad and it keeps defaults.
            assert(core.connectDevice("sfc", 2, "Gamepad").isEmpty()) { "connect gamepad on port 2" }
            core.tick()
            assert(core.getButtonBit(2, "A") == EmulatorInput.BTN_A) { "port 2 A is still default" }

            // Empty map resets the whole port to defaults.
            assert(core.setInputMapping(1, arrayOf(), arrayOf()).isEmpty()) { "reset should succeed" }
            core.tick()
            assert(core.getButtonBit(1, "A") == EmulatorInput.BTN_A) { "A back to default" }
            assert(core.getButtonBit(1, "B") == EmulatorInput.BTN_B) { "B back to default" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun remapRejectsBadInput() {
        val core = AresCore()
        try {
            assert(core.init())

            // No staged system yet.
            assert(core.setInputMapping(1, arrayOf("A"), arrayOf("B")) == "SYSTEM_NOT_LOADED") {
                "remap before load is SYSTEM_NOT_LOADED"
            }

            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)
            core.tick()

            // No controller registered on the port yet.
            assert(core.setInputMapping(1, arrayOf("A"), arrayOf("B")) == "INVALID_PARAMETERS") {
                "remap with no device is INVALID_PARAMETERS"
            }
            assert(core.connectDevice("sfc", 1, "Gamepad").isEmpty()) { "connect gamepad" }
            core.tick()

            // sfc has 2 ports — port 3 is out of range.
            assert(core.setInputMapping(3, arrayOf("A"), arrayOf("B")) == "INVALID_PARAMETERS") {
                "out-of-range port is INVALID_PARAMETERS"
            }
            // An unknown button names itself in the detail.
            assert(core.setInputMapping(1, arrayOf("A"), arrayOf("q")) == "UNKNOWN_BUTTON:q") {
                "unknown source button is reported with its name"
            }
            // A rejected batch leaves the defaults intact.
            core.tick()
            assert(core.getButtonBit(1, "A") == EmulatorInput.BTN_A) { "rejected remap changed nothing" }
        } finally {
            core.destroy()
        }
    }

    /** Same synthetic ROM-only LoROM as CoreOptionsTest / StateAndOptionsTest. */
    private fun makeLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES REMAP TEST      ".toByteArray()
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
