import GameController
import RetroEmulator

/// Maps GCController face buttons and d-pad to the ares input bitmask for port 1.
///
/// GCExtendedGamepad uses Xbox face-button naming (A=bottom, B=right, X=left, Y=top).
/// SNES uses positional naming for the same layout (B=bottom, A=right, Y=left, X=top).
/// Bit positions match kSnesButtons in ares_ios_api.cpp.
final class EmulatorInput {

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
                ares_set_input(self.ctx, 1, 0)
            }
        )
        GCController.controllers().first.map { wire($0) }
    }

    func stopObserving() {
        observations.forEach { NotificationCenter.default.removeObserver($0) }
        observations.removeAll()
    }

    // MARK: - Private

    private func wire(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }

        // Bit mask constants — must match kSnesButtons in ares_ios_api.cpp
        let B      : UInt32 = 1 << 0
        let Y      : UInt32 = 1 << 1
        let select : UInt32 = 1 << 2
        let start  : UInt32 = 1 << 3
        let up     : UInt32 = 1 << 4
        let down   : UInt32 = 1 << 5
        let left   : UInt32 = 1 << 6
        let right  : UInt32 = 1 << 7
        let A      : UInt32 = 1 << 8
        let X      : UInt32 = 1 << 9
        let L      : UInt32 = 1 << 10
        let R      : UInt32 = 1 << 11

        pad.valueChangedHandler = { [weak self] pad, _ in
            guard let self else { return }
            var mask: UInt32 = 0
            // GC A (bottom) → SNES B; GC B (right) → SNES A
            if pad.buttonA.isPressed { mask |= B }
            if pad.buttonB.isPressed { mask |= A }
            // GC X (left) → SNES Y; GC Y (top) → SNES X
            if pad.buttonX.isPressed { mask |= Y }
            if pad.buttonY.isPressed { mask |= X }
            if pad.dpad.up.isPressed    { mask |= up }
            if pad.dpad.down.isPressed  { mask |= down }
            if pad.dpad.left.isPressed  { mask |= left }
            if pad.dpad.right.isPressed { mask |= right }
            if pad.leftShoulder.isPressed  { mask |= L }
            if pad.rightShoulder.isPressed { mask |= R }
            if pad.buttonOptions?.isPressed == true { mask |= select }
            if pad.buttonMenu.isPressed             { mask |= start }
            ares_set_input(self.ctx, 1, mask)
        }
    }
}
