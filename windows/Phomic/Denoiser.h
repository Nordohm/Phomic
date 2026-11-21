#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "rnnoise.h"

namespace phomic
{
    // Noise suppression with RNNoise (third_party/rnnoise): takes out background noise (fans,
    // keyboards, traffic) and keeps the voice, the way the iPhone's Voice Isolation does, but on
    // the PC, so the iPhone can send its microphone unprocessed. RNNoise itself delays the sound
    // by 20 ms.
    //
    // RNNoise works on blocks of exactly 10 ms (480 samples at 48 kHz) and wants samples in
    // 16-bit range; the audio device asks for blocks of its own size. While those line up with
    // RNNoise's (the device asks for 10 ms at a time, or a multiple), each one goes straight
    // through. The first time one doesn't, 10 ms of silence goes out, once, and from then on
    // everything is that 10 ms later, which is always enough.
    //
    // Works on 48 kHz mono blocks of any size, in place. One per audio stream; not thread-safe.
    class Denoiser
    {
    public:
        Denoiser() : m_state(rnnoise_create(nullptr)) {}
        ~Denoiser() { rnnoise_destroy(m_state); }
        Denoiser(Denoiser const&) = delete;
        Denoiser& operator=(Denoiser const&) = delete;

        // Starts afresh (after it was off): forgets the noise it had learnt, and lines up with
        // the next block again.
        void Reset()
        {
            rnnoise_init(m_state, nullptr);
            m_inCount = 0;
            m_outStart = 0;
            m_outCount = 0;
        }

        void Process(float* samples, uint32_t count)
        {
            for (uint32_t done = 0; done < count;)
            {
                const uint32_t chunk = std::min<uint32_t>(count - done, kFrame);
                float* part = samples + done;
                for (uint32_t i = 0; i < chunk; ++i)
                {
                    m_in[m_inCount++] = part[i] * kScale;
                    if (m_inCount == kFrame)
                    {
                        Denoise();
                    }
                }
                for (uint32_t i = 0; i < chunk; ++i)
                {
                    if (m_outCount == 0)
                    {
                        // RNNoise's block isn't complete yet: a block of delay, once.
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
        static constexpr float kScale = 32768.f;

        void Denoise()
        {
            std::array<float, kFrame> denoised;
            rnnoise_process_frame(m_state, denoised.data(), m_in.data());
            for (float sample : denoised)
            {
                Push(sample / kScale);
            }
            m_inCount = 0;
        }

        void Push(float sample)
        {
            m_out[(m_outStart + m_outCount) % kOutSize] = sample;
            ++m_outCount;
        }

        DenoiseState* m_state;
        std::array<float, kFrame> m_in{};
        int m_inCount = 0;
        std::array<float, kOutSize> m_out{};
        int m_outStart = 0;
        int m_outCount = 0;
    };
}
