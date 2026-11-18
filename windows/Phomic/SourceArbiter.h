#pragma once

namespace phomic
{
    enum class Source : int
    {
        None = 0,
        WiFi = 1,
        Bluetooth = 2,
        Usb = 3,
    };

    // The name a "switch" request uses for a transport.
    inline char const* TransportName(Source source)
    {
        switch (source)
        {
        case Source::WiFi: return "wifi";
        case Source::Bluetooth: return "bluetooth";
        case Source::Usb: return "usb";
        default: return "";
        }
    }

    // What a session that lost the jitter buffer to `owner` shows as its reason.
    inline wchar_t const* SwitchedToText(Source owner)
    {
        switch (owner)
        {
        case Source::WiFi: return L"Switched to Wi-Fi.";
        case Source::Bluetooth: return L"Switched to Bluetooth.";
        case Source::Usb: return L"Switched to USB.";
        default: return L"Another connection took over.";
        }
    }

    // Decides which transport may feed the jitter buffer. Only one connection streams at
    // a time, and Wi-Fi, Bluetooth and USB are equals: the newest connection takes over and
    // the other one hangs up.
    class SourceArbiter
    {
    public:
        Source Owner() const { return static_cast<Source>(m_owner.load()); }
        bool Owns(Source source) const { return m_owner.load() == static_cast<int>(source); }

        void Force(Source source) { m_owner.store(static_cast<int>(source)); }

        void Release(Source source)
        {
            int expected = static_cast<int>(source);
            m_owner.compare_exchange_strong(expected, 0);
        }

    private:
        std::atomic<int> m_owner{ 0 };
    };
}
