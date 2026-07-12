package com.kevinbatdorf.plugins.retroemulator

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Left analog stick → d-pad mapping (audit item 5).
 *
 * Threshold semantics ported from ares desktop-ui InputDigital::value()
 * (input.cpp:190-193): an axis bound to a digital input reads pressed past
 * half deflection, strict comparison. Assertions read the mask back through
 * the JNI seam (nativeGetInputState) — the exact value Platform::input()
 * will see on its next poll.
 */
@RunWith(AndroidJUnit4::class)
class StickInputTest {

    private lateinit var core: AresCore
    private lateinit var input: EmulatorInput

    @Before
    fun setUp() {
        core = AresCore()
        assertTrue(core.init())
        input = EmulatorInput(core)
    }

    @After
    fun tearDown() {
        core.destroy()
    }

    private fun joystickEvent(
        x: Float = 0f, y: Float = 0f,
        hatX: Float = 0f, hatY: Float = 0f,
    ): MotionEvent {
        val props = arrayOf(MotionEvent.PointerProperties().apply { id = 0 })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            setAxisValue(MotionEvent.AXIS_X, x)
            setAxisValue(MotionEvent.AXIS_Y, y)
            setAxisValue(MotionEvent.AXIS_HAT_X, hatX)
            setAxisValue(MotionEvent.AXIS_HAT_Y, hatY)
        })
        return MotionEvent.obtain(
            0L, 0L, MotionEvent.ACTION_MOVE, 1, props, coords,
            0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_JOYSTICK, 0,
        )
    }

    private fun dispatch(event: MotionEvent) {
        assertTrue(input.onMotionEvent(event))
        event.recycle()
    }

    @Test
    fun stickPastThresholdSetsDpadBit() {
        dispatch(joystickEvent(x = 0.6f))
        assertEquals(EmulatorInput.BTN_RIGHT, core.getInputState(1))

        dispatch(joystickEvent(x = -0.6f))
        assertEquals(EmulatorInput.BTN_LEFT, core.getInputState(1))

        dispatch(joystickEvent(y = -0.6f))
        assertEquals(EmulatorInput.BTN_UP, core.getInputState(1))

        dispatch(joystickEvent(y = 0.6f))
        assertEquals(EmulatorInput.BTN_DOWN, core.getInputState(1))
    }

    @Test
    fun stickInsideDeadzoneSetsNothing() {
        dispatch(joystickEvent(x = 0.4f, y = -0.4f))
        assertEquals(0, core.getInputState(1))
    }

    @Test
    fun exactlyHalfDeflectionIsNotPressed() {
        // ares InputDigital uses strict comparison: +16384 itself is not "Hi".
        dispatch(joystickEvent(x = 0.5f, y = 0.5f))
        assertEquals(0, core.getInputState(1))
    }

    @Test
    fun diagonalSetsBothBits() {
        dispatch(joystickEvent(x = -0.75f, y = -0.75f))
        assertEquals(EmulatorInput.BTN_LEFT or EmulatorInput.BTN_UP, core.getInputState(1))
    }

    @Test
    fun returnToCenterClearsStickBits() {
        dispatch(joystickEvent(x = 1.0f))
        assertEquals(EmulatorInput.BTN_RIGHT, core.getInputState(1))

        dispatch(joystickEvent())
        assertEquals(0, core.getInputState(1))
    }

    @Test
    fun hatAndStickMergeLikeIndependentBindings() {
        // ares ORs every binding of a digital input (InputDigital::value()
        // result |= output) — hat right + stick left both read pressed.
        dispatch(joystickEvent(x = -1.0f, hatX = 1.0f))
        assertEquals(EmulatorInput.BTN_LEFT or EmulatorInput.BTN_RIGHT, core.getInputState(1))
    }

    @Test
    fun stickDoesNotClobberKeyBits() {
        assertTrue(input.onKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)))
        dispatch(joystickEvent(x = 0.9f))
        assertEquals(EmulatorInput.BTN_B or EmulatorInput.BTN_RIGHT, core.getInputState(1))

        dispatch(joystickEvent())
        assertEquals(EmulatorInput.BTN_B, core.getInputState(1))
    }
}
