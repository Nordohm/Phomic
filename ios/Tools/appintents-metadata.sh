#!/usr/bin/env bash
# Regenerates Resources/Metadata.appintents: the App Intents metadata iOS needs before it
# runs ToggleMuteIntent (the Live Activity's mute button). Xcode makes this during every
# build; xtool doesn't, so it's generated here and shipped as a resource.
#
# Run from anywhere:  bash Tools/appintents-metadata.sh
# Needs xtool's Darwin SDK (set SWIFT_SDK_BUNDLE if it isn't in the default place below)
# and a Rust reimplementation of Apple's tool (MIT):
#   https://codeberg.org/viraptor/re-appintentsmetadataprocessor
#   (cargo build --release, then copy target/release/appintentsmetadataprocessor to
#   /root/.cache/xtool/tools/, or set AIMP to wherever it is)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
# Default: next to xtool's cache.
TOOL=${AIMP:-/root/.cache/xtool/tools/appintentsmetadataprocessor}
BUNDLE=${SWIFT_SDK_BUNDLE:-/root/.swiftpm/swift-sdks/darwin.artifactbundle}
SDK=$(ls -d "$BUNDLE"/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS*.sdk | head -1)
TOOLCHAIN=$BUNDLE/Developer/Toolchains/XcodeDefault.xctoolchain
MODULE=PhomicActivity
TARGET=arm64-apple-ios18.0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ls "$ROOT"/Sources/$MODULE/*.swift > "$WORK/sources.txt"
# The compiler records constant values for types conforming to these protocols.
echo '["AppIntent","AppEntity","AppEnum","EntityQuery","AppShortcutsProvider","DynamicOptionsProvider","TransientAppEntity"]' > "$WORK/protocols.json"

swiftc -target "$TARGET" -sdk "$SDK" -resource-dir "$TOOLCHAIN/usr/lib/swift" \
    -module-name "$MODULE" -parse-as-library -swift-version 6 -wmo -c -o "$WORK/$MODULE.o" \
    -emit-const-values-path "$WORK/$MODULE.swiftconstvalues" \
    -Xfrontend -const-gather-protocols-file -Xfrontend "$WORK/protocols.json" \
    $(cat "$WORK/sources.txt")
echo "$WORK/$MODULE.swiftconstvalues" > "$WORK/constvalues.txt"

mkdir -p "$WORK/out"
"$TOOL" --output "$WORK/out" --toolchain-dir "$TOOLCHAIN" --module-name "$MODULE" \
    --sdk-root "$SDK" --xcode-version 26A000 --platform-family iOS --deployment-target 18.0 \
    --target-triple "$TARGET" --source-file-list "$WORK/sources.txt" \
    --swift-const-vals-list "$WORK/constvalues.txt" --binary-file "$WORK/$MODULE.o" \
    --force --compile-time-extraction

echo "--- output:"
find "$WORK/out" -type f | sed "s|$WORK/out/||"
rm -rf "$ROOT/Resources/Metadata.appintents"
cp -r "$WORK/out/Metadata.appintents" "$ROOT/Resources/Metadata.appintents"
echo "Wrote Resources/Metadata.appintents"
