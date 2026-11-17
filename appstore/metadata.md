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
Phomic turns your iPhone into a microphone for your Windows PC, wirelessly or with a cable.

Install the free Phomic app on your PC, open Phomic on your iPhone and tap your computer, or plug the iPhone in. Your iPhone's microphone then shows up on the PC as a microphone called "Phomic", so you can pick it in any app that uses one: voice chat, video calls, streaming and recording.

WI-FI, BLUETOOTH OR USB
• Wi-Fi: uncompressed 48 kHz audio with low latency on your local network.
• Bluetooth: no network needed, within about 10 meters of the PC.
• USB: plug the iPhone into the PC for the lowest latency. It connects by itself.
• Switch between them while you talk.

MUTE FROM ANYWHERE
• One big button: green when you're live, red when you're muted. The screen glows with your voice.
• A Live Activity on the Lock Screen and in the Dynamic Island, with its own mute button.
• Muting fully releases the microphone.

SOUNDS LIKE YOUR IPHONE
• Phomic sends the microphone as it is and adds no processing of its own.
• Choose Standard, Voice Isolation or Wide Spectrum with the iPhone's Mic Mode.

PRIVATE BY DESIGN
• Audio goes straight from your iPhone to your PC, over your own network, Bluetooth or a cable. Nothing goes through the internet.
• No account, no ads, no tracking.
• An optional pairing code keeps other phones out.

WHAT YOU NEED
• A Windows 10 or 11 PC with the free Phomic app: github.com/Nordohm/Phomic
• VB-CABLE, a free virtual audio driver by VB-Audio. The PC app shows you how to get it.
• For USB: Apple Devices, Apple's free app from the Microsoft Store.
```

**What's New in This Version** (4,000)

```
• USB: plug your iPhone into your PC for the lowest latency. Pick the USB tab and Phomic on the PC connects by itself. Needs Phomic 1.1 for Windows and Apple's free Apple Devices app.
• A new look: Wi-Fi, Bluetooth and USB as tabs, Liquid Glass on iOS 26, and the screen glows with your voice while you're live.
• Clearer status: Live, Muted or Connecting, with your PC's name below.
• The Live Activity goes away when you close Phomic.
• Settings shows the app's version, and saved pairing codes are listed as Paired PCs.
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

Made by `appstore/make_assets.py` from real screenshots (status bar set to 9:41, PC renamed "Desktop PC").
The Lock Screen slide keeps the real Live Activity but draws its own wallpaper and clock: the
screenshot's wallpaper is someone else's art.

- `out/screenshots/6.1/` 1179 × 2556: iPhone with Dynamic Island (medium), required
- `out/screenshots/6.9/` 1320 × 2868: iPhone with Dynamic Island (large)
  1. Your iPhone is your PC's mic · 2. Mute from the Lock Screen · 3. Wi-Fi or Bluetooth ·
  4. Private by design
- `out/header-universal.png` 5244 × 2950: product page header and search results
