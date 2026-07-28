package com.kevinbatdorf.plugins.retroemulator

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot

/** One of the four d-pad directions, named as the cores' button nodes are. */
enum class DpadDirection(val button: String) {
    Up("Up"),
    Down("Down"),
    Left("Left"),
    Right("Right"),
}

/**
 * Turns a finger position into the 1–2 directions a physical d-pad would report.
 *
 * A d-pad is one input area doing continuous position→direction resolution, not
 * four independent buttons: four buttons can never report a diagonal, and every
 * shipped overlay (RetroArch's OVERLAY_TYPE_DPAD_AREA, Dolphin's single d-pad
 * drawable, Lemuroid's CrossDial) resolves one position instead.
 *
 * Positions are normalized offsets from the pad centre, +y pointing down the
 * screen, and are deliberately *not* clamped: a finger that slides past the
 * pad's edge keeps holding its direction rather than dropping input mid-jump.
 */
object DpadResolver {

    /** Below this distance from centre the pad reads neutral. */
    const val DEFAULT_DEAD_ZONE = 0.1f

    /**
     * How much harder a diagonal is to hit than a cardinal. 1.0 gives all eight
     * anchors an equal 45° slice; above 1.0 narrows the diagonals, so aiming
     * straight up doesn't creep into Up+Right on a shaky thumb.
     */
    const val DEFAULT_DIAGONAL_STRENGTH = 1.25f

    /** Anchors counter-clockwise from Right, alternating cardinal / diagonal. */
    private val ANCHORS: List<Set<DpadDirection>> = listOf(
        setOf(DpadDirection.Right),
        setOf(DpadDirection.Up, DpadDirection.Right),
        setOf(DpadDirection.Up),
        setOf(DpadDirection.Up, DpadDirection.Left),
        setOf(DpadDirection.Left),
        setOf(DpadDirection.Down, DpadDirection.Left),
        setOf(DpadDirection.Down),
        setOf(DpadDirection.Down, DpadDirection.Right),
    )

    fun resolve(
        nx: Float,
        ny: Float,
        deadZone: Float = DEFAULT_DEAD_ZONE,
        diagonalStrength: Float = DEFAULT_DIAGONAL_STRENGTH,
    ): Set<DpadDirection> {
        if (hypot(nx, ny) < deadZone) return emptySet()

        // Screen y grows downward; flip it so a positive angle means up.
        val angle = atan2(-ny.toDouble(), nx.toDouble())
        val step = PI / 4

        var best = 0
        var bestScore = Double.MAX_VALUE
        for (i in ANCHORS.indices) {
            val spread = abs(angleDelta(angle, i * step))
            val score = if (i % 2 == 1) spread * diagonalStrength else spread
            if (score < bestScore) {
                bestScore = score
                best = i
            }
        }
        return ANCHORS[best]
    }

    /** Shortest signed distance between two angles, in (-PI, PI]. */
    private fun angleDelta(a: Double, b: Double): Double {
        var d = (a - b) % (2 * PI)
        if (d > PI) d -= 2 * PI
        if (d < -PI) d += 2 * PI
        return d
    }
}
