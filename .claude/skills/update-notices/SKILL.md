---
name: update-notices
description: Telling users a newer version is out - for an iOS app via Apple's lookup service, for a Windows app via the GitHub Releases API. Use when adding, changing or debugging an in-app update notice.
---

# Update notices

The same on every platform:

- Compare versions number by number: "1.10.0" is newer than "1.9.2"; missing parts count as 0.
- Show a notice with just the title ("Version X.Y.Z is available") and a button that opens the download.
- Closing it hides it until the next version: store the closed version, and compare against that.
- Offline or a bad reply: show nothing, and try again at the next check.
- Ask for a fresh answer, never a cached one.
- Mention the check in the privacy policy: what it sends (only the app's identity) and to whom.

## iOS: Apple's lookup service

- `GET https://itunes.apple.com/lookup?id=<app id>` → `results[0].version`, the version the App Store shows.
  `URLRequest.cachePolicy = .reloadIgnoringLocalCacheData`.
- Check on `UIApplication.didBecomeActiveNotification`, at most every 6 hours.
- The current version is `CFBundleShortVersionString`. Store the closed version in `UserDefaults`.
- Open the store page with `itms-apps://apps.apple.com/app/id<app id>`.
- A new version takes a while to reach the lookup service after release; that's fine.

## Windows: GitHub Releases

- `GET https://api.github.com/repos/<owner>/<repo>/releases/latest` with a `User-Agent` (required) and
  `Accept: application/vnd.github+json`. Drafts and prereleases aren't "latest".
- WinRT `HttpClient`: give it an `HttpBaseProtocolFilter` with
  `CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache)`, or it may serve an old answer.
- First check about 15 seconds after start (not during startup), then every 12 hours.
- Read `tag_name` (`vX.Y.Z`), `html_url`, and the installer from `assets[].browser_download_url` by its name
  pattern.
- Only open URLs that start with `https://github.com/<owner>/<repo>/releases/`, whatever the reply says;
  otherwise open that prefix.
- An environment variable that overrides the URL lets tests serve a fake reply.
- WinUI: an `InfoBar` at the top, with an action button and its close button.
