import GameController
import RetroEmulator

/// Maps GCController face buttons and d-pad to the ares HARDWARE input bitmask
/// for logical port 1. Software input (developer overlays) goes through the
/// native per-port path (`emu_press_button`), resolved against the connected
/// device — the two masks are OR'd natively at poll time, so neither source can
/// clobber the other (mirrors Android's hwMask/swMask merge).
///
/// GCExtendedGamepad uses Xbox face-button naming (A=bottom, B=right, X=left, Y=top).
/// SNES uses positional naming for the same layout (B=bottom, A=right, Y=left, X=top).
/// Bit positions match the positional gamepad in system_registry.cpp.
final class EmulatorInput {

    // Positional bit constants — must match def.buttons in system_registry.cpp.
    private static let B: UInt32      = 1 << 0
    private static let Y: UInt32      = 1 << 1
    private static let select: UInt32 = 1 << 2
    private static let start: UInt32  = 1 << 3
    private static let up: UInt32     = 1 << 4
    private static let down: UInt32   = 1 << 5
    private static let left: UInt32   = 1 << 6
    private static let right: UInt32  = 1 << 7
    private static let A: UInt32      = 1 << 8
    private static let X: UInt32      = 1 << 9
    private static let L: UInt32      = 1 << 10
    private static let R: UInt32      = 1 << 11

    /// D-pad bits for an analog direction input. Threshold is ares' digital-input
    /// semantics: pressed past half deflection (desktop-ui InputDigital::value(),
    /// input.cpp:190-193 — ±16384 of s16 full scale). GameController convention:
    /// positive y is up.
    static func directionBits(x: Float, y: Float) -> UInt32 {
        var mask: UInt32 = 0
        if x < -0.5 { mask |= left }
        if x >  0.5 { mask |= right }
        if y >  0.5 { mask |= up }
        if y < -0.5 { mask |= down }
        return mask
    }

    private let ctx: OpaquePointer
    private var observations: [Any] = []

    init(ctx: OpaquePointer) {
        self.ctx = ctx
    }

    func startObserving() {
        let nc = NotificationCenter.default
        observations.append(
            nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) {
                [weak self] n in
                if let c = n.object as? GCController { self?.wire(c) }
            }
        )
        observations.append(
            nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) {
                [weak self] _ in
                guard let self else { return }
                emu_set_input(self.ctx, 1, 0)
            }
        )
        GCController.controllers().first.map { wire($0) }
    }

    func stopObserving() {
        observations.forEach { NotificationCenter.default.removeObserver($0) }
        observations.removeAll()
    }

    private func wire(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }

        pad.valueChangedHandler = { [weak self] pad, _ in
            guard let self else { return }
            var mask: UInt32 = 0
            // GC A (bottom) → SNES B; GC B (right) → SNES A
            if pad.buttonA.isPressed { mask |= Self.B }
            if pad.buttonB.isPressed { mask |= Self.A }
            // GC X (left) → SNES Y; GC Y (top) → SNES X
            if pad.buttonX.isPressed { mask |= Self.Y }
            if pad.buttonY.isPressed { mask |= Self.X }
            if pad.dpad.up.isPressed    { mask |= Self.up }
            if pad.dpad.down.isPressed  { mask |= Self.down }
            if pad.dpad.left.isPressed  { mask |= Self.left }
            if pad.dpad.right.isPressed { mask |= Self.right }
            // Left stick doubles as the d-pad, ares digital-input threshold.
            mask |= Self.directionBits(
                x: pad.leftThumbstick.xAxis.value,
                y: pad.leftThumbstick.yAxis.value
            )
            if pad.leftShoulder.isPressed  { mask |= Self.L }
            if pad.rightShoulder.isPressed { mask |= Self.R }
            if pad.buttonOptions?.isPressed == true { mask |= Self.select }
            if pad.buttonMenu.isPressed             { mask |= Self.start }

            emu_set_input(self.ctx, 1, mask)
        }
    }
}
