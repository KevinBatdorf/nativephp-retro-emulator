package com.kevinbatdorf.plugins.retroemulator

import android.content.Context
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView

/** A window onto an [EmulatorSession]; owns nothing, so the game survives this view. */
class EmulatorRenderer(context: Context, val session: EmulatorSession) : SurfaceView(context), SurfaceHolder.Callback {

    val input: EmulatorInput get() = session.input

    // Set when this view installs a global gamepad capturer on its host window
    // (input-capture="global"); invoked on release to restore the original callback.
    var windowCaptureRestore: (() -> Unit)? = null

    init {
        // Hardware gamepads deliver key/motion events to the focused view.
        // In an EDGE host nothing else routes them here.
        isFocusable = true
        isFocusableInTouchMode = true
        holder.addCallback(this)
        // Gamepad input doesn't reset Android's idle timer — keep the display
        // awake while the emulator view is showing.
        keepScreenOn = true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        input.onKeyEvent(event) || super.onKeyDown(keyCode, event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        input.onKeyEvent(event) || super.onKeyUp(keyCode, event)

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        input.onMotionEvent(event) || super.onGenericMotionEvent(event)

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        // Buttons held across a focus loss would otherwise stay pressed forever.
        if (!hasWindowFocus) input.reset()
    }

    override fun surfaceCreated(holder: SurfaceHolder) = session.attachSurface(holder.surface)

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) =
        session.surfaceChanged(holder.surface, width, height)

    override fun surfaceDestroyed(holder: SurfaceHolder) = session.detachSurface(holder.surface)
}
