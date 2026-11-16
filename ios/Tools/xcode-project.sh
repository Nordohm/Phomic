#!/usr/bin/env bash
# Makes Xcode/Phomic.xcodeproj from Xcode/project.yml, for App Store builds. Needs a Mac with
# Xcode and XcodeGen (brew install xcodegen); GitHub Actions runs it in .github/workflows/ios.yml.
# Both outputs (the project and the asset catalog) are generated, so they're not in Git.
set -euo pipefail
cd "$(dirname "$0")/../Xcode"

# The app icon, as the asset catalog Xcode wants (one 1024 x 1024 image).
iconset=Assets.xcassets/AppIcon.appiconset
rm -rf Assets.xcassets
mkdir -p "$iconset"
cp ../Resources/AppIcon.png "$iconset/AppIcon.png"
cat > Assets.xcassets/Contents.json <<'EOF'
{ "info" : { "author" : "xcode", "version" : 1 } }
EOF
cat > "$iconset/Contents.json" <<'EOF'
{
  "images" : [
    { "filename" : "AppIcon.png", "idiom" : "universal", "platform" : "ios", "size" : "1024x1024" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
EOF

xcodegen generate --spec project.yml
