package com.kevinbatdorf.plugins.retroemulator

import kotlin.math.abs

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
 * One input area, not four buttons, which could never report a diagonal.
 *
 * Axes are tested independently, as Dolphin's overlay does (InputOverlay.kt:
 * `bounds.top + height / 3 > y` and its three siblings). Choosing one zone by
 * angle instead — RetroArch's `abs_y > slope_high * abs_x`, Lemuroid's anchor
 * distances — makes a second direction cost rotation, so a thumb already deep
 * on an arm must travel further to catch a turn.
 *
 * Positions are normalized offsets from the pad centre, +y pointing down the
 * screen, and are deliberately *not* clamped: a finger that slides past the
 * pad's edge keeps holding its direction rather than dropping input mid-jump.
 */
object DpadResolver {

    /**
     * How far off centre an axis must travel before its direction engages, as a
     * fraction of the pad's half-extent. Doubles as the dead zone — a finger in
     * the centre square crosses nothing and reads neutral. Dolphin's outer
     * thirds put this at 1/3.
     */
    const val DEFAULT_THRESHOLD = 0.33f

    /**
     * How hard the weaker axis must compete before a diagonal forms: it is
     * dropped while its offset is under this fraction of the stronger axis. 0
     * gives free diagonals the instant both axes cross (Dolphin); raising it
     * keeps cardinals clean when a thumb drifts, the job RetroArch's
     * diagonal-sensitivity slopes do. 1 would demand a perfect 45°.
     */
    const val DEFAULT_DIAGONAL_RATIO = 0f

    fun resolve(
        nx: Float,
        ny: Float,
        threshold: Float = DEFAULT_THRESHOLD,
        diagonalRatio: Float = DEFAULT_DIAGONAL_RATIO,
        diagonals: Boolean = true,
    ): Set<DpadDirection> {
        val ax = abs(nx)
        val ay = abs(ny)

        var horizontal = ax > threshold
        var vertical = ay > threshold

        if (horizontal && vertical) {
            if (!diagonals) {
                // Snap to whichever axis is further out. A ratio can only make a
                // diagonal unlikely; a 4-way game needs it impossible.
                if (ay > ax) horizontal = false else vertical = false
            } else if (diagonalRatio > 0f) {
                if (ay < diagonalRatio * ax) vertical = false
                else if (ax < diagonalRatio * ay) horizontal = false
            }
        }

        val directions = mutableSetOf<DpadDirection>()
        if (vertical) directions.add(if (ny < 0f) DpadDirection.Up else DpadDirection.Down)
        if (horizontal) directions.add(if (nx < 0f) DpadDirection.Left else DpadDirection.Right)

        return directions
    }
}
