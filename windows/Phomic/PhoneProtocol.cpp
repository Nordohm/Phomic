#include "pch.h"
#include "PhoneProtocol.h"
#include "JsonUtil.h"
#include "Pairing.h"

using namespace winrt::Windows::Data::Json;

namespace phomic::protocol
{
    namespace
    {
        JsonObject Parse(std::string const& line)
        {
            JsonObject object{ nullptr };
            try
            {
                if (!JsonObject::TryParse(winrt::to_hstring(line), object))
                {
                    return nullptr;
                }
            }
            catch (winrt::hresult_error const&)
            {
                return nullptr;
            }
            return object;
        }
    }

    Hello ParseHello(std::string const& line)
    {
        Hello hello;
        try
        {
            const JsonObject msg = Parse(line);
            if (!msg || msg.GetNamedString(L"type", L"") != L"hello")
            {
                return hello;
            }
            hello.valid = true;
            hello.name = msg.GetNamedString(L"name", L"");
            hello.code = msg.GetNamedString(L"code", L"");
            if (hello.name.empty())
            {
                hello.name = L"iPhone";
            }
            if (static_cast<int>(msg.GetNamedNumber(L"version", 0)) != kProtocolVersion)
            {
                hello.refusal = L"The iPhone app and the PC app are different versions. Please update both.";
            }
            else if (static_cast<uint32_t>(msg.GetNamedNumber(L"sampleRate", 0)) != kSampleRate ||
                     static_cast<int>(msg.GetNamedNumber(L"channels", 0)) != 1 ||
                     static_cast<uint32_t>(msg.GetNamedNumber(L"frameSamples", 0)) != kFrameSamples)
            {
                hello.refusal = L"Unsupported audio format.";
            }
        }
        catch (winrt::hresult_error const&)
        {
            hello = Hello{}; // a field of the wrong type: not a usable hello
        }
        return hello;
    }

    Message ParseMessage(std::string const& line)
    {
        Message message;
        try
        {
            const JsonObject msg = Parse(line);
            if (!msg)
            {
                return message;
            }
            const auto type = msg.GetNamedString(L"type", L"");
            if (type == L"ping")
            {
                message.type = Message::Type::Ping;
                message.pingId = static_cast<int64_t>(msg.GetNamedNumber(L"id", 0));
                message.rttMs = msg.GetNamedNumber(L"rtt", -1);
            }
            else if (type == L"mute")
            {
                message.type = Message::Type::Mute;
                message.muted = msg.GetNamedBoolean(L"muted", false);
            }
            else if (type == L"bye")
            {
                message.type = Message::Type::Bye;
            }
        }
        catch (winrt::hresult_error const&)
        {
            message = Message{}; // ignore malformed messages
        }
        return message;
    }

    std::string Welcome(std::wstring const& pcName, uint32_t session, uint16_t udpPort)
    {
        const std::string audio = udpPort != 0 ? std::format(R"("udpPort":{})", udpPort) : std::string(R"("transport":"usb")");
        return std::format(R"({{"type":"welcome","version":{},"name":"{}","pcId":"{}",{},"session":{}}})",
            kProtocolVersion, JsonEscape(std::wstring_view(pcName)), JsonEscape(std::wstring_view(pairing::PcId())), audio, session);
    }

    std::string Pong(int64_t id)
    {
        return std::format(R"({{"type":"pong","id":{}}})", id);
    }

    std::string Error(std::wstring_view message)
    {
        return std::format(R"({{"type":"error","message":"{}"}})", JsonEscape(message));
    }

    std::string Switch(Source to)
    {
        return std::format(R"({{"type":"switch","transport":"{}"}})", TransportName(to));
    }

    uint32_t NewSession(std::mt19937& rng)
    {
        uint32_t session = 0;
        do
        {
            session = static_cast<uint32_t>(rng());
        } while (session == 0);
        return session;
    }

    bool ParseAudio(const uint8_t* data, size_t size, Audio& audio)
    {
        if (size != kPacketBytes || std::memcmp(data, kPacketMagic, sizeof(kPacketMagic)) != 0)
        {
            return false;
        }
        std::memcpy(&audio.session, data + 4, sizeof(audio.session));
        std::memcpy(&audio.sequence, data + 8, sizeof(audio.sequence));
        std::memcpy(audio.samples, data + kPacketHeaderBytes, sizeof(audio.samples));
        int peak = 0;
        for (int16_t sample : audio.samples)
        {
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        audio.peak = peak / 32768.f;
        return true;
    }
}
