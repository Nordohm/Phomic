#include "pch.h"
#include "Resampler.h"

namespace phomic
{
    void Upsampler::SetFactor(int factor)
    {
        if (factor == m_factor)
        {
            return;
        }
        m_factor = factor;
        m_history.assign(kTapsPerPhase, 0.f);
        if (factor <= 1)
        {
            m_taps.clear();
            return;
        }

        // Low-pass just below the input signal's Nyquist frequency, relative to the
        // output rate; same corners as the decimator on the iPhone.
        const double cutoff = (factor == 2) ? 0.2 : 0.135;
        const int length = kTapsPerPhase * factor;
        // Odd-length symmetric filter (last tap stays zero) so the delay is a whole
        // number of samples.
        const int designed = length - 1;
        const double center = (designed - 1) / 2.0;
        constexpr double pi = 3.14159265358979323846;

        std::vector<double> taps(length, 0.0);
        double sum = 0;
        for (int i = 0; i < designed; ++i)
        {
            const double x = i - center;
            const double sinc = (x == 0) ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
            const double window = 0.42 - 0.5 * std::cos(2 * pi * i / (designed - 1)) + 0.08 * std::cos(4 * pi * i / (designed - 1));
            taps[i] = sinc * window;
            sum += taps[i];
        }

        // Zero-stuffing divides the signal level by `factor`; the gain puts it back.
        m_taps.resize(length);
        for (int i = 0; i < length; ++i)
        {
            m_taps[i] = static_cast<float>(taps[i] * factor / sum);
        }
    }

    void Upsampler::Process(const float* in, size_t count, float* out)
    {
        if (m_factor <= 1)
        {
            std::copy(in, in + count, out);
            return;
        }
        for (size_t i = 0; i < count; ++i)
        {
            std::memmove(&m_history[1], &m_history[0], (kTapsPerPhase - 1) * sizeof(float));
            m_history[0] = in[i];
            for (int phase = 0; phase < m_factor; ++phase)
            {
                float acc = 0.f;
                for (int k = 0; k < kTapsPerPhase; ++k)
                {
                    acc += m_taps[phase + k * m_factor] * m_history[k];
                }
                out[i * m_factor + phase] = acc;
            }
        }
    }
}
