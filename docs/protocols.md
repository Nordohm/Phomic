# Protocols

How the iPhone app and the PC app talk to each other. For building the apps, see
[development.md](development.md).

```
                       ┌─ Wi-Fi: 48 kHz PCM, 10 ms UDP packets (+ TCP control) ─┐
iPhone mic ─► RemoteIO ├─ USB cable: the same packets and control, one TCP stream ┤─► jitter buffer ─► VB-CABLE ─► "Phomic" mic
  (Mic Mode)           └─ Bluetooth LE: 48 kHz Opus, iPhone → PC GATT writes ─────┘     (48 kHz)
```

## Wi-Fi protocol (version 1)

**Discovery:** the PC registers DNS-SD service `<computer name>._phomic._tcp.local` on port 50505, with TXT
`v=1`, `id=<PC id>` (8 hex digits, the same id as over Bluetooth) and `pair=1|0` (whether a code is required).

**Control:** TCP to port 50505, one UTF-8 JSON object per line (`\n`). `→` is from the phone, `←` from the PC:

```
→ {"type":"hello","version":1,"name":"iPhone","sampleRate":48000,"channels":1,"frameSamples":480,"code":"1234"}
← {"type":"welcome","version":1,"name":"MY-PC","pcId":"3A7C91E2","udpPort":50505,"session":123456789}
→ {"type":"ping","id":7,"rtt":9.8}
← {"type":"pong","id":7}
→ {"type":"mute","muted":true}
← {"type":"switch","transport":"bluetooth"}
→ {"type":"bye"}
```

- `hello` must be the first line, within 5 s. It has `code` only if the phone knows one.
- Instead of `welcome`, the PC can refuse and close the connection. For a missing or wrong code it sends the first
  error below, and the phone asks the user for the code. For anything else it sends the second, and the phone
  doesn't retry by itself.
  ```
  ← {"type":"error","reason":"pairing","pcId":"3A7C91E2","name":"MY-PC","message":"Wrong pairing code."}
  ← {"type":"error","message":"…"}
  ```
- The phone pings every second; `rtt` is the last measured round trip in ms. The PC drops a phone it hasn't heard
  from in 6 s, and the phone gives up after 5 s without a reply.
- While muted, the phone sends no audio.
- `switch` is the PC's *Switch* button: the phone hangs up and moves to that transport (`wifi`, `bluetooth` or
  `usb`).
- Either side can send `bye` to hang up.

**Audio:** UDP from the phone to `udpPort` on the address the TCP connection resolved to. Each datagram
is 972 bytes, little-endian: `"PHMC"` magic (4) · `session` (4) · sequence number (4, wraps) · 480 × int16 PCM
(mono, 48 kHz, 10 ms).

## USB protocol

Windows can't reach an iPhone over USB by itself. Apple's USB service can: *usbmux*, part of the Apple Devices app
(or Apple Mobile Device Support), listening on `127.0.0.1:27015`. It reports iPhones as they're plugged in, and
opens TCP connections to ports on them through the cable. Messages to it are XML plists, each after a 16-byte
little-endian header (length, version 1, type 8, tag).

1. The PC sends `Listen` and gets `Attached` (with a `DeviceID`) for every Apple device plugged in, and `Detached`
   when one goes. Devices that usbmux reaches over the network instead are ignored.
2. For each one, the PC sends `Connect` with `DeviceID` and `PortNumber` 50505 (in network byte order) on a new
   connection to usbmux, every 300 ms (a refused try is closed without `TIME_WAIT`). `Number` 0 in the reply
   means the connection is open; 3 means nothing listens there, because Phomic on that iPhone isn't on USB right
   now.
3. Phomic on the iPhone listens on port 50505 of its **loopback** interface while USB is picked, so only usbmux on
   the iPhone itself can reach it, and Local Network permission isn't involved.
4. From then on the connection carries the [Wi-Fi protocol](#wi-fi-protocol-version-1), with two changes: `welcome`
   has `"transport":"usb"` and no `udpPort`, and the audio packets (the same 972 bytes as a UDP datagram) travel on
   this connection, between the control lines. A packet starts with `PHMC` and a line with `{`. There's no pairing
   code.

If the cable comes out, both ends notice at once. The iPhone keeps listening, and the PC connects again when the
iPhone is plugged back in. After a `bye` from either side the iPhone stops listening.

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
| `8E5A1C6<c>-3B7D-4F62-9A1E-6C2D<PC id>` | service | advertised |
| `8E5A1C51-3B7D-4F62-9A1E-6C2D4B8F0A11` | audio | write without response |
| `8E5A1C52-3B7D-4F62-9A1E-6C2D4B8F0A11` | control (UTF-8 JSON) | write, notify |

In the service UUID, `<c>` is `1` if a pairing code is required, else `0`. PC `3A7C91E2` without a code advertises
`8E5A1C60-3B7D-4F62-9A1E-6C2D3A7C91E2`.

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
6. The PC's *Switch* notifies `{"type":"switch","transport":"wifi"}`. The phone stops Bluetooth and connects
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
