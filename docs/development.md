# Development

How the apps are built and how they work inside. The protocols between them are in
[protocols.md](protocols.md). General notes that aren't about this app (releasing, platform pitfalls, legal,
writing) are in [`.claude/skills/`](../.claude/skills/), one folder per topic.

| Part | Tech | Location |
|---|---|---|
| Windows app (receiver) | WinUI 3, C++/WinRT, WASAPI, Winsock, DNS-SD, Bluetooth LE (GATT server), usbmux | [`windows/`](../windows/) |
| iOS app (sender) | SwiftUI, Core Audio I/O units, Network.framework, CoreBluetooth, built with [xtool](https://github.com/xtool-org/xtool) | [`ios/`](../ios/) |

## Connections

Wi-Fi, Bluetooth and USB are equals, and only one is used at a time:

- **On the iPhone:** **Wi-Fi**, **Bluetooth** and **USB** are the tabs at the bottom (Liquid Glass on iOS 26).
  Changing tab while live moves the session to the other transport, and the microphone stays on. Picking USB ends the wireless session at once; with the
  cable in, the PC reaches the iPhone through it a fraction of a second later and a new session starts.
- **On the PC:** the *Connection* card lists all three, each with its status and an on/off switch. While one is in
  use, the others show **Switch**, which asks the iPhone to move over.
- **Newest wins:** if a new connection arrives on any transport, it takes over, and the old one hangs up.
- **You pick the PC:** over Wi-Fi and Bluetooth the iPhone lists PCs in that tab, found with Bonjour on Wi-Fi
  and by each PC's Bluetooth advertisement, and only the PC you pick connects. Over USB it's the PC at the other
  end of the cable.
- **Optional pairing code:** *Require pairing code* on the PC applies to Wi-Fi and Bluetooth alike. The PC shows a
  4-digit code next to a refresh button (*New code*), and the iPhone asks for it once per PC and remembers it. A new
  code makes every iPhone ask again;
  *Settings* (gear icon) on the iPhone forgets saved codes. The code keeps strangers' phones out; it isn't
  encryption. USB doesn't ask for it: plugging the iPhone in, and its *Trust This Computer?* prompt, already show
  whose iPhone it is.
- **Buffering:** *Automatic* (default) measures how unevenly packets arrive and keeps just enough buffer to cover
  99.5% of it plus 10 ms. It grows at once when the connection gets worse, or after a dropout, and shrinks one
  step at a time after 10 s of calm. It stays between 20 ms (30 ms on Bluetooth) and 200 ms. The fixed choices
  are 30, 60 and 120 ms; on Bluetooth they never go below 60 ms.

## Making it show up as a microphone in Windows

Windows only lets **kernel audio drivers** create recording devices; there is no user-mode API for a
virtual microphone, and Windows 11 only loads kernel drivers that Microsoft has signed. Phomic therefore
relies on **[VB-CABLE](https://vb-cable.com)**, a Microsoft-signed virtual audio cable by VB-Audio. It's
donationware, and all participations are welcome. Phomic plays the iPhone's audio into one end of the cable,
and other apps record from the other end.

Phomic doesn't ship VB-CABLE: people install it from VB-Audio themselves, so Phomic doesn't redistribute VB-Audio's
software (see [`windows/third_party/README.md`](../windows/third_party/README.md)). The PC app guides through it from
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
  *Show details*, the line at the bottom of the window and the installer all take it from there). The installer
  script is `windows/installer/Phomic.iss`:
  per-user install, Start-menu shortcut. The uninstaller closes a running Phomic from that folder (`Phomic.exe
  --quit`: like *Quit* in the tray menu, ended after 5 s if need be), gives VB-CABLE its names back, deletes the
  settings, and finally asks whether to remove VB-CABLE too (default no; it opens VB-Audio's setup, which needs
  admin rights). It takes `/KEEPSETTINGS=1` and `/KEEPMICROPHONE=1` (which also skips that question).
- On first launch, Windows Firewall asks whether Phomic may accept connections. Allow it on **private
  networks**. Phomic listens on **TCP and UDP port 50505**. A no makes block rules for that path and Windows
  doesn't ask again: the Wi-Fi row then says the firewall blocks iPhones, and **Allow** fixes it after a UAC
  prompt (Phomic again, elevated, with `--allow-firewall`: its block rules become allow rules, plus a rule named
  "Phomic" for the current kind of network if none covers it). The window rereads the rules every 10 s while it's
  open, so a fix in Windows' own settings shows too. `PHOMIC_FIREWALL_EXE=<path>` makes it check another copy's
  rules (for testing).
- Bluetooth needs no pairing. With the **Bluetooth** switch on, the PC publishes Phomic's Bluetooth service and
  waits: the iPhone connects to it, the way a phone connects to headphones. The PC never scans or retries.
- **USB** goes through Apple's USB service (*usbmux*, from the [Apple Devices](https://apps.microsoft.com/detail/9NP83LWLPZ9K)
  app, or Apple Mobile Device Support from iTunes) on `127.0.0.1:27015`. With the **USB** switch on, the PC
  watches for iPhones being plugged in and tries to connect to each one a few times a second until Phomic on it
  answers, so picking USB on the iPhone takes effect at once.
  When USB can't work, the row says why, with one button that fixes it where Phomic can (`AppleUsb.*` finds out,
  without admin rights):

  | The row says | Phomic checks | Button |
  |---|---|---|
  | Needs Apple Devices | Apple Devices isn't installed, nor Apple Mobile Device Support | **Get Apple Devices** (Microsoft Store) |
  | Waiting for Apple's USB service… | Apple Devices is installed, but nothing answers on port 27015 yet. After 20 s Phomic starts the service itself, without a window | — |
  | Apple's USB service didn't start | that didn't help either | **Open Apple Devices** (opening it starts the service) |
  | … Apple Devices is off under *Startup* | its startup task is turned off, so Phomic doesn't start it uninvited | **Start it** (then **Open Apple Devices** if that didn't help) |
  | A Windows port forward blocks Apple's USB service | a `netsh interface portproxy` rule listens on 27015 | **Copy command** (the `netsh … delete` to run as administrator) |
  | *Program* uses port 27015 | another program listens there | — |
  | Apple Mobile Device Service is stopped | only the older Apple Mobile Device Support is installed, and its service isn't running | **Get Apple Devices** |
  | Apple's USB service has lost track of the iPhone | Windows' USB device list has more iPhones than Apple's service, for 6 s. It happens when the service restarts while an iPhone is plugged in, and only plugging it in again fixes it: Windows won't let a program reset a USB device without admin rights | — (unplug, wait, plug back in) |
  | Apple's USB service can't reach the iPhone | Apple's service lists the iPhone but closes every connection to it without an answer, for 5 s (seen once, even for the iPhone's own system port) | — (plug in again; if that doesn't help, restart the iPhone) |

  If Apple's service stops during a session (Apple Devices updating, say), the session ends with that reason and
  Phomic connects again once the service is back. To start the service without a window, Phomic activates the
  background agent in the Apple Devices package (`AMPDevicesAgent`, a COM server), which starts it the way the
  Apple Devices window does. That's undocumented, hence the button as a fallback. A service started while an
  iPhone was already plugged in sometimes misses it; then the row asks to plug it in again.
- **Tray:** Phomic has a tray icon (click: open the window; right-click: *Open* / *Quit*; the tooltip shows
  the status). At the bottom of the window:
  - *Keep app running in tray when closing* makes the close button hide the window. *Quit* in the tray menu
    exits.
  - *Launch app in tray when Windows starts* adds Phomic to your sign-in apps (`HKCU\…\Run`, no admin rights). It
    starts with the window hidden (`Phomic.exe --tray`). Task Manager's *Startup apps* can turn it off too.

  Starting Phomic again while it runs opens the existing window.
- The window is 600 logical pixels wide and never taller than its content. It follows the content (for example
  when *Show details* opens) unless you made it shorter yourself.
- **Gain:** the iPhone sends its microphone raw, which is quiet, and how quiet depends on the voice, the distance
  and the Mic Mode. *Automatic gain* (on by default, `AutoGain.h`) keeps speech at an even level, about -23 dBFS:
  it tracks the room's background noise, counts what's clearly above it as speech, turns down quickly when that's
  too loud and up slowly, and holds still in pauses, so the background doesn't swell up when nobody talks (0 to
  +30 dB; it starts where it was last time). Turn it off to set *Gain* yourself (-12 to +30 dB, 0 = as the iPhone
  sends it). Either way a limiter keeps the result under -1 dBFS: loud parts are turned down for a moment instead
  of clipping, and the level shows *limiting* while it does.
