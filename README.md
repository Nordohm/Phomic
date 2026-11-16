# Phomic

Use an iPhone as a microphone for a Windows PC, over Wi-Fi or Bluetooth.

| Part | Tech | Location |
|---|---|---|
| Windows app (receiver) | WinUI 3, C++/WinRT, WASAPI, Winsock, DNS-SD, Bluetooth LE (GATT server) | [`windows/`](windows/) |
| iOS app (sender) | SwiftUI, Core Audio I/O units, Network.framework, CoreBluetooth, built with [xtool](https://github.com/xtool-org/xtool) | [`ios/`](ios/) |

```
                       ┌─ Wi-Fi: 48 kHz PCM, 10 ms UDP packets (+ TCP control) ─┐
iPhone mic ─► RemoteIO                                                           ├─► jitter buffer ─► VB-CABLE ─► "Phomic" mic
  (Mic Mode)  ─────────── Bluetooth LE: 48 kHz Opus, iPhone → PC GATT writes ───┘     (48 kHz)
```

| | Wi-Fi | Bluetooth LE |
|---|---|---|
| Audio | 48 kHz, 16-bit PCM (~0.8 Mbit/s) | 48 kHz Opus, 64 kbit/s (less on small links) |
| Latency | buffer + ~10 ms | buffer + ~20 ms |
| Needs | same network, firewall allowed | Bluetooth on both, within ~10 m |

Wi-Fi and Bluetooth are equals, and only one is used at a time:

- **On the iPhone:** a **Wi-Fi | Bluetooth** switch sits at the top. Changing it while live moves the session to
  the other transport, and the microphone stays on.
- **On the PC:** the *Connection* card lists both, each with its status and an on/off switch. While one is in
  use, the other shows **Switch here**, which asks the iPhone to move over.
- **Newest wins:** if a new connection arrives on either transport, it takes over, and the old one hangs up.
- **You pick the PC on both transports:** the iPhone lists PCs under *Computers*, found with Bonjour on Wi-Fi
  and by each PC's Bluetooth advertisement. Only the PC you pick connects.
- **Optional pairing code:** *Require pairing code* on the PC applies to Wi-Fi and Bluetooth alike. The PC shows a
  4-digit code next to a refresh button (*New code*), and the iPhone asks for it once per PC and remembers it. A new
  code makes every iPhone ask again;
  *Settings* (gear icon) on the iPhone forgets saved codes. The code keeps strangers' phones out; it isn't
  encryption.
- **Buffering:** *Auto* (default) measures how unevenly packets arrive and keeps just enough buffer to cover
  99.5% of it plus 10 ms. It grows at once when the connection gets worse, or after a dropout, and shrinks one
  step at a time after 10 s of calm. It stays between 20 ms (30 ms on Bluetooth) and 200 ms. The fixed choices
  are 30, 60 and 120 ms; on Bluetooth they never go below 60 ms.

## Install (Windows)

1. Download `Phomic-Setup-<version>.exe` (or the portable `.zip`) from the releases and run it. It installs for your
   user only; no admin rights needed. Windows may warn that the publisher is unknown (the app isn't code-signed):
   choose *More info › Run anyway*.
2. Start Phomic. If the microphone driver isn't there yet, the banner's **Get VB-CABLE** opens VB-Audio's page:
   download VB-CABLE, unzip it, run `VBCABLE_Setup_x64.exe` as administrator and click *Install Driver*
   (restart if Windows asks). Back in Phomic, **Set up** names it "Phomic".
3. When Windows Firewall asks, allow Phomic on **private networks**.
4. Install Phomic on your iPhone (see [iOS app](#ios-app)) and pick your PC. In Discord, Teams, OBS and so on,
   choose the microphone **Phomic**.

Uninstalling (*Settings › Apps*) also gives the VB-CABLE devices their original names back and removes Phomic's
settings. VB-CABLE itself stays installed; remove it with its own setup program if you don't need it.

## Support

Phomic is free and open source (MIT). If it's useful to you, you can
[buy me a coffee](https://buymeacoffee.com/nordohm) ☕. VB-CABLE, which the Phomic microphone runs on, is
donationware by VB-Audio: you can support them at [vb-cable.com](https://vb-cable.com).

## Making it show up as a microphone in Windows

Windows only lets **kernel audio drivers** create recording devices; there is no user-mode API for a
virtual microphone, and Windows 11 only loads kernel drivers that Microsoft has signed. Phomic therefore
relies on **[VB-CABLE](https://vb-cable.com)**, a Microsoft-signed virtual audio cable by VB-Audio. It's
donationware, and all participations are welcome. Phomic plays the iPhone's audio into one end of the cable,
and other apps record from the other end.

Phomic doesn't ship VB-CABLE: people install it from VB-Audio themselves, so Phomic doesn't redistribute VB-Audio's
software (see [`windows/third_party/README.md`](windows/third_party/README.md)). The PC app guides through it from
its banner:

1. **Get VB-CABLE:** opens VB-Audio's download page. Unzip the download, run `VBCABLE_Setup_x64.exe` as
   administrator and click *Install Driver*. Sometimes Windows wants a restart before the device appears; Phomic
   notices it by itself.
2. **Set up:** applies Windows' own per-device settings (the same as *Settings › Sound › Rename / Don't allow*).
   No admin rights are needed:

   | VB-CABLE endpoint | Becomes | Why |
   |---|---|---|
   | CABLE Output (recording) | **Phomic** | the only one left: pick it as the microphone in Discord, Teams, OBS and so on |
   | CABLE Input (playback) | renamed "Phomic (internal)", **disabled** | Phomic writes into the driver directly (below) |
   | CABLE In 16 Ch (playback) | **disabled** | unused |

**How Phomic plays into a disabled device:** the PC app doesn't go through a Windows playback endpoint at all.
`CableOutput` opens VB-CABLE's render pin with kernel streaming. VB-CABLE is a WaveRT driver, so the
driver maps its cyclic audio buffer into Phomic's process, and Phomic writes 20 ms ahead of the driver's play
position. A playback endpoint can be disabled while its driver pin keeps working. This was measured
with the endpoint disabled: the audio arrives on "Phomic" bit-exact in level and frequency. It also skips one
buffering stage in the Windows audio engine. Under *Play the iPhone's audio into*, the first option, **Phomic
microphone**, is this direct path. Your speakers are still listed if you want to listen in.

Apps that show the driver name will list it as "Phomic (VB-Audio Virtual Cable)"; the driver label can't be
changed without modifying the signed package. To undo step 2 (original names, both playback endpoints enabled),
run `Phomic.exe --restore-microphone`, from an administrator prompt if Windows asks for one.

Open-source virtual audio drivers such as Scream and VirtualDrivers' Virtual Audio Driver aren't an
alternative: on Windows 11 they only load in Test Mode. A dedicated Phomic driver would need Microsoft's
signature, which requires a paid EV code-signing certificate.

## Windows app

Requirements: Visual Studio 2026 with the *WinUI application development* (C++) workload. You don't
need to create a project in Visual Studio; it's already in `windows/`.

- Open `windows/Phomic.slnx` and press F5, **or** build from a terminal:

  ```bash
  msbuild windows/Phomic.slnx -t:restore -p:RestorePackagesConfig=true
  msbuild windows/Phomic.slnx -p:Configuration=Release -p:Platform=x64
  ```

- The output is unpackaged and self-contained: `windows/x64/Release/Phomic/` holds `Phomic.exe` plus the
  Windows App SDK runtime, and you can copy that folder anywhere. Nothing needs installing or signing.
- **A release:** `powershell -ExecutionPolicy Bypass -File windows/build-release.ps1` builds Release and writes
  `windows/release/Phomic-Setup-<version>.exe` and `Phomic-<version>-portable.zip` (no debug symbols). It needs
  [Inno Setup 7](https://jrsoftware.org/isdl.php). The version lives in `windows/Phomic/Version.h` (exe version info,
  *Show details* and the installer all take it from there). The installer script is `windows/installer/Phomic.iss`:
  per-user install, Start-menu shortcut; its uninstaller takes `/KEEPSETTINGS=1` and `/KEEPMICROPHONE=1`.
- On first launch, Windows Firewall asks whether Phomic may accept connections. Allow it on **private
  networks**. Phomic listens on **TCP and UDP port 50505**.
- Bluetooth needs no pairing. With the **Bluetooth** switch on, the PC publishes Phomic's Bluetooth service and
  waits: the iPhone connects to it, the way a phone connects to headphones. The PC never scans or retries.
- **Tray:** Phomic has a tray icon (click: open the window; right-click: *Open* / *Quit*; the tooltip shows
  the status). At the bottom of the window:
  - *Keep app running in tray when closing* makes the close button hide the window. *Quit* in the tray menu
    exits.
  - *Launch app in tray when Windows starts* adds Phomic to your sign-in apps (`HKCU\…\Run`, no admin rights). It
    starts with the window hidden (`Phomic.exe --tray`). Task Manager's *Startup apps* can turn it off too.

  Starting Phomic again while it runs opens the existing window.
- The window opens at its full width (680 logical pixels; it can't be narrower than 440) and is never taller than
  its content. It follows the content (for example when *Show details* opens) unless you made it shorter yourself.
- *Show details* under *Connection* lists the stream (link, audio format, packets and loss, buffer,
  interruptions) while an iPhone is connected, and this PC's name, addresses, port and Bluetooth state.
- Icon: an iPhone as the capsule of a microphone, drawn on a 24-pixel grid so it stays readable at taskbar size. On
  Windows it's the glyph alone (no tile). Source: `windows/Phomic/Assets/Phomic.svg`; `Phomic.ico` is rendered from it.
  The iOS app uses `Resources/AppIcon.png` (from `AppIcon.svg`, white on blue, via `iconPath` in `xtool.yml`).

Code map:

| File | What it does |
|---|---|
| `StreamServer.*` | Wi-Fi: TCP control + UDP audio on one background thread (`WSAPoll`), Bonjour registration via `DnsServiceRegister` |
| `BluetoothReceiver.*` | Bluetooth LE: GATT server and its advertisement (iPhones connect to it), Opus decode and loss concealment |
| `Adpcm.h`, `Resampler.*` | Fallback IMA-ADPCM decoder; polyphase windowed-sinc upsampler (24/16 kHz → 48 kHz) |
| `SourceArbiter.h` | Decides whether Wi-Fi or Bluetooth feeds the audio pipeline |
| `JitterBuffer.*` | Reorders packets, hides loss, absorbs jitter (fixed or Auto), and corrects phone/PC clock drift by resampling ±0.5% |
| `CableOutput.*` | Direct kernel-streaming (WaveRT) output into VB-CABLE's driver pin, so its playback endpoints can stay disabled |
| `VirtualMic.*` | Finds VB-CABLE's endpoints (grouped per cable device), renames/disables them, launches VB-CABLE's installer |
| `AudioOutput.*` | Event-driven WASAPI shared-mode playback (for listening on real speakers), device list, hot-plug watcher |
| `MainWindow.*` | UI |
| `TrayIcon.*`, `Startup.h` | Tray icon and menu; *Start with Windows* (Run key) |
| `Settings.*` | Preferences in `HKCU\Software\Phomic` |

## iOS app

Built with [xtool](https://github.com/xtool-org/xtool): `ios/` is a SwiftPM package (`Package.swift`) plus
`xtool.yml`, not an Xcode project. Install xtool (see its README), then in `ios/`:

```bash
xtool dev            # build, sign, install and launch on the connected iPhone
xtool dev build      # just compile (unsigned)
```

- **On a Mac,** xtool uses Xcode's iOS SDK. It can also generate an Xcode project
  (`xtool dev generate-xcode-project`) if you'd rather work in Xcode. xtool runs on Linux and Windows too, but
  there it needs the iOS SDK extracted from Xcode, and Apple's Xcode license only allows that SDK on Apple
  computers.
- **App ID:** `io.github.nordohm.Phomic` (`xtool.yml`). An App ID belongs to one developer team, so change it to
  your own before you build. With a free Apple account, xtool's signature lasts 7 days, so run `xtool dev` again
  after that; a paid developer account makes it last a year.

iOS asks for **Microphone**, **Local Network** (Wi-Fi) and **Bluetooth** (only when you use it) access.

- **Wi-Fi:** tap the PC under *Computers*, or use *Connect by Address* with an address from *Show details*
  under *Connection* in the PC app.
- **Bluetooth:** set the switch at the top to *Bluetooth* and tap your PC under *Computers*. The iPhone connects
  to that PC only. If the link drops, iOS keeps a pending connection and reconnects as soon as the PC is back in
  range, also with Phomic in the background.
- **Pairing code:** a lock icon marks PCs that require one. Phomic asks for it the first time, or again after a *New code*.
  *Settings* (gear icon) lists the PCs whose code is saved: swipe one to forget it, or *Forget All Codes*.
- **Mute:** the big button is green when live and red when muted. Muting fully releases the microphone (the
  orange dot disappears). Phomic keeps silent, mixable playback
  running so iOS doesn't suspend it, and your music isn't interrupted.
- **Processing:** Phomic records the microphone raw (RemoteIO, measurement mode, no automatic gain) and adds no
  processing of its own. The iPhone's **Mic Mode** decides: *Standard*, *Voice Isolation* (removes background
  noise) or *Wide Spectrum* (keeps everything). Change it in Control Center, or with the *Processing* row under the
  mute button while live, which opens the system picker. Apps can only read the setting, not change it. For a gate, compressor or noise
  suppression beyond that, use what Discord, Teams or OBS already have on the PC.
- **Live Activity:** while connected, the lock screen and Dynamic Island show *Live* (green), *Muted* (red) or
  *Reconnecting…*, with the PC's name and Wi-Fi or Bluetooth. Tap the microphone to mute or unmute without
  opening Phomic; tap elsewhere to open it. It's a widget extension (`Sources/PhomicWidget`, `PhomicWidget-Info.plist`);
  `Sources/PhomicActivity` holds what it shows and `ToggleMuteIntent`, the button's App Intent (also in Shortcuts as
  *Toggle Mute*). It's both a `LiveActivityIntent` (runs in the app) and an `AudioRecordingIntent` (may turn the
  microphone on from the background), hence iOS 18 or later.
- **App Intents metadata:** iOS only runs an intent that the bundle's `Metadata.appintents` describes. Xcode
  generates that during every build; xtool doesn't. So `Tools/appintents-metadata.sh` generates it with
  [re-appintentsmetadataprocessor](https://codeberg.org/viraptor/re-appintentsmetadataprocessor), an open Rust
  reimplementation of Apple's tool, into `Resources/Metadata.appintents`, which `xtool.yml` copies into the app and
  the widget. The result is committed, so you only need the script after changing `ToggleMuteIntent` (it expects
  xtool's Darwin SDK and the tool; see the top of the script). Xcode builds (below) generate their own; there,
  `PhomicActivityIntents` (an `AppIntentsPackage`) is how the app and the widget include the intent.
- **Background:** streaming continues with the screen locked. After a Wi-Fi drop it retries every 2 s; after a
  Bluetooth drop iOS reconnects by itself (background mode `bluetooth-central`).

### App Store builds (no Mac needed)

App Store Connect only takes apps built with Xcode, and Xcode only runs on a Mac, so GitHub's Macs do it:
[`.github/workflows/ios.yml`](.github/workflows/ios.yml) (*Actions › iOS app › Run workflow*) turns the package
into an Xcode project ([`ios/Xcode/project.yml`](ios/Xcode/project.yml), with
[XcodeGen](https://github.com/yonaskolb/XcodeGen)) and builds it. With **Upload** off it only checks that it builds;
with it on, it signs the app and uploads it to App Store Connect, where it appears under TestFlight. The build
number is the workflow's run number + 10. The App Store build is iPhone-only; iPads run it as an iPhone app.

One-time setup for uploads (needs the paid Apple Developer Program):

1. developer.apple.com › *Identifiers*: register `io.github.nordohm.Phomic` and
   `io.github.nordohm.Phomic.PhomicWidget` as App IDs (no capabilities needed).
2. Run `bash ios/Tools/app-store-signing.sh csr` (Git Bash works). Upload the `request.csr` it makes under
   *Certificates › + › Apple Distribution*, download the `.cer`, then run
   `bash ios/Tools/app-store-signing.sh p12 <the .cer>`.
3. *Profiles › + › App Store Connect*: one profile per App ID, with that certificate. Download both, then run
   `bash ios/Tools/app-store-signing.sh profiles <app profile> <widget profile>`.
4. App Store Connect: create the app with bundle ID `io.github.nordohm.Phomic`. Under *Users and Access ›
   Integrations*, create an API key (role *App Manager*) and download its `AuthKey_<key ID>.p8`.
5. Run `bash ios/Tools/app-store-signing.sh secret <the AuthKey .p8> <issuer ID>`. It packs the certificate, both
   profiles and the API key into `~/phomic-signing/secret.txt`. On GitHub › *Settings › Secrets and variables ›
   Actions*, add it as the one secret `APPLE_SIGNING`. Keep `~/phomic-signing` private.

The workflow keeps the certificate's name, the team ID and the key IDs out of its log. The App Store version is
`MARKETING_VERSION` in `ios/Xcode/project.yml`. The product page's text and images are in
[`appstore/`](appstore/metadata.md).

Code map: `MicController.swift` (state, lifecycle, pairing codes), `AudioCapture.swift` (Core Audio I/O,
real-time ring buffer, framing), `StreamClient.swift` (Wi-Fi), `BluetoothLink.swift` (connects to the PC over Bluetooth),
`BluetoothScanner.swift` (finds PCs over Bluetooth), `BluetoothAudio.swift` (Opus encoder; ADPCM fallback),
`ServerBrowser.swift` (Bonjour), `WireProtocol.swift`, `ContentView.swift` (all screens, including Settings),
`Acknowledgements.swift` (the Opus license notice). `Sources/COpus` builds libopus.

## Wi-Fi protocol (version 1)

**Discovery:** the PC registers DNS-SD service `<computer name>._phomic._tcp.local` on port 50505, with TXT
`v=1`, `id=<PC id>` (8 hex digits, the same id as over Bluetooth) and `pair=1|0` (whether a code is required).

**Control:** TCP to port 50505, one UTF-8 JSON object per line (`\n`).

| Direction | Message | Notes |
|---|---|---|
| phone → PC | `{"type":"hello","version":1,"name":"iPhone","sampleRate":48000,"channels":1,"frameSamples":480,"code":"1234"}` | must be the first line, within 5 s; `code` only if the phone knows one |
| PC → phone | `{"type":"welcome","version":1,"name":"MY-PC","pcId":"3A7C91E2","udpPort":50505,"session":123456789}` | |
| PC → phone | `{"type":"error","reason":"pairing","pcId":"…","name":"MY-PC","message":"Wrong pairing code."}` | missing or wrong code; the phone asks the user for it |
| PC → phone | `{"type":"error","message":"…"}` | any other refusal; then closes, and the phone doesn't auto-retry |
| phone → PC | `{"type":"ping","id":7,"rtt":9.8}` | every second; `rtt` is the last measured round trip in ms |
| PC → phone | `{"type":"pong","id":7}` | |
| phone → PC | `{"type":"mute","muted":true}` | the phone stops sending audio while muted |
| PC → phone | `{"type":"switch","transport":"bluetooth"}` | the PC's *Switch here*: the phone hangs up Wi-Fi and starts Bluetooth |
| either | `{"type":"bye"}` | clean hang-up |

The PC drops a phone it hasn't heard from in 6 s, and the phone gives up after 5 s without a reply.

**Audio:** UDP from the phone to `udpPort` on the address the TCP connection resolved to. Each datagram
is 972 bytes, little-endian: `"PHMC"` magic (4) · `session` (4) · sequence number (4, wraps) · 480 × int16 PCM
(mono, 48 kHz, 10 ms).

## Bluetooth LE protocol (version 2)

The PC is the GATT **server** (peripheral) and the iPhone is the **client** (central): the iPhone connects to
the PC, the way it connects to headphones. Version 1 had the roles reversed. Then the PC had to scan for the
iPhone and retry, which was slow, and iOS barely advertises in the background, so it couldn't reconnect. There's
no OS-level Bluetooth pairing or encryption; the optional pairing code works the same as over Wi-Fi.

The PC sends one advertisement (many adapters can only send one at a time): its Phomic service, connectable,
under a UUID that carries the PC id and whether it wants a pairing code. Windows adds the PC's Bluetooth name.
That's all an iPhone needs to list the PC (with its lock icon) and to find exactly the PC the user picked.

| UUID | What | Properties |
|---|---|---|
| `8E5A1C6<c>-3B7D-4F62-9A1E-6C2D<PC id>` | service; `c` = `1` if a pairing code is required, else `0`; e.g. `8E5A1C60-…-6C2D3A7C91E2` for PC `3A7C91E2` | advertised |
| `8E5A1C51-3B7D-4F62-9A1E-6C2D4B8F0A11` | audio | write without response |
| `8E5A1C52-3B7D-4F62-9A1E-6C2D4B8F0A11` | control (UTF-8 JSON) | write, notify |

When the pairing setting changes, the PC republishes the service under the other UUID once no iPhone is
connected over Bluetooth.

1. The iPhone connects to the peripheral it listed the PC from, and also scans for both variants of that PC's
   service UUID, in case the PC's address changed.
2. It subscribes to control, then writes `{"type":"hello","v":2,"name":"iPhone","muted":false,"code":"1234"}`.
3. The PC notifies `{"type":"welcome","v":2,"name":"MY-PC","pcId":"3A7C91E2"}`, or
   `{"type":"error","reason":"pairing","pcId":"…","name":"MY-PC","message":"…"}`. After an error, the iPhone
   hangs up and asks for the code.
4. Audio: one write without response per 10 ms frame (format below). The iPhone writes
   `{"type":"state","name":"iPhone","muted":true}` when it mutes or unmutes.
5. Hanging up: the iPhone writes `{"type":"bye"}` and disconnects. The PC notifies `{"type":"bye"}` (optionally with
   `"reason"`), and the iPhone disconnects and doesn't come back. The newest iPhone wins: when a second one says
   hello, the first gets a `bye`.
6. The PC's *Switch here* notifies `{"type":"switch","transport":"wifi"}`. The phone stops Bluetooth and connects
   to the same PC over Wi-Fi: it looks the PC up by id or name with Bonjour, falling back to the last Wi-Fi address.

Windows can list the iPhone's subscription a moment after its `hello` arrives, so the PC waits up to 2 s for it
before notifying `welcome`. The iPhone says `hello` again if no `welcome` came within 2.5 s, and starts over after 7 s.

If the link drops without a `bye`, the iPhone keeps a pending connection to the PC, and iOS completes it as soon
as the PC is reachable. The PC notices the drop when the session closes, the iPhone unsubscribes, or audio stops
for 10 s while not muted.

Each audio write is one 10 ms frame, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | format: `0xB1` = Opus |
| 1 | 2 | sequence number (uint16, wraps) |
| 3 | rest | one Opus packet: 48 kHz mono, 10 ms (480 samples), CELT-only (`RESTRICTED_LOWDELAY`) |

The encoder runs at 64 kbit/s with constrained VBR, so a packet is about 80 bytes. When the link's maximum
write size is smaller, the iPhone lowers the bitrate until each packet fits. For a missing sequence
number, the PC lets Opus conceal the gap (up to 5 frames) before it decodes the next packet. Opus adds 2.5 ms of
delay.

Fallback formats, which the PC still accepts: `0xA1` / `0xA2` are IMA-ADPCM at 24 / 16 kHz. Bytes 3–4 hold the
predictor (int16), byte 5 the step index (0–88), and the rest holds 4-bit codes, two samples per byte, low
nibble first. The iPhone sends them only if it can't create an Opus encoder.

## Troubleshooting

- **The PC doesn't appear on the phone (Wi-Fi).** Both devices must be on the same network. Check
  *Settings › Privacy & Security › Local Network › Phomic* on the iPhone, or connect manually.
- **"No audio is arriving" (Wi-Fi).** Windows Firewall is blocking UDP 50505. Allow Phomic on private networks
  (*Windows Security › Firewall › Allow an app*), and make sure the network is set to *Private*.
- **Bluetooth never connects.** The *Bluetooth* switch must be on under *Connection* in the PC app. If the PC
  says it can't accept connections, its Bluetooth adapter doesn't support the peripheral role: use Wi-Fi.
  *Show details* shows the connection interval once connected.
- **Crackles or dropouts.** Leave *Buffering* on *Auto*, or pick *Stable (120 ms)*. *Show details* under
  *Connection* shows packet loss, the current buffer and rebuffer counts.
- **Too quiet in Raw mode.** Raise *Gain* in the PC app.
