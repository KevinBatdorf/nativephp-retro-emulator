import XCTest
import RetroEmulator

/// The bring-your-own libretro loader on the static iOS build. No core
/// dylib exists on the simulator (the buildbot publishes no iOS slices), so
/// these prove the loader itself: registered despite claiming no system,
/// probing unknown names, and rejecting non-core dylibs cleanly.
final class LibretroTests: XCTestCase {
    private var ctx: OpaquePointer!

    override func setUp() {
        super.setUp()
        ctx = emu_create()
        emu_reset(ctx)
    }

    override func tearDown() {
        emu_reset(ctx)
        super.tearDown()
    }

    func testLoaderIsRegisteredInStaticBuild() {
        // The loader claims no system, so the engines list is the only
        // observable proof its link anchor survived (the -O2 elision trap).
        let json = String(cString: emu_get_backends_json())
        let root = try! JSONSerialization.jsonObject(
            with: json.data(using: .utf8)!) as! [String: Any]
        let engines = root["engines"] as? [String] ?? []
        XCTAssertTrue(engines.contains("libretro"),
                      "the BYO loader must be registered — got \(engines)")
        XCTAssertTrue(["ares", "sameboy", "mgba"].allSatisfy(engines.contains),
                      "all bundled engines must be registered — got \(engines)")
    }

    func testUnknownCoreFailsToStage() {
        XCTAssertFalse(emu_load_system(ctx, "sfc", nil, "no_such_core_xyz"),
                       "a name no engine answers and no core satisfies must fail")
    }

    func testNonCoreDylibIsRejectedCleanly() {
        // dlopen succeeds on a real dylib; the retro_* symbol probe must
        // reject it without crashing — the probe chain end to end.
        XCTAssertFalse(emu_load_system(ctx, "sfc", nil, "/usr/lib/libSystem.B.dylib"),
                       "a dylib that is not a libretro core must be rejected")
    }

    func testBundledEngineIsNeverAdoptionProbed() {
        XCTAssertFalse(emu_load_system(ctx, "sfc", nil, "sameboy"),
                       "a bundled engine that doesn't claim the system must fail")
    }

    func testBundledEnginesRefuseEngineOptions() {
        XCTAssertTrue(emu_load_system(ctx, "gb", nil, nil))   // sameboy default
        let refusal = String(cString: emu_set_engine_option(ctx, "some_key", "v", true))
        XCTAssertTrue(refusal.contains("typed config"),
                      "bundled engines must refuse engine options — got '\(refusal)'")
        XCTAssertEqual(String(cString: emu_get_engine_options_json(ctx)), "[]",
                       "bundled engines declare no engine options")
    }

    func testEngineOptionWithNothingStagedErrors() {
        let refusal = String(cString: emu_set_engine_option(ctx, "k", "v", true))
        XCTAssertFalse(refusal.isEmpty, "no staged system must refuse, not no-op")
    }
}
