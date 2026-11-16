#pragma once

namespace phomic
{
    enum class Source : int
    {
        None = 0,
        WiFi = 1,
        Bluetooth = 2,
    };

    // Decides which transport may feed the jitter buffer. Only one connection streams at
    // a time, and Wi-Fi and Bluetooth are equals: the newest connection takes over and the
    // other one hangs up.
    class SourceArbiter
    {
    public:
        Source Owner() const { return static_cast<Source>(m_owner.load()); }
        bool Owns(Source source) const { return m_owner.load() == static_cast<int>(source); }

        bool TryClaim(Source source)
        {
            int expected = 0;
            return m_owner.compare_exchange_strong(expected, static_cast<int>(source)) || expected == static_cast<int>(source);
        }

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
