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
            // Props are whole percentages; the resolver and drawing want fractions.
            threshold: node.props.getFloat(
                "threshold", default: DpadResolver.defaultThreshold * 100) / 100,
            diagonalRatio: node.props.getFloat(
                "diagonal_ratio", default: DpadResolver.defaultDiagonalRatio * 100) / 100,
            thickness: CGFloat(node.props.getFloat("thickness", default: 36) / 100),
            radiusShare: CGFloat(node.props.getFloat("radius", default: 28) / 100),
            // 0 when no handler is bound, which keeps PHP out of the press path.
            onChange: node.props.getCallbackId("on_change"),
            nodeId: node.id,
            // 0 unless a SharedValue is bound; no timer starts otherwise.
            panXId: node.props.getInt("pan_x_id", default: 0),
            panYId: node.props.getInt("pan_y_id", default: 0),
            panXInitial: CGFloat(node.props.getFloat("pan_x_initial", default: 0)),
            panYInitial: CGFloat(node.props.getFloat("pan_y_initial", default: 0)),
            panSpeed: CGFloat(node.props.getFloat("pan_speed", default: 260)),
            panXMin: CGFloat(node.props.getFloat("pan_x_min", default: -.greatestFiniteMagnitude)),
            panXMax: CGFloat(node.props.getFloat("pan_x_max", default: .greatestFiniteMagnitude)),
            panYMin: CGFloat(node.props.getFloat("pan_y_min", default: -.greatestFiniteMagnitude)),
            panYMax: CGFloat(node.props.getFloat("pan_y_max", default: .greatestFiniteMagnitude)),
            baseColor: Color(argb: node.props.getColor("color", default: 0x66FF_FFFF)),
            activeColor: Color(argb: node.props.getColor("active_color", default: 0xE6FF_FFFF))
        )
    }
}

private struct DpadPad: View {
    let surface: String
    let port: Int
    let threshold: Float
    let diagonalRatio: Float
    let thickness: CGFloat
    let radiusShare: CGFloat
    let onChange: Int
    let nodeId: Int
    let panXId: Int
    let panYId: Int
    let panXInitial: CGFloat
    let panYInitial: CGFloat
    let panSpeed: CGFloat
    let panXMin: CGFloat
    let panXMax: CGFloat
    let panYMin: CGFloat
    let panYMax: CGFloat
    let baseColor: Color
    let activeColor: Color

    @State private var held: Set<DpadDirection> = []

    @State private var panTimer: Timer?

    var body: some View {
        GeometryReader { geo in
            let arm = min(geo.size.width, geo.size.height) * thickness
            let armLength = min(geo.size.width, geo.size.height) / 2 - arm / 2
            let span = arm + armLength * 2
            let radius = arm * radiusShare

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
            // High priority so an enclosing ScrollView doesn't pan the page
            // while a thumb is steering — the Android renderer consumes its
            // pointer changes for the same reason.
            .highPriorityGesture(
                // minimumDistance 0 so the pad answers the touch down, not the
                // first drag past a threshold.
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        push(DpadResolver.resolve(
                            nx: normalize(Float(value.location.x), Float(geo.size.width)),
                            ny: normalize(Float(value.location.y), Float(geo.size.height)),
                            threshold: threshold,
                            diagonalRatio: diagonalRatio
                        ))
                    }
                    .onEnded { _ in push([]) }
            )
        }
        // A press outlives the view that started it (navigation, rotation), and
        // the core would hold that button forever.
        .onDisappear {
            push([])
            panTimer?.invalidate()
            panTimer = nil
        }
        .onAppear {
            guard panXId != 0 || panYId != 0 else { return }
            if panXId != 0 { SharedValueStore.shared.set(panXInitial, for: panXId) }
            if panYId != 0 { SharedValueStore.shared.set(panYInitial, for: panYId) }

            let interval = 1.0 / 60.0
            panTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { _ in
                guard !held.isEmpty else { return }
                let step = panSpeed * CGFloat(interval)
                let dx = (held.contains(.right) ? step : 0) - (held.contains(.left) ? step : 0)
                let dy = (held.contains(.down) ? step : 0) - (held.contains(.up) ? step : 0)
                if panXId != 0, dx != 0 {
                    let next = SharedValueStore.shared.value(for: panXId) + dx
                    SharedValueStore.shared.set(min(max(next, panXMin), panXMax), for: panXId)
                }
                if panYId != 0, dy != 0 {
                    let next = SharedValueStore.shared.value(for: panYId) + dy
                    SharedValueStore.shared.set(min(max(next, panYMin), panYMax), for: panYId)
                }
            }
        }
    }

    @ViewBuilder
    private func highlight(
        _ direction: DpadDirection, arm: CGFloat, armLength: CGFloat, radius: CGFloat
    ) -> some View {
        let isVertical = direction == .up || direction == .down
        let offset = (arm + armLength) / 2

        ArmShape(direction: direction, radius: radius)
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
        defer {
            held = next
            if onChange != 0 {
                NativeUIBridge.sendTextChangeEvent(
                    onChange, nodeId: nodeId,
                    text: next.map(\.rawValue).joined(separator: ","))
            }
        }

        guard let renderer = EmulatorFunctions.renderer(named: surface) else { return }
        for direction in next.subtracting(held) {
            _ = renderer.pressButton(port: port, name: direction.rawValue, down: true)
        }
        for direction in held.subtracting(next) {
            _ = renderer.pressButton(port: port, name: direction.rawValue, down: false)
        }
    }
}

/// An arm's highlight, rounded on its outer tip and square where it meets the
/// hub. Rounding all four corners detaches the lit arm from the cross.
/// Hand-rolled because UnevenRoundedRectangle needs iOS 16.
private struct ArmShape: Shape {
    let direction: DpadDirection
    let radius: CGFloat

    func path(in rect: CGRect) -> Path {
        let r = min(radius, min(rect.width, rect.height) / 2)
        let (tl, tr, br, bl) = cornerRadii(r)
        var path = Path()

        path.move(to: CGPoint(x: rect.minX + tl, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX - tr, y: rect.minY))
        if tr > 0 {
            path.addQuadCurve(
                to: CGPoint(x: rect.maxX, y: rect.minY + tr),
                control: CGPoint(x: rect.maxX, y: rect.minY))
        }
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY - br))
        if br > 0 {
            path.addQuadCurve(
                to: CGPoint(x: rect.maxX - br, y: rect.maxY),
                control: CGPoint(x: rect.maxX, y: rect.maxY))
        }
        path.addLine(to: CGPoint(x: rect.minX + bl, y: rect.maxY))
        if bl > 0 {
            path.addQuadCurve(
                to: CGPoint(x: rect.minX, y: rect.maxY - bl),
                control: CGPoint(x: rect.minX, y: rect.maxY))
        }
        path.addLine(to: CGPoint(x: rect.minX, y: rect.minY + tl))
        if tl > 0 {
            path.addQuadCurve(
                to: CGPoint(x: rect.minX + tl, y: rect.minY),
                control: CGPoint(x: rect.minX, y: rect.minY))
        }
        path.closeSubpath()

        return path
    }

    private func cornerRadii(_ r: CGFloat) -> (CGFloat, CGFloat, CGFloat, CGFloat) {
        switch direction {
        case .up: return (r, r, 0, 0)
        case .down: return (0, 0, r, r)
        case .left: return (r, 0, 0, r)
        case .right: return (0, r, r, 0)
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
