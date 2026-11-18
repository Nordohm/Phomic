# Phomic: App Store product page (English, U.S.)

Text for App Store Connect, field by field. The limits are Apple's; `appstore/check_metadata.py`
checks them.

## App Information

**Name** (30)

```
Phomic
```

**Subtitle** (30)

```
Your iPhone as a PC microphone
```

**Category:** Utilities (secondary: Productivity)

## Version page

**Promotional Text** (170)

```
Turn your iPhone into a microphone for your PC, over Wi-Fi, Bluetooth or a USB cable. Pick it in any app, switch while you talk, and mute from the Lock Screen.
```

**Description** (4,000)

```
Your iPhone as a microphone for your Windows PC, over Wi-Fi, Bluetooth or a USB cable.

• Works in any app on the PC: voice chat, calls, streaming, recording
• Switch between Wi-Fi, Bluetooth and USB while you talk
• Mute from the Lock Screen or the Dynamic Island
• Automatic gain keeps your voice at an even level
• Audio goes straight to your PC, never through the internet
• No account, no ads, no tracking

Needs the free Phomic app for Windows (github.com/Nordohm/Phomic) and VB-CABLE, a free audio driver by VB-Audio. USB also needs Apple Devices from the Microsoft Store.
```

**What's New in This Version** (4,000)

```
• Automatic gain with Phomic 1.2 for Windows
• Smoother mute button
• The glow warms to red as you get loud
• Fixed: Live Activity saying "Phomic stopped"
• Tells you when an update is out
```

**Keywords** (100; words from the name and subtitle are already indexed)

```
mic,wireless,windows,computer,laptop,voice,chat,stream,recording,bluetooth,wifi,usb,cable,podcast
```

**Support URL**

```
https://github.com/Nordohm/Phomic/issues
```

**Marketing URL** (optional)

```
https://github.com/Nordohm/Phomic
```

**Copyright** (200)

```
2026 Nordohm
```

## App Privacy

- **Privacy Policy URL:** `https://github.com/Nordohm/Phomic/blob/main/PRIVACY.md`
- **Data collection:** No, we do not collect data from this app.

The URLs only work once the repository is public.

## App Review Information

- **Sign-in required:** off. Phomic has no accounts.
- **Contact information:** yours.
- **Attachment:** none so far; the notes offer a screen recording of the whole flow (PC app, iPhone app,
  muting from the Lock Screen), since the reviewer probably has no Windows PC to test with.

**Notes** (4,000)

```
Phomic streams the iPhone's microphone to a Windows PC, where it appears as a microphone for other apps. It needs the free companion app on the PC (https://github.com/Nordohm/Phomic/releases) and VB-CABLE, a free virtual audio driver by VB-Audio that the PC app links to. We're happy to send a video of the whole flow if that helps.

Without the PC app, Phomic opens to its main screen and lists no computers.

To test with a PC:
1. Install Phomic on a Windows 10 or 11 PC. Follow its banner to install VB-CABLE, then click Set up.
2. On the iPhone, pick the Wi-Fi or Bluetooth tab and tap the PC. For USB, install Apple Devices from the Microsoft Store on the PC, plug the iPhone in and pick the USB tab: the PC connects by itself.
3. Speak: the level meter in the PC app moves, and the "Phomic" microphone works in any app on the PC.
4. Tap the big button, or the Live Activity's button on the Lock Screen, to mute and unmute.

Background modes:
- audio: while live, Phomic records the microphone in the background, so the PC keeps hearing the user with the screen locked. While muted, the microphone is released (no orange indicator) and a silent, mixable audio session keeps the connection to the PC open, so the user can unmute from the Live Activity without reconnecting.
- bluetooth-central: reconnects to the PC over Bluetooth after a dropout.

No account or sign-in is needed, and the app collects no data.
```

## Assets

Made by `appstore/make_assets.py` from real dark-mode screenshots (1.2, Nov 2026): status bar set to 9:41, and
the PC's name "DESKTOP", cut out of the real name so it stays in the iPhone's font. The Lock Screen slide keeps
the real Live Activity but draws its own wallpaper and clock: the screenshot's wallpaper is someone else's art.

- `out/screenshots/6.1/` 1179 × 2556: iPhone with Dynamic Island (medium), required; App Store Connect scales
  it to the other iPhones
- `out/screenshots/6.9/` 1320 × 2868: iPhone with Dynamic Island (large), if ever wanted
  1. Your iPhone is your PC's mic · 2. Mute from the Lock Screen · 3. Wi-Fi, Bluetooth or USB · 4. Mute with
  one tap · 5. Private by design
- `out/header-universal.png` 5244 × 2950: product page header. It shows small on an iPhone, so it's only the
  headline and the three connections (with the app's own tab bar icons), inside the safe area 1402 × 962 at
  (1921, 660).
