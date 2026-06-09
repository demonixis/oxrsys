// swift-tools-version: 5.10
// SPDX-License-Identifier: MPL-2.0

import PackageDescription

let package = Package(
    name: "OXRSysSimulator",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
        .visionOS(.v1),
    ],
    products: [
        .library(name: "OXRSysSimulator", targets: ["OXRSysSimulator"]),
    ],
    dependencies: [
        .package(path: "../OXRSysStreaming"),
    ],
    targets: [
        .target(
            name: "OXRSysSimulator",
            dependencies: ["OXRSysStreaming"],
            path: "Sources/OXRSysSimulator"
        ),
        .testTarget(
            name: "OXRSysSimulatorTests",
            dependencies: ["OXRSysSimulator"],
            path: "Tests/OXRSysSimulatorTests"
        ),
    ]
)