- **Updates:** shortly after starting and then twice a day, Phomic asks GitHub for its latest release. If that's
  newer, a banner at the top offers the installer; closing it hides it until the next version. (For testing,
  `PHOMIC_UPDATE_URL` points the check at another URL.)
- *Show details* under *Connection* lists the stream (link, audio format, packets and loss, buffer,
  interruptions) while an iPhone is connected, and this PC's name, addresses, port and Bluetooth state.
- Icon: an iPhone as the capsule of a microphone, drawn on a 24-pixel grid so it stays readable at taskbar size.
  Windows shows it in two forms: white on a blue squircle like the iPhone app (`PhomicTile.ico`: Explorer, Start,
  taskbar, Alt+Tab, the installer) and the glyph alone (`Phomic.ico`: title bar and tray). Source:
  `windows/Phomic/Assets/Phomic.svg`; `python windows/make-tile-icon.py` makes `PhomicTile.svg` and `.ico` from it.
  The iOS app uses `Resources/AppIcon.png` (from `AppIcon.svg`, white on blue, via `iconPath` in `xtool.yml`).

- **Tests** (`windows/tests/`, Python 3, nothing to install) play the iPhone against the running PC app on the
  same computer, and read its window through UI Automation:
  - `python -I windows/tests/fake_iphone_wifi.py` runs the Wi-Fi protocol: refusals (version, audio format,
    pairing code), the hello timeout, ping, audio and mute, bye, lost contact, newest wins, *Disconnect*,
    *Switch*, and automatic gain settling on speech-like audio.
  - `python -I windows/tests/fake_usbmux.py` stands in for Apple's USB service with fake iPhones: start Phomic with
    the environment variable `PHOMIC_USBMUX_PORT=27115` (and USB on) and it talks to the fake instead. It covers
    retries and how fast a listening iPhone is picked up, bad data, unplugging, Wi-Fi and USB taking over from each
    other, and Apple's service going away and coming back.

  Both take test names to run only some. They take over the PC app's connection while they run.

