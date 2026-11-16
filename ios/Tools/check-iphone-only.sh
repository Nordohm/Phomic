#!/usr/bin/env bash
# Fails unless the built app (and any extension that states a device family) is iPhone-only:
# UIDeviceFamily = [1]. An app that also lists iPad (2) needs iPad screenshots and an iPad review.
# Used by .github/workflows/ios.yml.
#   bash Tools/check-iphone-only.sh <path to Phomic.app>
set -euo pipefail
app=${1:?usage: $0 <path to Phomic.app>}
status=0
for plist in "$app/Info.plist" "$app"/PlugIns/*.appex/Info.plist; do
    [ -f "$plist" ] || continue
    name=$(basename "$(dirname "$plist")")
    family=$(plutil -extract UIDeviceFamily json -o - "$plist" 2>/dev/null | tr -d ' \n' || true)
    echo "$name: UIDeviceFamily ${family:-(not set)}"
    if [ "$family" = "[1]" ] || { [ -z "$family" ] && [ "$plist" != "$app/Info.plist" ]; }; then
        continue
    fi
    echo "::error::$name isn't iPhone-only (UIDeviceFamily ${family:-not set}); see TARGETED_DEVICE_FAMILY in Xcode/project.yml"
    status=1
done
exit $status
