package com.kevinbatdorf.plugins.retroemulator

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Translates Android gamepad events into SNES button bitmasks and writes them
 * to the ares input state via [AresCore.setInputState].
 *
 * All methods must be called from the UI thread (the same thread that delivers
 * [KeyEvent] and [MotionEvent] via dispatchKeyEvent / dispatchGenericMotionEvent).
 * [AresCore.setInputState] is thread-safe and may be called from any thread.
 *
 * Button layout (SNES face buttons use Nintendo's positional convention):
 *   Android A → SNES B (bottom)   Android B → SNES A (right)
 *   Android X → SNES Y (left)     Android Y → SNES X (top)
 */
class EmulatorInput(private val core: AresCore) {

    companion object {
        // Bit positions — must match the kSnesButtons map in ares_jni.cpp.
        const val BTN_B      = 1 shl 0
        const val BTN_Y      = 1 shl 1
        const val BTN_SELECT = 1 shl 2
        const val BTN_START  = 1 shl 3
        const val BTN_UP     = 1 shl 4
        const val BTN_DOWN   = 1 shl 5
        const val BTN_LEFT   = 1 shl 6
        const val BTN_RIGHT  = 1 shl 7
        const val BTN_A      = 1 shl 8
        const val BTN_X      = 1 shl 9
        const val BTN_L      = 1 shl 10
        const val BTN_R      = 1 shl 11
    }

    // Current button state split into two sources so they can be updated
    // independently without clobbering each other.
    private var keyBits = 0  // set by dispatchKeyEvent (UI thread)
    private var hatBits = 0  // set by dispatchGenericMotionEvent hat axes (UI thread)

    /**
     * Process a key event from a connected gamepad. Returns true if the event
     * was consumed (i.e., maps to a recognised SNES button).
     */
    fun onKeyEvent(event: KeyEvent): Boolean {
        val bit = keycodeToBit(event.keyCode) ?: return false
        when (event.action) {
            KeyEvent.ACTION_DOWN -> keyBits = keyBits or bit
            KeyEvent.ACTION_UP   -> keyBits = keyBits and bit.inv()
            else -> {}
        }
        push()
        return true
    }

    /**
     * Process a generic motion event from a gamepad joystick or hat switch.
     * Returns true if the event source is a gamepad or joystick.
     */
    fun onMotionEvent(event: MotionEvent): Boolean {
        val src = event.source
        if (src and InputDevice.SOURCE_GAMEPAD == 0 &&
            src and InputDevice.SOURCE_JOYSTICK == 0) {
            return false
        }
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        var mask = 0
        if (hatX < -0.5f) mask = mask or BTN_LEFT
        if (hatX >  0.5f) mask = mask or BTN_RIGHT
        if (hatY < -0.5f) mask = mask or BTN_UP
        if (hatY >  0.5f) mask = mask or BTN_DOWN
        hatBits = mask
        push()
        return true
    }

    /** Clear all button state (call when the activity loses focus). */
    fun reset() {
        keyBits = 0
        hatBits = 0
        push()
    }

    private fun push() {
        core.setInputState(1, keyBits or hatBits)
    }

    private fun keycodeToBit(keyCode: Int): Int? = when (keyCode) {
        // Face buttons — Android uses Xbox layout; SNES uses Nintendo layout.
        // Android A (bottom) → SNES B, Android B (right) → SNES A,
        // Android X (left)   → SNES Y, Android Y (top)   → SNES X.
        KeyEvent.KEYCODE_BUTTON_A -> BTN_B
        KeyEvent.KEYCODE_BUTTON_B -> BTN_A
        KeyEvent.KEYCODE_BUTTON_X -> BTN_Y
        KeyEvent.KEYCODE_BUTTON_Y -> BTN_X
        // Shoulder buttons
        KeyEvent.KEYCODE_BUTTON_L1 -> BTN_L
        KeyEvent.KEYCODE_BUTTON_R1 -> BTN_R
        // Menu buttons
        KeyEvent.KEYCODE_BUTTON_START  -> BTN_START
        KeyEvent.KEYCODE_BUTTON_SELECT -> BTN_SELECT
        // D-pad delivered as key events by some controllers
        KeyEvent.KEYCODE_DPAD_UP    -> BTN_UP
        KeyEvent.KEYCODE_DPAD_DOWN  -> BTN_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT  -> BTN_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> BTN_RIGHT
        else -> null
    }
}
