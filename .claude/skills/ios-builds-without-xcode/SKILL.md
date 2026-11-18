---
name: ios-builds-without-xcode
description: Pitfalls of building iOS apps with xtool (SwiftPM, on Linux or Windows) and making App Store builds with Xcode on CI - the recorded SDK version, App Intents metadata, reaching the iPhone from a container. Use when an xtool build looks or behaves differently from an Xcode build, after updating the SDK, or when running iPhone tools from a dev container.
---

# iOS builds without Xcode

## The SDK version in the binary

On Linux, xtool links through clang, which gets the SDK as `--sysroot` but reads the SDK version
(`SDKSettings.json`) only from `-isysroot`. So the **deployment target** ends up in `LC_BUILD_VERSION` as the SDK
version, and iOS runs the app as if it were built with that old SDK: on an iOS 18 target, no Liquid Glass tab
bar, square glass buttons, old-style menus.

Fix it at the end of `Package.swift`, below every target, so it reaches the app and each extension:

```swift
#if os(Linux)
let iPhoneOSSDK = (Context.environment["HOME"] ?? "/root")
    + "/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk"
for target in package.targets where [.regular, .executable, .test].contains(target.type) {
    target.linkerSettings = (target.linkerSettings ?? []) + [
        .unsafeFlags(["-Xclang-linker", "-isysroot", "-Xclang-linker", iPhoneOSSDK], .when(platforms: [.iOS])),
    ]
}
#endif
```

- clang then reads the real version from the SDK at every link: nothing to update when the SDK changes, and the
  minimum still comes from `platforms`. Xcode builds (and builds on a Mac) are unaffected; Xcode passes
  `-isysroot` itself.
- Don't also pass a fixed `-Xlinker -platform_version ios <min> <sdk>`: the linker uses the last one it gets,
  which would override the real version.
- If the SDK path is wrong, clang warns `no such sysroot directory` and the old behaviour comes back silently.
- Check a build: read `LC_BUILD_VERSION` (load command `0x32`: platform, minos, sdk; versions packed as
  `X << 16 | Y << 8 | Z`) from the app's and each extension's executable. `sdk` should be the SDK's version, not
  `minos`.

## App Intents metadata

iOS only runs an App Intent that the bundle's `Metadata.appintents` describes. Xcode generates it during every
build; xtool doesn't. Generate it with
[re-appintentsmetadataprocessor](https://codeberg.org/viraptor/re-appintentsmetadataprocessor) after changing an
intent, commit the result, and copy it into the app and each extension through `xtool.yml` resources. In the
Xcode build, share intents between targets with an `AppIntentsPackage`.

## App Store builds

Apple's license only allows the iOS SDK on Apple hardware, and App Store uploads need Xcode's toolchain anyway:
generate an Xcode project (XcodeGen `project.yml`) and build it on a GitHub macOS runner. See
`app-store-release`.

## Reaching the iPhone from a container

- Point libimobiledevice tools at the host's usbmuxd: `USBMUXD_SOCKET_ADDRESS=host.docker.internal:27020`
  (plus `--add-host=host.docker.internal:host-gateway`). `idevicesyslog`, `afcclient` and xtool's install work
  through it.
- Sources on a Windows drive are slow to build from: link `.build` to a Docker volume.
- From Git Bash, run container commands through WSL with `MSYS_NO_PATHCONV=1` (or Git Bash rewrites `/paths`),
  and pipe the script in (`… docker exec -i <container> bash -s`) so no shell on the way expands its `$`.
- Run long captures inside the container into a file and read it afterwards; a capture piped out through
  `docker exec` can block, and a killed client can leave it running.
