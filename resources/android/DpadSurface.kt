package com.kevinbatdorf.plugins.retroemulator

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.util.fastFirstOrNull
import com.nativephp.mobile.ui.nativerender.NativeUINode

/**
 * Compose entry point for the `dpad` EDGE node — an on-screen directional pad
 * that writes straight into the emulator core.
 *
 * PHP is not in the input loop: the node resolves a finger position into
 * directions itself (see [DpadResolver]) and calls the emulator renderer's
 * button path, so a press costs no bridge round-trip and no recomposition of
 * the app's tree. The pressed highlight is drawn here for the same reason.
 *
 * Host-only: depends on Compose and the host app's NativePHP types, so the
 * plugin's own test app excludes it from compilation.
 */
object DpadSurface {

    private val DIRECTIONS_BY_BUTTON = DpadDirection.values().associateBy { it.button }

    @Composable
    fun Render(node: NativeUINode, modifier: Modifier) {
        val surface = node.props.getString("surface", "main")
        val port = node.props.getInt("port", 1)
        val threshold = node.props.getFloat("threshold", DpadResolver.DEFAULT_THRESHOLD)
        val diagonalRatio =
            node.props.getFloat("diagonal_ratio", DpadResolver.DEFAULT_DIAGONAL_RATIO)
        val baseColor = Color(node.props.getColor("color", 0x66FFFFFF))
        val activeColor = Color(node.props.getColor("active_color", 0xE6FFFFFF.toInt()))

        var held by remember { mutableStateOf(emptySet<DpadDirection>()) }
        var fromCore by remember { mutableStateOf(emptySet<DpadDirection>()) }

        // A press outlives the composition it started in (navigation, rotation),
        // and the core would hold that button forever.
        DisposableEffect(surface, port) {
            onDispose {
                apply(surface, port, emptySet(), held)
                held = emptySet()
            }
        }

        // Mirror what the core is actually holding so a hardware pad lights the
        // on-screen one too. Two atomic loads per frame, and it skips the
        // recomposition unless the set changed.
        LaunchedEffect(surface, port) {
            while (true) {
                withFrameNanos { }
                val renderer = EmulatorFunctions.rendererFor(surface)
                val next = renderer?.pressedButtons(port)
                    ?.split(',')
                    ?.mapNotNull { name -> DIRECTIONS_BY_BUTTON[name] }
                    ?.toSet()
                    ?: emptySet()
                if (next != fromCore) fromCore = next
            }
        }

        Canvas(
            modifier = modifier.pointerInput(surface, port, threshold, diagonalRatio) {
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)

                    fun resolveAt(x: Float, y: Float) = DpadResolver.resolve(
                        nx = normalize(x, size.width),
                        ny = normalize(y, size.height),
                        threshold = threshold,
                        diagonalRatio = diagonalRatio,
                    )

                    fun push(next: Set<DpadDirection>) {
                        if (next == held) return
                        apply(surface, port, next, held)
                        held = next
                    }

                    push(resolveAt(down.position.x, down.position.y))

                    // Holding the gesture past the pad's edge is what keeps a
                    // direction from dropping mid-jump.
                    while (true) {
                        val change = awaitPointerEvent().changes
                            .fastFirstOrNull { it.id == down.id }
                        if (change == null || !change.pressed) break
                        push(resolveAt(change.position.x, change.position.y))
                    }

                    push(emptySet())
                }
            },
        ) {
            drawPad(held + fromCore, baseColor, activeColor)
        }
    }

    /** Offset from the pad's centre, in units of half its width/height. */
    private fun normalize(value: Float, extent: Int): Float {
        if (extent <= 0) return 0f
        val half = extent / 2f

        return (value - half) / half
    }

    /**
     * Push a direction change into the core, pressing what is newly held before
     * releasing what was dropped. A poll landing mid-change then sees a
     * momentary diagonal rather than a momentary neutral — a dropped direction
     * reads as a missed input, an extra frame of diagonal does not.
     */
    private fun apply(
        surface: String,
        port: Int,
        next: Set<DpadDirection>,
        previous: Set<DpadDirection>,
    ) {
        val renderer = EmulatorFunctions.rendererFor(surface) ?: return
        for (direction in next - previous) renderer.pressButton(port, direction.button, true)
        for (direction in previous - next) renderer.pressButton(port, direction.button, false)
    }

    private fun DrawScope.drawPad(
        held: Set<DpadDirection>,
        baseColor: Color,
        activeColor: Color,
    ) {
        val arm = minOf(size.width, size.height) * 0.36f
        val radius = CornerRadius(arm * 0.28f, arm * 0.28f)
        val centerX = size.width / 2f
        val centerY = size.height / 2f
        val armLength = minOf(centerX, centerY) - arm / 2f

        // The cross as one path, so the arms share a single silhouette.
        val cross = Path().apply {
            addRoundRect(
                RoundRect(
                    left = centerX - arm / 2f, top = centerY - arm / 2f - armLength,
                    right = centerX + arm / 2f, bottom = centerY + arm / 2f + armLength,
                    cornerRadius = radius,
                ),
            )
            addRoundRect(
                RoundRect(
                    left = centerX - arm / 2f - armLength, top = centerY - arm / 2f,
                    right = centerX + arm / 2f + armLength, bottom = centerY + arm / 2f,
                    cornerRadius = radius,
                ),
            )
        }
        drawPath(cross, baseColor)

        for (direction in held) {
            val (offset, armSize) = when (direction) {
                DpadDirection.Up ->
                    Offset(centerX - arm / 2f, centerY - arm / 2f - armLength) to Size(arm, armLength)
                DpadDirection.Down ->
                    Offset(centerX - arm / 2f, centerY + arm / 2f) to Size(arm, armLength)
                DpadDirection.Left ->
                    Offset(centerX - arm / 2f - armLength, centerY - arm / 2f) to Size(armLength, arm)
                DpadDirection.Right ->
                    Offset(centerX + arm / 2f, centerY - arm / 2f) to Size(armLength, arm)
            }
            drawPath(armPath(direction, offset, armSize, radius), activeColor)
        }
    }

    /**
     * An arm's highlight, rounded on its outer tip and square where it meets the
     * hub. Rounding all four corners detaches the lit arm from the cross.
     */
    private fun armPath(
        direction: DpadDirection,
        offset: Offset,
        size: Size,
        radius: CornerRadius,
    ): Path {
        val zero = CornerRadius.Zero
        val rect = RoundRect(
            left = offset.x,
            top = offset.y,
            right = offset.x + size.width,
            bottom = offset.y + size.height,
            topLeftCornerRadius = if (direction == DpadDirection.Down || direction == DpadDirection.Right) zero else radius,
            topRightCornerRadius = if (direction == DpadDirection.Down || direction == DpadDirection.Left) zero else radius,
            bottomRightCornerRadius = if (direction == DpadDirection.Up || direction == DpadDirection.Left) zero else radius,
            bottomLeftCornerRadius = if (direction == DpadDirection.Up || direction == DpadDirection.Right) zero else radius,
        )

        return Path().apply { addRoundRect(rect) }
    }
}
