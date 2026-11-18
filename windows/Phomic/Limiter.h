#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace phomic
{
    // Applies the gain and keeps the result under full scale: when a peak would go over the
    // ceiling, the level goes down at once, and back up over about a tenth of a second. So the
    // gain can be generous (quiet voices come through at a usable level) without the harsh
    // distortion of clipping when someone laughs or shouts.
    //
    // One per audio stream (it remembers how far down it is); not thread-safe.
    class Limiter
    {
    public:
        static constexpr float kCeiling = 0.89f; // -1 dBFS

        void Process(float* samples, uint32_t count, float gain)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const float x = samples[i] * gain;
                const float magnitude = std::fabs(x);
                const float allowed = magnitude > kCeiling ? kCeiling / magnitude : 1.f;
                m_reduction = std::min(allowed, m_reduction + (1.f - m_reduction) * kRelease);
                samples[i] = x * m_reduction;
            }
        }

    private:
        // Per sample at 48 kHz: back up with a time constant of about 100 ms.
        static constexpr float kRelease = 2.1e-4f;

        float m_reduction = 1.f;
    };
}
