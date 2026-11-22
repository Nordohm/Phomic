#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "DeepFilter.h"

namespace phomic
{
    // Noise suppression with DeepFilterNet3 (windows/DeepFilter, third_party/deepfilternet): takes out
    // background noise (fans, keyboards, traffic) and keeps the voice, the way the iPhone's Voice
    // Isolation does, but on the PC, so the iPhone can send its microphone unprocessed.
    //
    // DeepFilterNet works on blocks of exactly 10 ms (480 samples at 48 kHz); the audio device asks for
    // blocks of its own size. While those line up (the device asks for 10 ms at a time, or a multiple),
    // each one goes straight through. The first time one doesn't, 10 ms of silence goes out, once, and
    // from then on everything is that 10 ms later, which is always enough.
    //
    // Loading the model takes a moment: make one off the audio thread, and use it once IsReady().
    // Works on 48 kHz mono blocks of any size, in place. One per audio stream; not thread-safe.
    class Denoiser
    {
    public:
        Denoiser() : m_state(phomic_df_create(kAttenuationLimitDb)) {}
        ~Denoiser() { phomic_df_free(m_state); }
        Denoiser(Denoiser const&) = delete;
        Denoiser& operator=(Denoiser const&) = delete;

        bool IsReady() const { return m_state && phomic_df_frame_length(m_state) == kFrame; }

        // Starts afresh (after it was off): lines up with the next block again. The model still holds
        // the last moment it heard before it was turned off, so its first frames go out as silence.
        void Reset()
        {
            m_inCount = 0;
            m_outStart = 0;
            m_outCount = 0;
            m_silentFrames = kWarmUpFrames;
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
        // As far as it goes: fans and hiss gone, not just quieter.
        static constexpr float kAttenuationLimitDb = 100.f;
        // The model's own delay (window and look-ahead), in frames, and one more.
        static constexpr int kWarmUpFrames = 4;
        // The iPhone's microphone arrives unprocessed and often quiet (automatic gain, after this, turns it
        // up by as much as 30 dB), and the model doesn't hear frames under -70 dBFS: from speech at about
        // -55 dBFS it lost the quieter half of the voice. +20 dB in and back out: measured to keep the
        // voice whole from -2 to -72 dBFS, with the same noise taken out.
        static constexpr float kModelGain = 10.f;

        void Denoise()
        {
            for (float& sample : m_in)
            {
                sample *= kModelGain;
            }
            std::array<float, kFrame> denoised;
            phomic_df_process(m_state, m_in.data(), denoised.data());
            const bool silent = m_silentFrames > 0;
            if (silent)
            {
                --m_silentFrames;
            }
            for (float sample : denoised)
            {
                Push(silent ? 0.f : sample / kModelGain);
            }
            m_inCount = 0;
        }

        void Push(float sample)
        {
            m_out[(m_outStart + m_outCount) % kOutSize] = sample;
            ++m_outCount;
        }

        PhomicDeepFilter* m_state;
        std::array<float, kFrame> m_in{};
        int m_inCount = 0;
        std::array<float, kOutSize> m_out{};
        int m_outStart = 0;
        int m_outCount = 0;
        int m_silentFrames = kWarmUpFrames;
    };
}
