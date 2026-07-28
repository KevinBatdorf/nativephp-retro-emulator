import SwiftUI

/// iOS EDGE entry point for `<native:dpad>` — the SwiftUI counterpart to
/// Android's `DpadSurface`. An on-screen directional pad that writes straight
/// into the emulator core.
///
/// PHP is not in the input loop: the view resolves a finger position into
/// directions itself (see `DpadResolver`) and calls the emulator renderer's
/// button path, so a press costs no bridge round-trip and no re-render of the
/// app's tree. The pressed highlight is drawn here for the same reason.
struct DpadSurfaceView: View {
    let node: NativeUINode

    var body: some View {
        DpadPad(
            surface: node.props.getString("surface", default: "main"),
            port: node.props.getInt("port", default: 1),
            deadZone: node.props.getFloat("dead_zone", default: DpadResolver.defaultDeadZone),
            diagonalStrength: node.props.getFloat(
                "diagonal_strength", default: DpadResolver.defaultDiagonalStrength),
            baseColor: Color(argb: node.props.getColor("color", default: 0x66FF_FFFF)),
            activeColor: Color(argb: node.props.getColor("active_color", default: 0xE6FF_FFFF))
        )
    }
}

private struct DpadPad: View {
    let surface: String
    let port: Int
    let deadZone: Float
    let diagonalStrength: Float
    let baseColor: Color
    let activeColor: Color

    @State private var held: Set<DpadDirection> = []

    var body: some View {
        GeometryReader { geo in
            let arm = min(geo.size.width, geo.size.height) * 0.36
            let armLength = min(geo.size.width, geo.size.height) / 2 - arm / 2
            let span = arm + armLength * 2
            let radius = arm * 0.28

            ZStack {
                RoundedRectangle(cornerRadius: radius)
                    .fill(baseColor)
                    .frame(width: arm, height: span)
                RoundedRectangle(cornerRadius: radius)
                    .fill(baseColor)
                    .frame(width: span, height: arm)

                ForEach(Array(held), id: \.self) { direction in
                    highlight(direction, arm: arm, armLength: armLength, radius: radius)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            // Without this the gaps between the arms aren't hittable, and a
            // thumb landing on a corner reads as a miss.
            .contentShape(Rectangle())
            .gesture(
                // minimumDistance 0 so the pad answers the touch down, not the
                // first drag past a threshold.
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        push(DpadResolver.resolve(
                            nx: normalize(Float(value.location.x), Float(geo.size.width)),
                            ny: normalize(Float(value.location.y), Float(geo.size.height)),
                            deadZone: deadZone,
                            diagonalStrength: diagonalStrength
                        ))
                    }
                    .onEnded { _ in push([]) }
            )
        }
        // A press outlives the view that started it (navigation, rotation), and
        // the core would hold that button forever.
        .onDisappear { push([]) }
    }

    @ViewBuilder
    private func highlight(
        _ direction: DpadDirection, arm: CGFloat, armLength: CGFloat, radius: CGFloat
    ) -> some View {
        let isVertical = direction == .up || direction == .down
        let offset = (arm + armLength) / 2

        RoundedRectangle(cornerRadius: radius)
            .fill(activeColor)
            .frame(
                width: isVertical ? arm : armLength,
                height: isVertical ? armLength : arm
            )
            .offset(
                x: direction == .left ? -offset : (direction == .right ? offset : 0),
                y: direction == .up ? -offset : (direction == .down ? offset : 0)
            )
    }

    /// Offset from the pad's centre, in units of half its width/height.
    private func normalize(_ value: Float, _ extent: Float) -> Float {
        guard extent > 0 else { return 0 }
        let half = extent / 2

        return (value - half) / half
    }

    /// Push a direction change into the core, pressing what is newly held before
    /// releasing what was dropped. A poll landing mid-change then sees a
    /// momentary diagonal rather than a momentary neutral — a dropped direction
    /// reads as a missed input, an extra frame of diagonal does not.
    private func push(_ next: Set<DpadDirection>) {
        guard next != held else { return }
        defer { held = next }

        guard let renderer = EmulatorFunctions.renderer(named: surface) else { return }
        for direction in next.subtracting(held) {
            _ = renderer.pressButton(port: port, name: direction.rawValue, down: true)
        }
        for direction in held.subtracting(next) {
            _ = renderer.pressButton(port: port, name: direction.rawValue, down: false)
        }
    }
}

private extension Color {
    /// EDGE colors cross the wire as packed ARGB ints.
    init(argb: Int) {
        self.init(
            .sRGB,
            red: Double((argb >> 16) & 0xFF) / 255,
            green: Double((argb >> 8) & 0xFF) / 255,
            blue: Double(argb & 0xFF) / 255,
            opacity: Double((argb >> 24) & 0xFF) / 255
        )
    }
}
