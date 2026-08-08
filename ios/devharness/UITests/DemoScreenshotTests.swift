import XCTest

/// Drives the installed RetroEmu demo on the physical iPhone and captures
/// full-screen screenshots — the only screencap path a physical device offers
/// from the CLI. Verifies (by eye, from the exported attachments): home grid
/// layout, landscape rotation, safe-area top bar, and play-screen control fit.
final class DemoScreenshotTests: XCTestCase {

    private let demoBundleId = "com.kevin.oceanwaveshine"

    private func snap(_ name: String) {
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = name
        shot.lifetime = .keepAlways
        add(shot)
    }

    func testCaptureDemoScreens() throws {
        let app = XCUIApplication(bundleIdentifier: demoBundleId)
        XCUIDevice.shared.orientation = .portrait
        app.launch()

        // Cold boot: bundle extraction + Laravel boot before /home renders.
        sleep(25)
        snap("01-home-portrait")

        XCUIDevice.shared.orientation = .landscapeLeft
        sleep(3)
        snap("02-home-landscape")

        // Into a game (SNES) — EDGE native buttons surface as accessibility
        // elements; fall back to a blind center tap if the query misses.
        let snes = app.descendants(matching: .any)["SNES"].firstMatch
        if snes.waitForExistence(timeout: 5) {
            snes.tap()
        } else {
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.35)).tap()
        }
        sleep(15)
        snap("03-play-landscape")

        XCUIDevice.shared.orientation = .portrait
        sleep(3)
        snap("04-play-portrait")

        // The in-place menu overlay (settings + transport).
        let menu = app.descendants(matching: .any)["☰ Menu"].firstMatch
        if menu.waitForExistence(timeout: 5) {
            menu.tap()
            sleep(2)
            snap("05-menu-overlay")
        }

        app.terminate()
    }

    /// iOS kills the app if it suspends mid-run; conformance needs it held foreground.
    func testKeepAlive() throws {
        let app = XCUIApplication(bundleIdentifier: demoBundleId)
        app.launch()
        sleep(280)
        snap("keepalive-final")
        app.terminate()
    }

    /// Boots GBC and holds it while the temp audio dump fills.
    func testGbcTape() throws {
        let app = XCUIApplication(bundleIdentifier: demoBundleId)
        XCUIDevice.shared.orientation = .portrait
        app.launch()
        sleep(25)
        let tile = app.descendants(matching: .any)["GBC"].firstMatch
        if tile.waitForExistence(timeout: 10) { tile.tap() }
        sleep(30)
        snap("gbc-tape-state")
        app.terminate()
    }

    /// GBC appears twice: the second entry covers back-out → re-enter.
    func testBootSweep() throws {
        let app = XCUIApplication(bundleIdentifier: demoBundleId)
        XCUIDevice.shared.orientation = .portrait
        app.launch()
        sleep(25)

        let systems = ["NES", "SNES", "Game Boy", "GBC", "GBA", "Mega Drive", "GBC"]
        for (i, name) in systems.enumerated() {
            let tile = app.descendants(matching: .any)[name].firstMatch
            guard tile.waitForExistence(timeout: 10) else {
                snap(String(format: "sweep-%02d-%@-MISSING", i, name))
                continue
            }
            tile.tap()
            sleep(14)
            snap(String(format: "sweep-%02d-%@", i, name.replacingOccurrences(of: " ", with: "-")))
            let back = app.descendants(matching: .any)["Back"].firstMatch
            if back.waitForExistence(timeout: 5) { back.tap() }
            sleep(6)
        }
        app.terminate()
    }
}
