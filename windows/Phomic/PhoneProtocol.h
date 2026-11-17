#pragma once

#include "Protocol.h"
#include "SourceArbiter.h"

// The phone's messages over Wi-Fi and USB (see Protocol.h), read and written in one place.
// StreamServer (Wi-Fi) and UsbReceiver (USB) only differ in how the bytes travel.
namespace phomic::protocol
{
    // The phone's first line.
    struct Hello
    {
        bool valid = false;        // false: not a hello at all, just hang up
        std::wstring refusal;      // set: tell the phone (Error) and hang up
        std::wstring name;         // the phone's name, "iPhone" if it didn't say
        std::wstring code;         // the pairing code it sent, if any
    };
    Hello ParseHello(std::string const& line);

    // Every later line from the phone.
    struct Message
    {
        enum class Type { Other, Ping, Mute, Bye };
        Type type = Type::Other;
        int64_t pingId = 0;
        double rttMs = -1;         // Ping: the phone's last measured round trip
        bool muted = false;        // Mute
    };
    Message ParseMessage(std::string const& line);

    // `udpPort` 0: audio comes on the same connection (USB).
    std::string Welcome(std::wstring const& pcName, uint32_t session, uint16_t udpPort);
    std::string Pong(int64_t id);
    std::string Error(std::wstring_view message);
    std::string Switch(Source to);
    inline constexpr char kBye[] = R"({"type":"bye"})";
    // "bye" because another connection took over: the phone shouldn't come straight back.
    inline constexpr char kByeSwitched[] = R"({"type":"bye","reason":"switched"})";

    // A random session id; never 0, which means "no session".
    uint32_t NewSession(std::mt19937& rng);

    // One kPacketBytes audio packet.
    struct Audio
    {
        uint32_t session = 0;
        uint32_t sequence = 0;
        int16_t samples[kFrameSamples]{};
        float peak = 0;            // highest absolute sample, 0..1
    };
    // False if it isn't one (wrong size or magic).
    bool ParseAudio(const uint8_t* data, size_t size, Audio& audio);

    // Raises a level meter's peak (written on a network thread, taken by the UI).
    inline void RaisePeak(std::atomic<float>& peak, float level)
    {
        if (level > peak.load(std::memory_order_relaxed))
        {
            peak.store(level, std::memory_order_relaxed);
        }
    }
}
