---
name: ios-builds-without-xcode
description: Pitfalls of building iOS apps with xtool (SwiftPM, on Linux or Windows) and making App Store builds with Xcode on CI - the recorded SDK version, App Intents metadata, reaching the iPhone from a container. Use when an xtool build looks or behaves differently from an Xcode build, after updating the SDK, or when running iPhone tools from a dev container.
---

# iOS builds without Xcode

## The SDK version in the binary

xtool's linker (ld64.lld) writes the **deployment target** into `LC_BUILD_VERSION` as the SDK version. iOS
then runs the app as if it were built with that old SDK: on an iOS 18 target, no Liquid Glass tab bar, square
glass buttons, old-style menus.

Fix it in `Package.swift`, for the app and every extension (the last `-platform_version` wins):

```swift
let iOSSDKVersion = ["-Xlinker", "-platform_version", "-Xlinker", "ios", "-Xlinker", "18.0", "-Xlinker", "27.2"]
// linkerSettings: [.unsafeFlags(iOSSDKVersion)]
```

- The first number is the minimum iOS (as in `platforms`), the second the SDK actually used. Find it in
  `~/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/SDKSettings.json`
  (`Version`). Update it whenever the Darwin SDK is rebuilt from a newer Xcode.
- Keep it equal to what App Store builds use, so a development build behaves like the shipped one.
- Check a build: read `LC_BUILD_VERSION` (load command `0x32`: platform, minos, sdk; versions packed as
  `X << 16 | Y << 8 | Z`) from the app's and each extension's executable.

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
