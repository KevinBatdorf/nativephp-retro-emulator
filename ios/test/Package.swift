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
            name: "EmulatorCoreTests",
            dependencies: ["RetroEmulator"],
            path: "Tests/EmulatorCoreTests",
            linkerSettings: [
                .linkedLibrary("c++"),
                // The framework binary now carries librashader's Metal runtime.
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("IOSurface"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("CoreGraphics"),
            ]
        ),
    ]
)
