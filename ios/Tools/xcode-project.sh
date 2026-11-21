#!/usr/bin/env bash
# Makes Xcode/Phomic.xcodeproj from Xcode/project.yml, for App Store builds. Needs a Mac with
# Xcode and XcodeGen (brew install xcodegen); GitHub Actions runs it in .github/workflows/ios.yml.
# The project is generated, so it's not in Git. The icon is Resources/AppIcon.icon, which Xcode
# builds itself.
set -euo pipefail
cd "$(dirname "$0")/../Xcode"

xcodegen generate --spec project.yml
