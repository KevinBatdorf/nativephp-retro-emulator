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
/// A d-pad is one input area doing continuous position→direction resolution, not
/// four independent buttons: four buttons can never report a diagonal, and every
/// shipped overlay (RetroArch's OVERLAY_TYPE_DPAD_AREA, Dolphin's single d-pad
/// drawable, Lemuroid's CrossDial) resolves one position instead.
///
/// Positions are normalized offsets from the pad centre, +y pointing down the
/// screen, and are deliberately *not* clamped: a finger that slides past the
/// pad's edge keeps holding its direction rather than dropping input mid-jump.
///
/// Kept in lockstep with Android's `DpadResolver.kt` — the same anchors, the same
/// weighting — so the pad feels identical on both platforms.
enum DpadResolver {

    /// Below this distance from centre the pad reads neutral.
    static let defaultDeadZone: Float = 0.1

    /// How much harder a diagonal is to hit than a cardinal. 1.0 gives all eight
    /// anchors an equal 45° slice; above 1.0 narrows the diagonals, so aiming
    /// straight up doesn't creep into Up+Right on a shaky thumb.
    static let defaultDiagonalStrength: Float = 1.25

    /// Anchors counter-clockwise from Right, alternating cardinal / diagonal.
    private static let anchors: [Set<DpadDirection>] = [
        [.right],
        [.up, .right],
        [.up],
        [.up, .left],
        [.left],
        [.down, .left],
        [.down],
        [.down, .right],
    ]

    static func resolve(
        nx: Float,
        ny: Float,
        deadZone: Float = defaultDeadZone,
        diagonalStrength: Float = defaultDiagonalStrength
    ) -> Set<DpadDirection> {
        if hypot(nx, ny) < deadZone { return [] }

        // Screen y grows downward; flip it so a positive angle means up.
        let angle = atan2(Double(-ny), Double(nx))
        let step = Double.pi / 4

        var best = 0
        var bestScore = Double.greatestFiniteMagnitude
        for i in anchors.indices {
            let spread = abs(angleDelta(angle, Double(i) * step))
            let score = i % 2 == 1 ? spread * Double(diagonalStrength) : spread
            if score < bestScore {
                bestScore = score
                best = i
            }
        }

        return anchors[best]
    }

    /// Shortest signed distance between two angles, in (-pi, pi].
    private static func angleDelta(_ a: Double, _ b: Double) -> Double {
        var d = (a - b).truncatingRemainder(dividingBy: 2 * .pi)
        if d > .pi { d -= 2 * .pi }
        if d < -.pi { d += 2 * .pi }

        return d
    }
}
