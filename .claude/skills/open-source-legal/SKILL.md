---
name: open-source-legal
description: Legal checklist for publishing a free, open-source app (GitHub, App Store, a Windows installer) - licenses and third-party notices, trademarks, privacy policy, names, store assets and keeping a personal identity private. Use before a first release, when adding a dependency, naming an app, making store assets, or when asked whether something could get the project sued.
---

# Publishing an app without legal trouble

Not legal advice; a checklist of what was checked before a release.

## Your license

- A `LICENSE` file (MIT is simple: no warranty, keep the notice). Copyright line with the name you publish under.
- Say it in the README's License section, with a link to the third-party details.

## Third-party code

- List every dependency with its license and copyright holder in one place (`third_party/README.md`).
- MIT and BSD require their notice in every copy, binaries included: ship the texts with the program (a
  `licenses` folder) and with the app's acknowledgements.
- Code copied from another project (a control template, a snippet) keeps its license notice in a comment.
- **Windows App SDK runtime** (and other Microsoft redistributables): Microsoft's distributable-code terms require
  shipping their license and notices, and end-user terms at least as protective as theirs (no reverse
  engineering, no distributing the runtime apart from your app). Show them in the installer's license page.
- Don't bundle software you may not redistribute (a driver, say): link to the vendor's download, under their terms.

## Trademarks

- Name other companies' products only to say what yours works with ("for your Windows PC", "Bluetooth").
- Add a disclaimer: "X isn't made, sponsored or endorsed by Apple, Microsoft, … Apple, iPhone and App Store are
  trademarks of Apple Inc. …" in the README and the installer's license text.
- Logos are stricter than names. The Bluetooth logo (also as an SF Symbol) is a Bluetooth SIG trademark meant for
  members' qualified products. Using it is a known risk; the owner decides, and it's noted.
- App name: search trademarks, the App Store and domains first. Avoid Apple's patterns ("i…", "Air…") and names
  close to an existing app in the same category.

## Privacy

- A privacy policy (`PRIVACY.md` in the repo works) that lists every network request the app makes and what it
  sends (an update check sends only the app's identity), states that nothing is collected, and has a date.
  Update it with every new request.
- App Store Connect, App Privacy: "Data Not Collected" only if that's true for every request.
- iOS: a `PrivacyInfo.xcprivacy` with the required-reason APIs used (`UserDefaults`, file timestamps, …).

## Store assets

- Your own art only: no wallpapers, photos or icons someone else made. Draw backgrounds yourself.
- No real personal names (device names, contacts) in screenshots.

## Your identity

- Publish under a brand. Keep the legal name, address and personal accounts out of the public repo, commits
  (use the brand's noreply email), CI logs (mask the signing identity) and store pages, as far as the stores
  allow.
- Apple's Xcode license allows the iOS SDK only on Apple computers: say so where docs explain building on other
  systems.
