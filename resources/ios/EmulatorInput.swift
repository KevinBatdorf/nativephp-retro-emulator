import GameController
import RetroEmulator

/// Maps GCController face buttons and d-pad to the ares input bitmask for port 1,
/// and holds a software-input state map that developer-built overlays drive through
/// the bridge (`pressButton` / `releaseButton` / `setButtons`).
///
/// GCExtendedGamepad uses Xbox face-button naming (A=bottom, B=right, X=left, Y=top).
/// SNES uses positional naming for the same layout (B=bottom, A=right, Y=left, X=top).
/// Bit positions match kSnesButtons in ares_ios_api.cpp.
///
/// The hardware mask (from the controller) and the software mask (from the bridge)
/// are OR'd together before every push to ares, so neither source can clobber the
/// other — mirrors Android's keyBits/softwareBits merge.
final class EmulatorInput {

    // Bit mask constants — must match kSnesButtons in ares_ios_api.cpp.
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

    /// Maps a bridge button name to its ares bitmask. Names are the lowercase
    /// button identifiers exposed by `getPorts()`. Returns nil for unknown names.
    static func buttonNameToBit(_ name: String) -> UInt32? {
        switch name.lowercased() {
        case "b":                 return B
        case "y":                 return Y
        case "select":            return select
        case "start":             return start
        case "up":                return up
        case "down":              return down
        case "left":              return left
        case "right":             return right
        case "a":                 return A
        case "x":                 return X
        case "l", "l1", "lshoulder": return L
        case "r", "r1", "rshoulder": return R
        default:                  return nil
        }
    }

    private let ctx: OpaquePointer
    private var observations: [Any] = []

    /// Input state for port 1. Hardware and software masks are tracked separately
    /// and OR'd on every push. Guarded by `stateLock` because the controller handler
    /// runs on the main queue while bridge calls arrive on an arbitrary bridge thread.
    private var hardwareMask: UInt32 = 0
    private var softwareMask: UInt32 = 0
    private let stateLock = NSLock()

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
                self.stateLock.lock()
                self.hardwareMask = 0
                self.stateLock.unlock()
                self.pushCombined()
            }
        )
        GCController.controllers().first.map { wire($0) }
    }

    func stopObserving() {
        observations.forEach { NotificationCenter.default.removeObserver($0) }
        observations.removeAll()
    }

    // MARK: - Software input (bridge-driven)

    /// Set a single software button pressed. `bit` comes from `buttonNameToBit`.
    func pressSoftwareButton(_ bit: UInt32) {
        stateLock.lock()
        softwareMask |= bit
        stateLock.unlock()
        pushCombined()
    }

    /// Clear a single software button.
    func releaseSoftwareButton(_ bit: UInt32) {
        stateLock.lock()
        softwareMask &= ~bit
        stateLock.unlock()
        pushCombined()
    }

    // MARK: - Private

    private func pushCombined() {
        stateLock.lock()
        let combined = hardwareMask | softwareMask
        stateLock.unlock()
        ares_set_input(ctx, 1, combined)
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
            if pad.leftShoulder.isPressed  { mask |= Self.L }
            if pad.rightShoulder.isPressed { mask |= Self.R }
            if pad.buttonOptions?.isPressed == true { mask |= Self.select }
            if pad.buttonMenu.isPressed             { mask |= Self.start }

            self.stateLock.lock()
            self.hardwareMask = mask
            self.stateLock.unlock()
            self.pushCombined()
        }
    }
}
