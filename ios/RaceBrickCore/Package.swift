// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "RaceBrickCore",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [
        .library(name: "RaceBrickCore", targets: ["RaceBrickCore"]),
    ],
    targets: [
        .target(name: "RaceBrickCore"),
        .testTarget(name: "RaceBrickCoreTests", dependencies: ["RaceBrickCore"]),
    ]
)
