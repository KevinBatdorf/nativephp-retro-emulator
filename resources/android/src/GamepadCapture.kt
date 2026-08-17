package com.kevinbatdorf.plugins.retroemulator

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Window

/**
 * Wraps the host window's [Window.Callback] so hardware gamepad/joystick events
 * reach [input] regardless of which view holds focus — the input-capture="global"
 * mode. Everything else (touch, keyboard, the host UI's own focus navigation) is
 * delegated untouched, so the app's on-screen controls keep working; only the
 * pad is diverted to the game. Android otherwise routes gamepad keys to the
 * focused view, which any on-screen control steals.
 *
 * Kotlin interface delegation (`by delegate`) forwards every Window.Callback
 * method we don't override straight to the original callback. Restore [delegate]
 * as the window callback on teardown.
 */
class GamepadCapture(
    private val delegate: Window.Callback,
    private val input: EmulatorInput,
) : Window.Callback by delegate {

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (isGamepad(event.source) && input.onKeyEvent(event)) return true
        return delegate.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (isGamepad(event.source) && input.onMotionEvent(event)) return true
        return delegate.dispatchGenericMotionEvent(event)
    }

    private fun isGamepad(source: Int): Boolean =
        source and InputDevice.SOURCE_GAMEPAD != 0 ||
        source and InputDevice.SOURCE_JOYSTICK != 0
}
