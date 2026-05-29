// swift-tools-version: 5.10
// SPDX-License-Identifier: MPL-2.0

import PackageDescription

let package = Package(
    name: "OXRSysStreaming",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
        .visionOS(.v1),
    ],
    products: [
        .library(name: "OXRSysStreaming", targets: ["OXRSysStreaming"]),
    ],
    targets: [
        .target(
            name: "OXRSysStreaming",
            path: "Sources/OXRSysStreaming",
            resources: [
                .process("Shaders.metal"),
            ]
        ),
        .testTarget(
            name: "OXRSysStreamingTests",
            dependencies: ["OXRSysStreaming"],
            path: "Tests/OXRSysStreamingTests"
        ),
    ]
)
