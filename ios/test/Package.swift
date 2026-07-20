// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "RetroEmulatorTest",
    platforms: [.iOS(.v16)],
    targets: [
        // Pre-built xcframework produced by scripts/build_xcframework.sh.
        .binaryTarget(
            name: "RetroEmulator",
            path: "../../build/RetroEmulator.xcframework"
        ),
        .testTarget(
            name: "AresCoreTests",
            dependencies: ["RetroEmulator"],
            path: "Tests/AresCoreTests",
            // Test media staged locally (gitignored) — e.g. rdpqdemo.z64 for
            // the N64 render check. Ships with only a README. (Not named
            // "Resources": codesign rejects an iOS bundle with a root-level
            // Resources/ dir as a malformed macOS-style bundle.)
            resources: [.copy("TestMedia")],
            linkerSettings: [
                .linkedLibrary("c++"),
                // The framework binary now carries librashader's Metal runtime.
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                // ...and MoltenVK (N64's Vulkan implementation), which needs:
                .linkedFramework("IOSurface"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("CoreGraphics"),
            ]
        ),
    ]
)
