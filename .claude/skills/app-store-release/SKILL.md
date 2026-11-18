---
name: app-store-release
description: Releasing an iOS app on the App Store, from SDK requirements and build numbers to App Store Connect texts, screenshots and submitting for review. Use when preparing, uploading or submitting an iOS version, writing store texts, making store assets, or reading Apple's developer emails.
---

# Releasing an iOS app

## SDK and Xcode

- Each spring Apple starts requiring the newest SDK for uploads. An upload built with an older one gets an
  **ITMS-90725** warning email first (for example: from April 2027, iOS 27 SDK / Xcode 27). The warning doesn't
  block that upload.
- GitHub's `macos-latest` image lags behind new Xcode versions; newer ones come first as preview labels (such as
  `xcode-27`). Pick the label in `runs-on`, and in the job select the newest non-beta Xcode
  (`ls -d /Applications/Xcode_*.app | grep -v -i -E 'beta|release_candidate|_rc' | sort -V | tail -1`).
- Check the workflow log's `xcodebuild -version` before each release.
- The minimum iOS version (deployment target) is a separate choice. Raising it drops users on older iOS.

## Versions

- Version (`MARKETING_VERSION`) per release; the build number (`CURRENT_PROJECT_VERSION`) must grow with every
  upload, also across versions. A CI run number plus a fixed offset works.
- Signing in CI: one base64 secret holding the certificate, profiles and App Store Connect API key, unpacked into a
  keychain of the job's own. A public repo's logs are public: mask the signing identity (on a personal account
  it's your legal name), team ID and key IDs with `::add-mask::`.

## App Store Connect texts

Plain text: no Markdown; use "•" for bullets.

- **Description:** one sentence on what it does; where to get anything else it needs (a short domain, at the
  top); 4–6 bullets of what it does for the user; requirements last. No marketing filler.
- **What's New:** grouped under short headings, one line per change:
  ```
  New
  • …
  
  Improved
  • …
  
  Fixed
  • …
  ```
- **Marketing URL:** your own page for the app.
- **Review notes:** only what a reviewer needs to test: what else to install, the steps, and why each background
  mode is needed. Under about 1,000 characters. The field's own help says the same.

## Assets

- Screenshots: one set at 1179 × 2556 ("iPhone with Dynamic Island", medium) is enough; App Store Connect scales
  it for the other iPhones.
- Use real screens. Set the status bar to 9:41; replace personal names (device, computer, contact names); don't
  show someone else's art (a wallpaper): draw your own background.
- The product page header ("universal" asset) is 5244 × 2950, and every crop keeps the safe area 1402 × 962 at
  (1921, 660). On an iPhone it shows small: a short headline and a few big icons, nothing detailed.
- Icons in assets should be the app's own (the same SF Symbols the app shows), not look-alikes.

## Submitting

1. The build appears under TestFlight after processing; pick it on the version page.
2. Release options: *Automatically release this version*, *Release update to all users immediately* (or phased),
   and *Keep existing rating*.
3. **Add for Review**, then **Submit for Review** in the draft submission panel. Status: *Waiting for Review*,
   usually within 48 hours; Apple emails the result.
4. While it waits some fields can still be edited; a new build means removing the version from review first.

## Apple's emails

Tax and price updates (VAT changes, storefront price equalization, tax categories) only matter for paid apps and
in-app purchases. A free app without in-app purchases needs nothing.

See also `update-notices` (telling users about a new version) and `open-source-legal` (privacy policy, App
Privacy).
