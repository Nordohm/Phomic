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

Get the free Windows app at nordohm.com/Phomic

• Works in any app on the PC: voice chat, calls, streaming, recording
• Switch between Wi-Fi, Bluetooth and USB while you talk
• Mute from the Lock Screen or the Dynamic Island
• Automatic gain keeps your voice at an even level
• Noise suppression on the PC takes out fans, hiss and typing
• Audio goes straight to your PC, never through the internet
• No account, no ads, no tracking

The Windows app also needs VB-CABLE, a free audio driver by VB-Audio. USB needs Apple Devices from the Microsoft Store.
```

**What's New in This Version** (4,000)

```
New
• A new icon
• Noise suppression with Phomic 1.3 for Windows
• Warns when Mic Mode isn't Standard, so the PC gets your raw sound
• Give Feedback in Settings
• Shows where to get the PC app

Improved
• Errors show at the top and can be swiped away

Fixed
• Finds PCs again after a while in the background
• A session no longer ends when iOS won't restart the microphone in the background
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
https://nordohm.com/Phomic
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
- **Attachment:** none. Apple asks if it needs one.

**Notes** (4,000)

```
Phomic sends the iPhone's microphone to a Windows PC, where it shows up as a microphone for other apps. It needs the free PC app (https://github.com/Nordohm/Phomic/releases) and VB-CABLE, a free audio driver by VB-Audio that the PC app links to. Without the PC app, the list of computers stays empty.

To test:
1. On a Windows 10 or 11 PC, install Phomic, follow its banner to install VB-CABLE, then click Set up.
2. On the iPhone, pick the Wi-Fi or Bluetooth tab and tap the PC. USB: install Apple Devices from the Microsoft Store on the PC, plug the iPhone in and pick the USB tab.
3. Speak: the PC app's level meter moves.
4. Tap the big button, or the Live Activity's button, to mute.

Background modes:
- audio: keeps sending the microphone with the screen locked. While muted, the microphone is released (no orange dot) and a silent, mixable audio session keeps the connection, so the Live Activity can unmute.
- bluetooth-central: reconnects to the PC over Bluetooth after a dropout.
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
