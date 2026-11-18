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

## Windows: updating in place

Instead of opening the download page, the button can install the update (for a copy the installer set up;
a portable copy still gets the page):

- Take the installer's `digest` (`sha256:<hex>`) from the same API reply, download `browser_download_url`
  (`HttpClient.GetBufferAsync`, with a progress handler for "Downloading… 45 %"), hash it
  (`HashAlgorithmProvider` SHA-256) and refuse to run anything that doesn't match.
- Save it to the temp folder and run it with `ShellExecuteEx`. An Inno Setup per-user installer needs no admin
  rights: `/SILENT /SUPPRESSMSGBOXES /NORESTART` shows only a progress window. Then quit the app.
- Inno skips `postinstall` entries when silent, so add a `[Run]` entry gated on a parameter of your own
  (`Check: Switch('RESTARTAPP')`) to start the new version afterwards.
- Make sure the running app really exits when asked: an installer's Restart Manager sends `WM_QUERYENDSESSION` /
  `WM_ENDSESSION` to its windows, and an app that only hides to the tray makes Setup wait about 30 s and then kill
  it. Handle `WM_ENDSESSION` by quitting, and have Setup's `PrepareToInstall` ask the running copy to quit first.
- On failure, say why and fall back to the download page.
- Test with a fake API reply that points at a real release's installer, once with its digest and once with a wrong
  one, and an environment variable that stops before running it.
