# Third-party components

## libopus (`opus/`)

- **What:** the Opus audio codec, libopus 1.6.1. Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
  Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo,
  Mozilla and Amazon.
- **Origin:** https://opus-codec.org, downloaded from
  `https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz`.
- **License:** 3-clause BSD; see `opus/COPYING`. Opus is also covered by royalty-free patent licenses
  (listed in `COPYING`).
- **How Phomic uses it:** only the core library sources (`celt/`, `silk/`, `silk/float/`, `src/`, `include/`) are
  kept, unmodified. `opus.vcxproj` compiles them into a static library that is linked into `Phomic.exe`, so users
  install nothing extra. The PC app decodes Bluetooth audio with it. The iOS app builds the same sources as a
  SwiftPM target (`Sources/COpus`) and encodes with it. The license notice ships with both: the build copies
  `COPYING` to `licenses/opus/` next to `Phomic.exe`, and the iOS app shows it under *Settings › Acknowledgements*.

## VB-CABLE (not included)

- **What:** VB-CABLE Virtual Audio Device by Vincent Burel / VB-Audio (www.vb-cable.com), donationware.
- **Not shipped with Phomic:** the repo, the installer and the zip don't contain it. When VB-CABLE isn't installed,
  Phomic's banner links to VB-Audio's own page (`https://vb-audio.com/Cable/`), and people download and install it
  from there, under VB-Audio's terms. (The `readme.txt` in VB-Audio's package says it must not be integrated into
  another software's installation without the author's agreement, so Phomic doesn't bundle, download or run it.)
- **How Phomic uses it once installed:** Phomic finds the endpoints by their driver name and uses Windows' per-device
  settings (the same ones as *Settings › Sound › Rename / Don't allow*) to name them "Phomic" / "Phomic (internal)" and
  disable the unused "CABLE In 16ch" input. It plays into the driver directly (kernel streaming), so other apps only
  see "Phomic". `Phomic.exe --restore-microphone` (also run when Phomic is uninstalled) undoes the renaming.
- **Credit:** not required, since Phomic doesn't distribute VB-CABLE (the readme asks for it "in case of
  distribution"), but given anyway: *Show details › This PC › Mic driver* says the microphone runs on VB-CABLE by
  VB-Audio (www.vb-cable.com), donationware, and so does the main README.
