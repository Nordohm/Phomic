#pragma once

// Wire protocol shared with the iOS app (see docs/protocols.md).
//
// Control: TCP, newline-delimited UTF-8 JSON objects, one per line.
// Audio:   UDP datagrams, little-endian:
//            [0..3]   magic "PHMC"
//            [4..7]   session id (from the "welcome" message)
//            [8..11]  sequence number, +1 per packet, wraps
//            [12..]   kFrameSamples x int16 PCM, mono, kSampleRate Hz
//
// USB: the same messages over one TCP stream, which the PC opens to kUsbPort on the
// iPhone through Apple's USB service (usbmux). The iPhone listens on its loopback
// interface only, and only while USB is picked in the app. Audio packets travel between
// the control lines, unchanged: a packet starts with "PHMC" and a line with "{".

#include <cstddef>
#include <cstdint>

namespace phomic
{
    constexpr uint16_t kDefaultPort = 50505;
    // The iPhone's USB listener (on its loopback interface).
    constexpr uint16_t kUsbPort = 50505;
    // Apple's usbmux service: the Apple Devices app, or Apple Mobile Device Support.
    constexpr uint16_t kUsbmuxPort = 27015;
    constexpr int kProtocolVersion = 1;
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kFrameSamples = 480; // 10 ms per packet
    constexpr size_t kPacketHeaderBytes = 12;
    constexpr size_t kPacketBytes = kPacketHeaderBytes + kFrameSamples * sizeof(int16_t);
    constexpr uint8_t kPacketMagic[4] = { 'P', 'H', 'M', 'C' };

    // DNS-SD service type the iPhone browses for.
    constexpr wchar_t kServiceType[] = L"_phomic._tcp.local";

    // Client pings once per second; drop it if we hear nothing for this long.
    constexpr uint64_t kClientTimeoutMs = 6000;
    // A new TCP connection must send "hello" within this time.
    constexpr uint64_t kHelloTimeoutMs = 5000;
}
