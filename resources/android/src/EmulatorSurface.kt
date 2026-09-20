package com.kevinbatdorf.plugins.retroemulator

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.fragment.app.FragmentActivity
import com.nativephp.mobile.ui.nativerender.NativeUINode

/**
 * Compose entry point for the `emulator` EDGE node — referenced by the
 * generated PluginRendererRegistration.
 *
 * Host-only: depends on Compose and the host app's NativePHP types, so the
 * plugin's own test app excludes it from compilation.
 */
object EmulatorSurface {

    @Composable
    fun Render(node: NativeUINode, modifier: Modifier) {
        val name = node.props.getString("name", "main")
        val zIndex = node.props.getInt("z_index", 0)
        val inputCapture = node.props.getString("input_capture", "focus")

        // Declarative setup (optional) — native boots the system/rom on first
        // mount and re-stages only when these change, so a screen can run with
        // no imperative PHP. config is a JSON string (the wire carries no map).
        val system = node.props.getString("system", "")
        val config = node.props.getString("config", "")
        val rom = node.props.getString("rom", "")

        AndroidView(
            modifier = modifier,
            factory = { context ->
                val activity = context as? FragmentActivity
                val session = activity?.let { EmulatorFunctions.sessionFor(name, it) } ?: EmulatorSession(context)

                EmulatorRenderer(context, session).also { view ->
                    // A SurfaceView can't freely interleave with siblings —
                    // z_index snaps to the nearest SurfaceView Z flag.
                    if (zIndex in 1..99) view.setZOrderMediaOverlay(true)
                    if (zIndex >= 100) view.setZOrderOnTop(true)

                    if (activity != null && inputCapture == "global") {
                        // Route the hardware pad to this session at the window,
                        // so it drives the game no matter what the host has
                        // focused. Touch/UI still reach the app.
                        val window = activity.window
                        val original = window.callback
                        window.callback = GamepadCapture(original, session.input)
                        view.windowCaptureRestore = {
                            if (window.callback is GamepadCapture) window.callback = original
                        }
                    }

                    // Focus mode (default): gamepad events go to the focused
                    // view, so claim focus to work without host-side wiring.
                    view.requestFocus()
                }
            },
            update = { view ->
                // Runs after factory and on every recomposition.
                val bootKey = listOf(system, config, rom).joinToString(" ")
                if (system.isNotEmpty() && view.session.declaredBootKey != bootKey) {
                    view.session.declaredBootKey = bootKey
                    EmulatorFunctions.applyDeclarativeSetup(name, system, config, rom)
                }
            },
            onRelease = { view ->
                // The surface detaches through the holder callback; the session stays.
                view.windowCaptureRestore?.invoke()
            },
        )
    }
}
