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
import com.nativephp.mobile.ui.nativerender.NativeElementBridge
import com.nativephp.mobile.ui.nativerender.NativeUINode
import com.nativephp.mobile.ui.nativerender.SharedValueStore

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

    // Feel defaults live in DpadResolver as fractions; expose them as the
    // percentages the props use so the two can't drift apart.
    private const val DEFAULT_THRESHOLD_PERCENT = DpadResolver.DEFAULT_THRESHOLD * 100f
    private const val DEFAULT_DIAGONAL_RATIO_PERCENT = DpadResolver.DEFAULT_DIAGONAL_RATIO * 100f

    /** Arm width as a share of the pad's shorter side. */
    private const val DEFAULT_THICKNESS_PERCENT = 36f

    /** Corner rounding as a share of the arm's width; 50 is a fully round tip. */
    private const val DEFAULT_RADIUS_PERCENT = 28f

    /** Travel for a bound pan value, in dp per second. */
    private const val DEFAULT_PAN_SPEED_DP = 260f

    @Composable
    fun Render(node: NativeUINode, modifier: Modifier) {
        val surface = node.props.getString("surface", "main")
        val port = node.props.getInt("port", 1)
        // Props are whole percentages; the resolver and drawing want fractions.
        val threshold = node.props.getFloat("threshold", DEFAULT_THRESHOLD_PERCENT) / 100f
        val diagonalRatio =
            node.props.getFloat("diagonal_ratio", DEFAULT_DIAGONAL_RATIO_PERCENT) / 100f
        val thickness = node.props.getFloat("thickness", DEFAULT_THICKNESS_PERCENT) / 100f
        val radius = node.props.getFloat("radius", DEFAULT_RADIUS_PERCENT) / 100f
        // 0 when no handler is bound, which keeps PHP out of the press path.
        val onChange = node.props.getCallbackId("on_change")
        // 0 unless a SharedValue is bound; no frame loop starts otherwise.
        val panXId = node.props.getInt("pan_x_id", 0)
        val panYId = node.props.getInt("pan_y_id", 0)
        val panSpeed = node.props.getFloat("pan_speed", DEFAULT_PAN_SPEED_DP)
        val panXMin = node.props.getFloat("pan_x_min", Float.NEGATIVE_INFINITY)
        val panXMax = node.props.getFloat("pan_x_max", Float.POSITIVE_INFINITY)
        val panYMin = node.props.getFloat("pan_y_min", Float.NEGATIVE_INFINITY)
        val panYMax = node.props.getFloat("pan_y_max", Float.POSITIVE_INFINITY)
        val baseColor = Color(node.props.getColor("color", 0x66FFFFFF))
        val activeColor = Color(node.props.getColor("active_color", 0xE6FFFFFF.toInt()))

        var held by remember { mutableStateOf(emptySet<DpadDirection>()) }

        // A press outlives the composition it started in (navigation, rotation),
        // and the core would hold that button forever.
        DisposableEffect(surface, port) {
            onDispose {
                apply(surface, port, emptySet(), held)
                held = emptySet()
            }
        }

        if (panXId != 0 || panYId != 0) {
            LaunchedEffect(panXId, panYId, panSpeed, panXMax, panYMax) {
                SharedValueStore.seed(panXId, node.props.getFloat("pan_x_initial", 0f))
                SharedValueStore.seed(panYId, node.props.getFloat("pan_y_initial", 0f))
                var last = withFrameNanos { it }
                while (true) {
                    val now = withFrameNanos { it }
                    val seconds = (now - last) / 1_000_000_000f
                    last = now
                    if (held.isEmpty()) continue

                    val step = panSpeed * seconds
                    val dx = (if (DpadDirection.Right in held) step else 0f) -
                        (if (DpadDirection.Left in held) step else 0f)
                    val dy = (if (DpadDirection.Down in held) step else 0f) -
                        (if (DpadDirection.Up in held) step else 0f)
                    if (panXId != 0 && dx != 0f) {
                        val next = SharedValueStore.valueOf(panXId) + dx
                        SharedValueStore.set(panXId, next.coerceIn(panXMin, panXMax))
                    }
                    if (panYId != 0 && dy != 0f) {
                        val next = SharedValueStore.valueOf(panYId) + dy
                        SharedValueStore.set(panYId, next.coerceIn(panYMin, panYMax))
                    }
                }
            }
        }

        Canvas(
            modifier = modifier.pointerInput(surface, port, threshold, diagonalRatio) {
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    // Claim the gesture so an ancestor scroller doesn't pan the
                    // page while a thumb is steering.
                    down.consume()

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
                        if (onChange != 0) {
                            NativeElementBridge.sendTextChangeEvent(
                                onChange, node.id, next.joinToString(",") { it.button })
                        }
                    }

                    push(resolveAt(down.position.x, down.position.y))

                    // Holding the gesture past the pad's edge is what keeps a
                    // direction from dropping mid-jump.
                    while (true) {
                        val change = awaitPointerEvent().changes
                            .fastFirstOrNull { it.id == down.id }
                        if (change == null || !change.pressed) break
                        change.consume()
                        push(resolveAt(change.position.x, change.position.y))
                    }

                    push(emptySet())
                }
            },
        ) {
            drawPad(held, baseColor, activeColor, thickness, radius)
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
        thickness: Float,
        radiusShare: Float,
    ) {
        val arm = minOf(size.width, size.height) * thickness
        val radius = CornerRadius(arm * radiusShare, arm * radiusShare)
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
