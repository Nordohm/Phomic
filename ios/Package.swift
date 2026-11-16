// swift-tools-version: 6.0

import PackageDescription

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
                .unsafeFlags(["-Xlinker", "-u", "-Xlinker", "_main"]),
            ]
        ),
        .target(
            name: "Phomic",
            dependencies: ["COpus", "PhomicActivity"]
        ),
    ]
)
