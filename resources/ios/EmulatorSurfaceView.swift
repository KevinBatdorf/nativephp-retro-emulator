import SwiftUI

/// iOS EDGE entry point for `<native:emulator>` — the SwiftUI counterpart to
/// Android's `EmulatorSurface`. Wraps the Metal-backed `EmulatorRenderer`
/// UIView and registers it under the node's `name` prop so `Emulator.*` bridge
/// calls resolve to this instance; unregisters on teardown.
struct EmulatorSurfaceView: View {
    let node: NativeUINode

    var body: some View {
        EmulatorViewContainer(
            name: node.props.getString("name", default: "main"),
            zIndex: node.props.getInt("z_index", default: 0),
            // input_capture (focus | global) exists for Android, where the OS
            // routes gamepad keys to the focused view. iOS GCController input
            // is app-global by construction — both modes already behave as
            // "global", so the prop needs no wiring here.
            system: node.props.getString("system", default: ""),
            config: node.props.getString("config", default: ""),
            rom: node.props.getString("rom", default: "")
        )
    }
}

private struct EmulatorViewContainer: UIViewRepresentable {
    let name: String
    let zIndex: Int
    let system: String
    let config: String
    let rom: String

    func makeUIView(context: Context) -> EmulatorRenderer {
        let renderer = EmulatorRenderer(frame: .zero)
        renderer.surfaceName = name
        renderer.layer.zPosition = CGFloat(zIndex)
        EmulatorFunctions.register(name: name, renderer: renderer)
        return renderer
    }

    // Runs after make and on every SwiftUI update; re-stage only when the
    // declarative props actually change.
    func updateUIView(_ uiView: EmulatorRenderer, context: Context) {
        let bootKey = [system, config, rom].joined(separator: " ")
        if !system.isEmpty, uiView.declaredBootKey != bootKey {
            uiView.declaredBootKey = bootKey
            EmulatorFunctions.applyDeclarativeSetup(
                surface: name, system: system, configJson: config, rom: rom)
        }
    }

    static func dismantleUIView(_ uiView: EmulatorRenderer, coordinator: ()) {
        EmulatorFunctions.unregister(name: uiView.surfaceName, renderer: uiView)
    }
}
