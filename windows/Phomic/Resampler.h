#pragma once

namespace phomic
{
    // Integer-factor upsampler (polyphase windowed-sinc FIR). Used to bring Bluetooth
    // audio (24 kHz or 16 kHz) back up to the 48 kHz the rest of the pipeline uses.
    class Upsampler
    {
    public:
        // Resets the filter state when the factor changes.
        void SetFactor(int factor);
        int Factor() const { return m_factor; }

        // Reads `count` samples, writes `count * Factor()` samples.
        void Process(const float* in, size_t count, float* out);

    private:
        static constexpr int kTapsPerPhase = 32;

        int m_factor = 0;
        std::vector<float> m_taps;     // kTapsPerPhase * factor, polyphase order: taps[p + k*factor]
        std::vector<float> m_history;  // m_history[k] = input k samples ago
    };
}
