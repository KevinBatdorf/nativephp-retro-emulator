import Foundation

/// One of the four d-pad directions, named as the cores' button nodes are.
enum DpadDirection: String, CaseIterable {
    case up = "Up"
    case down = "Down"
    case left = "Left"
    case right = "Right"
}

/// Turns a finger position into the 1–2 directions a physical d-pad would report.
///
/// One input area, not four buttons, which could never report a diagonal.
///
/// Axes are tested independently, as Dolphin's overlay does (InputOverlay.kt:
/// `bounds.top + height / 3 > y` and its three siblings). Choosing one zone by
/// angle instead — RetroArch's `abs_y > slope_high * abs_x`, Lemuroid's anchor
/// distances — makes a second direction cost rotation, so a thumb already deep
/// on an arm must travel further to catch a turn.
///
/// Positions are normalized offsets from the pad centre, +y pointing down the
/// screen, and are deliberately *not* clamped: a finger that slides past the
/// pad's edge keeps holding its direction rather than dropping input mid-jump.
///
/// Kept in lockstep with Android's `DpadResolver.kt` so the pad feels identical
/// on both platforms.
enum DpadResolver {

    /// How far off centre an axis must travel before its direction engages, as a
    /// fraction of the pad's half-extent. Doubles as the dead zone — a finger in
    /// the centre square crosses nothing and reads neutral. Dolphin's outer
    /// thirds put this at 1/3.
    static let defaultThreshold: Float = 0.33

    /// How hard the weaker axis must compete before a diagonal forms: it is
    /// dropped while its offset is under this fraction of the stronger axis. 0
    /// gives free diagonals the instant both axes cross (Dolphin); raising it
    /// keeps cardinals clean when a thumb drifts, the job RetroArch's
    /// diagonal-sensitivity slopes do. 1 would demand a perfect 45°.
    static let defaultDiagonalRatio: Float = 0

    static func resolve(
        nx: Float,
        ny: Float,
        threshold: Float = defaultThreshold,
        diagonalRatio: Float = defaultDiagonalRatio,
        diagonals: Bool = true
    ) -> Set<DpadDirection> {
        let ax = abs(nx)
        let ay = abs(ny)

        var horizontal = ax > threshold
        var vertical = ay > threshold

        if horizontal, vertical {
            if !diagonals {
                if ay > ax { horizontal = false } else { vertical = false }
            } else if diagonalRatio > 0 {
                if ay < diagonalRatio * ax {
                    vertical = false
                } else if ax < diagonalRatio * ay {
                    horizontal = false
                }
            }
        }

        var directions: Set<DpadDirection> = []
        if vertical { directions.insert(ny < 0 ? .up : .down) }
        if horizontal { directions.insert(nx < 0 ? .left : .right) }

        return directions
    }
}
