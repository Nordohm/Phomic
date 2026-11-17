// swift-tools-version: 6.0

import PackageDescription

// The iOS SDK version xtool builds record in the app. xtool's linker (ld64.lld) gets the
// deployment target there (18.0) instead of the SDK's own version, and iOS then runs the app
// the way it ran apps built for iOS 18: no Liquid Glass tab bar, square glass buttons, system
// menus drawn the old way. This passes the linker its version again (the last one wins):
// minimum iOS 18.0 (as in `platforms` below), SDK 26.0, the first with the current design.
// App Store builds come from Xcode (Xcode/project.yml) and record their real SDK.
let iOSSDKVersion = ["-Xlinker", "-platform_version", "-Xlinker", "ios", "-Xlinker", "18.0", "-Xlinker", "26.0"]

let package = Package(
    name: "Phomic",
    platforms: [
        .iOS(.v18),
        .macOS(.v14),
    ],
    products: [
        // The main app (xtool.yml `product`) and its Live Activity widget extension.
        .library(
            name: "Phomic",
            targets: ["Phomic"]
        ),
        .library(
            name: "PhomicWidget",
            targets: ["PhomicWidget"]
        ),
        // For the Xcode project (Xcode/project.yml, App Store builds): it compiles the app and
        // widget sources itself and takes these two from the package.
        .library(name: "COpus", targets: ["COpus"]),
        .library(name: "PhomicActivity", targets: ["PhomicActivity"]),
    ],
    targets: [
        // libopus 1.6.1 (BSD-3-Clause, see Sources/COpus/COPYING): core codec only,
        // floating point, portable C. Used to send Opus audio over Bluetooth.
        .target(
            name: "COpus",
            path: "Sources/COpus",
            exclude: ["COPYING", "AUTHORS"],
            sources: ["celt", "silk", "src", "phomic_opus.c"],
            publicHeadersPath: "include",
            cSettings: [
                .define("OPUS_BUILD"),
                .define("USE_ALLOCA"),
                .define("HAVE_LRINTF"),
                .headerSearchPath("celt"),
                .headerSearchPath("silk"),
                .headerSearchPath("silk/float"),
                // Optimize the codec even in Debug builds (unoptimized Opus is very slow),
                // and don't surface warnings from third-party code.
                .unsafeFlags(["-O2", "-w"]),
            ]
        ),
        // What the Live Activity shows; shared by the app and the widget.
        .target(name: "PhomicActivity"),
        .target(
            name: "PhomicWidget",
            dependencies: ["PhomicActivity"],
            linkerSettings: [
                // xtool links extensions with -dead_strip and the entry point _NSExtensionMain,
                // which finds the widget's @main through the __swift5_entry section. Nothing
                // references that, so the linker would drop it and every line of widget code
                // (and the signer then fails: "Can't find free space of LoadCommands"). Keep it.
                .unsafeFlags(["-Xlinker", "-u", "-Xlinker", "_main"] + iOSSDKVersion),
            ]
        ),
        .target(
            name: "Phomic",
            dependencies: ["COpus", "PhomicActivity"],
            linkerSettings: [.unsafeFlags(iOSSDKVersion)]
        ),
    ]
)
