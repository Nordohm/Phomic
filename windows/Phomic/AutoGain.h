#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace phomic
{
    // Automatic gain: keeps speech at a steady level, the way voice chat apps do. The iPhone
    // sends its microphone raw (measurement mode), so how loud it arrives depends on the voice,
    // the distance and the Mic Mode; this turns it up or down to match.
    //
    // It listens for speech, meaning sound clearly above the background noise (whose level it
    // tracks), and moves the gain so speech averages kTargetDb: down quickly when it's too
    // loud, up slowly. In pauses it holds still, so the background doesn't swell up when nobody
    // talks. The Limiter after it catches the peaks it's too slow for.
    //
    // Works on 48 kHz mono blocks of any size. One per audio stream; not thread-safe.
    class AutoGain
    {
    public:
        static constexpr float kMinDb = 0.f, kMaxDb = 30.f;

        explicit AutoGain(float startDb) : m_gainDb(std::clamp(startDb, kMinDb, kMaxDb)), m_appliedDb(m_gainDb) {}

        float GainDb() const { return m_gainDb; }

        // Measures the block, updates the gain and applies it in place (ramped across the block,
        // so a change doesn't click).
        void Process(float* samples, uint32_t count)
        {
            if (count == 0)
            {
                return;
            }
            double sum = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                sum += static_cast<double>(samples[i]) * samples[i];
            }
            const double meanSquare = sum / count;
            const double seconds = count / kSampleRate;

            // Digital silence (muted, or nothing arriving) says nothing about the room: skip it.
            if (meanSquare > 1e-11)
            {
                Measure(meanSquare, seconds);
            }

            const float from = DbToGain(m_appliedDb);
            const float to = DbToGain(m_gainDb);
            for (uint32_t i = 0; i < count; ++i)
            {
                samples[i] *= from + (to - from) * (static_cast<float>(i + 1) / count);
            }
            m_appliedDb = m_gainDb;
        }

    private:
        static constexpr double kSampleRate = 48000;
        static constexpr double kTargetDb = -23;      // speech, RMS over about 100 ms
        static constexpr double kSpeechAboveNoiseDb = 10;
        static constexpr double kLevelSeconds = 0.1;  // how the level is averaged
        static constexpr double kNoiseDownSeconds = 0.05, kNoiseUpSeconds = 15;
        static constexpr double kGainDownSeconds = 0.4, kGainUpSeconds = 3;

        static float DbToGain(float db) { return std::pow(10.f, db / 20.f); }

        static double Approach(double value, double target, double seconds, double timeConstant)
        {
            return value + (target - value) * (1 - std::exp(-seconds / timeConstant));
        }

        void Measure(double meanSquare, double seconds)
        {
            m_power = m_started ? Approach(m_power, meanSquare, seconds, kLevelSeconds) : meanSquare;
            const double levelDb = 10 * std::log10(m_power);
            // Background noise, from this block alone: the short gaps between words pull it down
            // at once; it only rises slowly, so talking doesn't count as noise.
            const double blockDb = 10 * std::log10(meanSquare);
            m_noiseDb = !m_started ? blockDb
                      : Approach(m_noiseDb, blockDb, seconds, blockDb < m_noiseDb ? kNoiseDownSeconds : kNoiseUpSeconds);
            m_started = true;

            if (levelDb > m_noiseDb + kSpeechAboveNoiseDb)
            {
                const double wanted = std::clamp(kTargetDb - levelDb, static_cast<double>(kMinDb), static_cast<double>(kMaxDb));
                m_gainDb = static_cast<float>(Approach(m_gainDb, wanted, seconds,
                                                       wanted < m_gainDb ? kGainDownSeconds : kGainUpSeconds));
            }
        }

        float m_gainDb;
        float m_appliedDb;
        double m_power = 0;
        double m_noiseDb = -100;
        bool m_started = false;
    };
}
