// swift-tools-version:5.9
//
// Swift package for the iOS app (also builds on macOS, which is how CI
// checks it). It compiles the shared C core straight from src/ and include/,
// the same files vcd and its tests use, so the phone and the host can never
// disagree on the wire format.
//
//   VCCore             C: the wire format, control protocol, sender engine and
//                      on-screen gamepad layout
//   VirtualController  Swift: ControllerLink, the sender thread and its API
//
// This file has to live at the repository root: a package cannot reach
// sources outside its own directory.
import PackageDescription

let package = Package(
    name: "VirtualController",
    platforms: [.iOS(.v15), .macOS(.v12)],
    products: [
        .library(name: "VirtualController", targets: ["VirtualController"]),
    ],
    targets: [
        .target(
            name: "VCCore",
            path: ".",
            exclude: ["docs", "mobile", "scripts", "tests", "tools", "Makefile", "PLAN.md"],
            sources: ["src/encoder.c", "src/protocol.c", "src/vc_client.c",
                      "src/layout.c", "src/parser.c"],
            publicHeadersPath: "include"   // include/module.modulemap picks what Swift sees
            // Warnings are enforced by the Makefile build of the same files
            // (-Wall -Wextra -Werror); unsafeFlags here would stop apps from
            // depending on this package by version.
        ),
        .target(
            name: "VirtualController",
            dependencies: ["VCCore"],
            path: "mobile/ios/Sources/VirtualController"
        ),
    ],
    cLanguageStandard: .c11
)
