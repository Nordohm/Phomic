#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "DeepFilter.h"
#include "rnnoise.h"

namespace phomic
{
    // Noise suppression: takes out background noise (fans, keyboards, traffic) and keeps the voice, the way
    // the iPhone's Voice Isolation does, but on the PC, so the iPhone can send its microphone unprocessed.
    // Two models, measured on an i5-8600K:
    //  - Light, RNNoise (third_party/rnnoise): 20 ms of delay, about 1.5 % of a CPU core. Fine on steady
    //    noise; lets much of a keyboard through.
    //  - Strong, DeepFilterNet3 (windows/DeepFilter, third_party/deepfilternet): 30 ms, about 5 %. Takes
    //    typing down too. Loading it takes some 300 ms: make one off the audio thread.
    //
    // Both work on blocks of exactly 10 ms (480 samples at 48 kHz); the audio device asks for blocks of its
    // own size. While those line up (the device asks for 10 ms at a time, or a multiple), each one goes
    // straight through. The first time one doesn't, 10 ms of silence goes out, once, and from then on
    // everything is that 10 ms later, which is always enough.
    //
    // Works on 48 kHz mono blocks of any size, in place. One per audio stream; not thread-safe.
    class Denoiser
    {
    public:
        enum class Strength
        {
            Light,
            Strong,
        };

        explicit Denoiser(Strength strength)
        {
            if (strength == Strength::Light)
            {
                m_rnnoise = rnnoise_create(nullptr);
            }
            else
            {
                m_deepFilter = phomic_df_create(kAttenuationLimitDb);
            }
        }
        ~Denoiser()
        {
            if (m_rnnoise)
            {
                rnnoise_destroy(m_rnnoise);
            }
            phomic_df_free(m_deepFilter);
        }
        Denoiser(Denoiser const&) = delete;
        Denoiser& operator=(Denoiser const&) = delete;

        bool IsReady() const
        {
            return m_rnnoise || (m_deepFilter && phomic_df_frame_length(m_deepFilter) == kFrame);
        }

        // Starts afresh (after it was off): lines up with the next block again. RNNoise forgets the noise
        // it had learnt; DeepFilterNet can't, it still holds the last moment it heard before it was turned
        // off, so its first frames go out as silence.
        void Reset()
        {
            if (m_rnnoise)
            {
                rnnoise_init(m_rnnoise, nullptr);
            }
            m_inCount = 0;
            m_outStart = 0;
            m_outCount = 0;
            m_silentFrames = m_deepFilter ? kWarmUpFrames : 0;
        }

        void Process(float* samples, uint32_t count)
        {
            for (uint32_t done = 0; done < count;)
            {
                const uint32_t chunk = std::min<uint32_t>(count - done, kFrame);
                float* part = samples + done;
                for (uint32_t i = 0; i < chunk; ++i)
                {
                    m_in[m_inCount++] = part[i];
                    if (m_inCount == kFrame)
                    {
                        Denoise();
                    }
                }
                for (uint32_t i = 0; i < chunk; ++i)
                {
                    if (m_outCount == 0)
                    {
                        // The model's block isn't complete yet: a block of delay, once.
                        for (int j = 0; j < kFrame; ++j)
                        {
                            Push(0.f);
                        }
                    }
                    part[i] = m_out[m_outStart];
                    m_outStart = (m_outStart + 1) % kOutSize;
                    --m_outCount;
                }
                done += chunk;
            }
        }

    private:
        static constexpr int kFrame = 480;
        static constexpr int kOutSize = 3 * kFrame; // the delay, a block, and room to spare
        // DeepFilterNet as far as it goes: fans and hiss gone, not just quieter.
        static constexpr float kAttenuationLimitDb = 100.f;
        // DeepFilterNet's own delay (window and look-ahead), in frames, and one more.
        static constexpr int kWarmUpFrames = 4;
        // The iPhone's microphone arrives unprocessed and often quiet (automatic gain, after this, turns it
        // up by as much as 30 dB), and both models do worse on quiet sound. DeepFilterNet doesn't hear
        // frames under -70 dBFS: from speech at about -55 dBFS it lost the quieter half of the voice.
        // RNNoise keeps the voice but takes less noise out the quieter it all is. +20 dB in and back out:
        // measured to keep the voice whole from -2 to -72 dBFS with both, and RNNoise then takes about
        // 20 dB more noise out.
        static constexpr float kModelGain = 10.f;
        // RNNoise wants samples in 16-bit range.
        static constexpr float kRnnoiseScale = 32768.f;

        void Denoise()
        {
            std::array<float, kFrame> denoised;
            const float scale = m_rnnoise ? kModelGain * kRnnoiseScale : kModelGain;
            for (float& sample : m_in)
            {
                sample *= scale;
            }
            if (m_rnnoise)
            {
                rnnoise_process_frame(m_rnnoise, denoised.data(), m_in.data());
            }
            else
            {
                phomic_df_process(m_deepFilter, m_in.data(), denoised.data());
            }
            const bool silent = m_silentFrames > 0;
            if (silent)
            {
                --m_silentFrames;
            }
            for (float sample : denoised)
            {
                Push(silent ? 0.f : sample / scale);
            }
            m_inCount = 0;
        }

        void Push(float sample)
        {
            m_out[(m_outStart + m_outCount) % kOutSize] = sample;
            ++m_outCount;
        }

        DenoiseState* m_rnnoise = nullptr;
        PhomicDeepFilter* m_deepFilter = nullptr;
        std::array<float, kFrame> m_in{};
        int m_inCount = 0;
        std::array<float, kOutSize> m_out{};
        int m_outStart = 0;
        int m_outCount = 0;
        int m_silentFrames = 0;
    };
}