Code map:

| File | What it does |
|---|---|
| `StreamServer.*` | Wi-Fi: TCP control + UDP audio on one background thread (`WSAPoll`), Bonjour registration via `DnsServiceRegister` |
| `BluetoothReceiver.*` | Bluetooth LE: GATT server and its advertisement (iPhones connect to it), Opus decode and loss concealment |
| `UsbReceiver.*` | USB: watches Apple's usbmux service for plugged-in iPhones and connects to Phomic on them through the cable |
| `AppleUsb.*` | Finds out why USB can't work: Apple Devices installed and on at startup, who holds port 27015, iPhones Windows sees |
| `Firewall.*` | Whether Windows Firewall lets iPhones reach Phomic on the current network, and the fix (run elevated) |
| `Adpcm.h`, `Resampler.*` | Fallback IMA-ADPCM decoder; polyphase windowed-sinc upsampler (24/16 kHz → 48 kHz) |
| `SourceArbiter.h` | Decides whether Wi-Fi, Bluetooth or USB feeds the audio pipeline |
| `JitterBuffer.*` | Reorders packets, hides loss, absorbs jitter (fixed or Auto), and corrects phone/PC clock drift by resampling ±0.5% |
| `CableOutput.*` | Direct kernel-streaming (WaveRT) output into VB-CABLE's driver pin, so its playback endpoints can stay disabled |
| `VirtualMic.*` | Finds VB-CABLE's endpoints (grouped per cable device), renames/disables them, launches VB-CABLE's installer |
| `AudioOutput.*` | Event-driven WASAPI shared-mode playback (for listening on real speakers), device list, hot-plug watcher |
| `AutoGain.h`, `Limiter.h` | Automatic gain; applying the gain and keeping the result under -1 dBFS |
| `MainWindow.*` | UI, including the update check |
| `App.xaml` | Shared styles, and a CheckBox template with a plain check mark (WinUI's animated one sometimes showed a stray "-") |
| `TrayIcon.*`, `Startup.h` | Tray icon and menu; *Start with Windows* (Run key) |
| `Settings.*` | Preferences in `HKCU\Software\Phomic` |

## iOS app

To build it yourself:

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
- **SDK version:** xtool's linker records the deployment target (18.0) as the SDK version, and iOS then shows the
  app as if it were built for iOS 18 (no Liquid Glass). `Package.swift` passes the linker the real SDK version
  (27.2) instead; update it there when the SDK changes.
- **App ID:** `io.github.nordohm.Phomic` (`xtool.yml`). An App ID belongs to one developer team, so change it to
  your own before you build. With a free Apple account, xtool's signature lasts 7 days, so run `xtool dev` again
  after that; a paid developer account makes it last a year.

iOS asks for **Microphone**, **Local Network** (Wi-Fi) and **Bluetooth** (only when you use it) access.

- **Wi-Fi:** tap your PC in the *Wi-Fi* tab, or use *Connect by Address* with an address from *Show details*
  under *Connection* in the PC app.
- **Bluetooth:** pick the *Bluetooth* tab and tap your PC. The iPhone connects
  to that PC only. If the link drops, iOS keeps a pending connection and reconnects as soon as the PC is back in
  range, also with Phomic in the background.
- **USB:** pick the *USB* tab and plug the iPhone into the PC. While Phomic is on screen the iPhone waits, and the
  PC connects to it through the cable; once connected it carries on with the screen locked. Unplugging pauses the session
  and plugging back in resumes it.
- **Pairing code:** a lock icon marks PCs that require one. Phomic asks for it the first time, or again after a *New code*.
  *Settings* (gear icon) lists them under *Paired PCs*: swipe one to forget it, or *Forget All PCs*.
- **Version:** at the bottom of *Settings*, read from the app's Info.plist, so it's always the installed one
  (`MARKETING_VERSION` on App Store builds).
- **Updates:** when the App Store has a newer version, the list of PCs shows a card that opens it (Phomic asks
  Apple's lookup service when it comes on screen, at most every 6 hours). Closing the card hides it until the next
  version.
- **Mute:** the big button is green when live and red when muted; while live, the screen glows green from the button outwards with your voice, warming through yellow and orange to red as the sound nears full scale, and fades to a still red when muted. Switching the microphone on or off runs off the main thread, so the button animates without stutter. Muting fully releases the microphone (the
  orange dot disappears). Phomic keeps silent, mixable playback
  running so iOS doesn't suspend it, and your music isn't interrupted.
- **Processing:** Phomic records the microphone raw (RemoteIO, measurement mode, no automatic gain) and adds no
  processing of its own. The iPhone's **Mic Mode** decides: *Standard* (unprocessed) or *Voice Isolation* (removes
  background noise). iOS offers *Wide Spectrum* only in call apps such as FaceTime, where *Standard* is processed for
  calls; in a recording app like Phomic, *Standard* already keeps everything. Change it in Control Center, or with the *Mic Mode* button under the
  mute button while live, which opens the system picker. Apps can only read the setting, not change it. For a gate, compressor or noise
  suppression beyond that, use what Discord, Teams or OBS already have on the PC.
- **Live Activity:** while connected, the lock screen and Dynamic Island show *Live* (green), *Muted* (red) or
  *Reconnecting…*, with the PC's name and Wi-Fi, Bluetooth or USB. Swiping Phomic away in the app switcher ends it.
  (It has no stale date: iOS rations Live Activity updates from apps in the background and drops the rest, so a
  renewed stale date didn't always arrive, and a running Phomic showed as stopped.) Tap the microphone to mute or unmute without
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
  Bluetooth drop iOS reconnects by itself (background mode `bluetooth-central`); over USB the PC reconnects.

### App Store builds (no Mac needed)

App Store Connect only takes apps built with Xcode, and Xcode only runs on a Mac, so GitHub's Macs do it:
[`.github/workflows/ios.yml`](../.github/workflows/ios.yml) (*Actions › iOS app › Run workflow*) turns the package
into an Xcode project ([`ios/Xcode/project.yml`](../ios/Xcode/project.yml), with
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
[`appstore/`](../appstore/metadata.md).

Code map: `MicController.swift` (state, lifecycle, pairing codes), `AudioCapture.swift` (Core Audio I/O,
real-time ring buffer, framing), `StreamClient.swift` (Wi-Fi, and USB once connected), `UsbListener.swift` (waits for
the PC over USB), `BluetoothLink.swift` (connects to the PC over Bluetooth),
`BluetoothScanner.swift` (finds PCs over Bluetooth), `BluetoothAudio.swift` (Opus encoder; ADPCM fallback),
`ServerBrowser.swift` (Bonjour), `WireProtocol.swift`, `ContentView.swift` (all screens, including Settings),
`LiveActivityController.swift` (the Live Activity), `UpdateCheck.swift` (newer version on the App Store?),
`Acknowledgements.swift` (the Opus license notice). `Sources/COpus` builds libopus.
