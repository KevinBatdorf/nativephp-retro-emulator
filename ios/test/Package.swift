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
            linkerSettings: [.linkedLibrary("c++")]
        ),
    ]
)
