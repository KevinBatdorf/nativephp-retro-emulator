package com.kevinbatdorf.plugins.retroemulator

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.fragment.app.FragmentActivity
import com.nativephp.mobile.ui.nativerender.NativeUINode

/**
 * Compose entry point for the `emulator` EDGE node — referenced by the
 * generated PluginRendererRegistration. Wraps the classic GLSurfaceView
 * renderer in AndroidView and registers it under the node's surface name so
 * Emulator.* bridge calls resolve to this instance.
 *
 * Host-only: depends on Compose and the host app's NativePHP types, so the
 * plugin's own test app excludes it from compilation.
 */
object EmulatorSurface {

    @Composable
    fun Render(node: NativeUINode, modifier: Modifier) {
        val name = node.props.getString("name", "main")
        val zIndex = node.props.getInt("z_index", 0)

        AndroidView(
            modifier = modifier,
            factory = { context ->
                EmulatorRenderer(context).also { renderer ->
                    // A SurfaceView can't freely interleave with siblings —
                    // z_index snaps to the nearest SurfaceView Z flag.
                    if (zIndex in 1..99) renderer.setZOrderMediaOverlay(true)
                    if (zIndex >= 100) renderer.setZOrderOnTop(true)

                    (context as? FragmentActivity)?.let { activity ->
                        EmulatorFunctions.registerSurface(name, renderer, activity)
                    }
                }
            },
            onRelease = { renderer ->
                EmulatorFunctions.unregisterSurface(name)
                renderer.release()
            },
        )
    }
}
