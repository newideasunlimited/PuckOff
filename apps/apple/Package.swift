// swift-tools-version: 6.2
// SPDX-License-Identifier: Apache-2.0
import PackageDescription

let package = Package(
  name: "PuckOffKit",
  platforms: [.macOS(.v14), .iOS(.v17)],
  products: [
    .library(name: "PuckOffCore", targets: ["PuckOffCore"]),
    .library(name: "PuckOffPlatform", targets: ["PuckOffPlatform"]),
  ],
  targets: [
    .target(name: "PuckOffCore"),
    .target(name: "PuckOffPlatform", dependencies: ["PuckOffCore"]),
    .testTarget(
      name: "PuckOffCoreTests",
      dependencies: ["PuckOffCore"],
      resources: [.copy("Fixtures")]
    ),
  ]
)
