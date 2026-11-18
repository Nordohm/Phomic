#include "pch.h"
#include "JitterBuffer.h"

namespace phomic
{
    namespace
    {
        constexpr uint32_t kFadeSamples = 64;
        // How hard the drift controller pulls toward the target depth:
        // 10 ms of excess buffer -> ~0.2% faster playback -> drained in ~5 s.
        constexpr double kDriftGain = 4e-6;
        constexpr double kMaxDrift = 0.005;
        constexpr double kDepthSmoothing = 0.02; // per audio callback (~10 ms)

        // Auto buffering
        constexpr uint32_t kAutoMaxFrames = 20;        // never more than 200 ms
        constexpr double kAutoPercentile = 0.995;      // cover 99.5% of the arrival jitter...
        constexpr double kAutoMarginMs = 10.0;         // ...plus one frame of margin
        constexpr double kShrinkAfterMs = 10'000;      // shrink only after 10 s of calm
        constexpr double kBoostDecayMs = 30'000;       // an underrun adds a frame for 30 s
        constexpr uint32_t kUpdateEvery = 50;          // re-evaluate twice a second
    }

    double JitterBuffer::NowMs() const
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    JitterBuffer::JitterBuffer() : m_slots(kSlots)
    {
    }

    void JitterBuffer::Reset()
    {
        std::lock_guard guard(m_lock);
        ResetLocked();
        m_stats = {};
    }

    void JitterBuffer::ResetLocked()
    {
        for (auto& slot : m_slots)
        {
            slot.valid = false;
        }
        m_hasStream = false;
        m_buffering = true;
        m_playSeq = 0;
        m_highSeq = 0;
        m_framePos = kFrameSamples;
        m_concealRun = 0;
        m_lastGood.fill(0.f);
        m_frac = 1.0;
        m_prev = 0.f;
        m_cur = 0.f;
        m_avgDepth = 0;
        m_offsetCount = 0;
        m_offsetNext = 0;
        m_haveTiming = false;
        m_measuredSinceUpdate = 0;
    }

    void JitterBuffer::SetTargetFrames(uint32_t frames)
    {
        std::lock_guard guard(m_lock);
        m_automatic = false;
        ApplyTargetLocked(frames);
    }

    void JitterBuffer::SetAutomatic(uint32_t minFrames)
    {
        std::lock_guard guard(m_lock);
        const bool wasAutomatic = m_automatic;
        m_automatic = true;
        m_minFrames = std::clamp<uint32_t>(minFrames, 2, kAutoMaxFrames);
        if (!wasAutomatic)
        {
            ApplyTargetLocked(std::max<uint32_t>(m_minFrames, 4)); // start modest, adapt from there
            m_lowSince = -1;
        }
        else if (m_targetFrames < m_minFrames)
        {
            ApplyTargetLocked(m_minFrames);
        }
    }

    void JitterBuffer::ApplyTargetLocked(uint32_t frames)
    {
        m_targetFrames = std::clamp<uint32_t>(frames, 2, 40);
        m_maxFrames = m_targetFrames * 2 + 4;
    }

    void JitterBuffer::MeasureArrivalLocked(uint32_t seq)
    {
        // How late did this packet arrive compared with its 10 ms slot? The earliest
        // arrival in the window is "on time"; everything else is jitter we must absorb.
        const double now = NowMs();
        if (!m_haveTiming)
        {
            m_haveTiming = true;
            m_timingBaseSeq = seq;
        }
        const double offset = now - static_cast<double>(SeqDiff(seq, m_timingBaseSeq)) * 10.0;
        m_offsets[m_offsetNext] = offset;
        m_offsetNext = (m_offsetNext + 1) % kTimingWindow;
        m_offsetCount = std::min(m_offsetCount + 1, kTimingWindow);
        if (m_automatic && ++m_measuredSinceUpdate >= kUpdateEvery)
        {
            m_measuredSinceUpdate = 0;
            UpdateAutoTargetLocked();
        }
    }

    void JitterBuffer::UpdateAutoTargetLocked()
    {
        if (m_offsetCount < 100)
        {
            return; // not enough data yet (1 s)
        }
        std::vector<double>& delays = m_delays;
        delays.assign(m_offsets.begin(), m_offsets.begin() + static_cast<std::ptrdiff_t>(m_offsetCount));
        const double earliest = *std::min_element(delays.begin(), delays.end());
        for (double& d : delays)
        {
            d -= earliest;
        }
        const size_t rank = static_cast<size_t>(kAutoPercentile * (delays.size() - 1));
        std::nth_element(delays.begin(), delays.begin() + static_cast<std::ptrdiff_t>(rank), delays.end());
        const double jitterMs = delays[rank];

        const double now = NowMs();
        if (m_boostFrames > 0 && now - m_boostAt > kBoostDecayMs)
        {
            --m_boostFrames;
            m_boostAt = now;
        }
        uint32_t wanted = static_cast<uint32_t>(std::ceil((jitterMs + kAutoMarginMs) / 10.0)) + m_boostFrames;
        wanted = std::clamp(wanted, m_minFrames, kAutoMaxFrames);

        if (wanted > m_targetFrames)
        {
            ApplyTargetLocked(wanted); // grow at once
            m_lowSince = -1;
        }
        else if (wanted < m_targetFrames)
        {
            if (m_lowSince < 0)
            {
                m_lowSince = now;
            }
            else if (now - m_lowSince > kShrinkAfterMs)
            {
                ApplyTargetLocked(m_targetFrames - 1); // shrink gently, one frame at a time
                m_lowSince = now;
            }
        }
        else
        {
            m_lowSince = -1;
        }
    }

    void JitterBuffer::Push(uint32_t seq, const int16_t* samples, bool measureTiming)
    {
        std::lock_guard guard(m_lock);
        if (measureTiming)
        {
            MeasureArrivalLocked(seq);
        }

        if (!m_hasStream)
        {
            m_hasStream = true;
            m_buffering = true;
            m_playSeq = seq;
            m_highSeq = seq;
        }

        int32_t ahead = SeqDiff(seq, m_playSeq);
        if (ahead < 0)
        {
            ++m_stats.late;
            return;
        }
        if (ahead >= static_cast<int32_t>(kSlots))
        {
            // The sender jumped far ahead (it restarted, or we stalled). Start over here.
            ResetLocked();
            m_hasStream = true;
            m_playSeq = seq;
            m_highSeq = seq;
        }

        Slot& slot = m_slots[seq % kSlots];
        if (slot.valid && slot.seq == seq)
        {
            return; // duplicate
        }
        slot.seq = seq;
        slot.valid = true;
        std::memcpy(slot.data.data(), samples, kFrameSamples * sizeof(int16_t));

        if (SeqDiff(seq, m_highSeq) > 0)
        {
            m_highSeq = seq;
        }
        ++m_stats.received;
    }

    int32_t JitterBuffer::DepthFramesLocked() const
    {
        if (!m_hasStream)
        {
            return 0;
        }
        return SeqDiff(m_highSeq, m_playSeq) + 1;
    }

    bool JitterBuffer::PopFrameLocked()
    {
        if (!m_hasStream)
        {
            return false;
        }

        const int32_t depth = DepthFramesLocked();
        if (m_buffering)
        {
            if (depth < static_cast<int32_t>(m_targetFrames))
            {
                return false;
            }
            m_buffering = false;
            m_concealRun = 1; // fade in from silence
            if (depth > static_cast<int32_t>(m_targetFrames))
            {
                // Recovering from a stall: the backlog arrived in one burst. The audio is
                // already discontinuous, so drop the stale part rather than carry the latency.
                SkipToTargetLocked();
            }
        }
        else if (depth <= 0)
        {
            // Ran dry: rebuild a cushion before playing again.
            m_buffering = true;
            ++m_stats.underruns;
            if (m_automatic)
            {
                // The measurement missed something: keep an extra frame for a while.
                m_boostFrames = std::min<uint32_t>(m_boostFrames + 1, 5);
                m_boostAt = NowMs();
                ApplyTargetLocked(std::min(m_targetFrames + 1, kAutoMaxFrames));
                m_lowSince = -1;
            }
            return false;
        }
        else if (depth > static_cast<int32_t>(m_maxFrames))
        {
            // Too much latency piled up while playing (burst after a short stall).
            SkipToTargetLocked();
        }

        Slot& slot = m_slots[m_playSeq % kSlots];
        if (slot.valid && slot.seq == m_playSeq)
        {
            for (uint32_t i = 0; i < kFrameSamples; ++i)
            {
                m_frame[i] = slot.data[i] / 32768.f;
            }
            slot.valid = false;
            if (m_concealRun > 0)
            {
                for (uint32_t i = 0; i < kFadeSamples; ++i)
                {
                    m_frame[i] *= static_cast<float>(i) / kFadeSamples;
                }
            }
            m_lastGood = m_frame;
            m_concealRun = 0;
        }
        else
        {
            // Lost packet: replay the previous frame fading out, then silence.
            ++m_stats.concealed;
            const float startGain = (m_concealRun == 0) ? 1.f : 0.f;
            for (uint32_t i = 0; i < kFrameSamples; ++i)
            {
                const float fade = 1.f - static_cast<float>(i) / kFrameSamples;
                m_frame[i] = m_lastGood[i] * startGain * fade;
            }
            ++m_concealRun;
        }

        ++m_playSeq;
        m_framePos = 0;
        return true;
    }

    void JitterBuffer::SkipToTargetLocked()
    {
        const uint32_t newPlay = m_highSeq - m_targetFrames + 1;
        for (uint32_t s = m_playSeq; s != newPlay; ++s)
        {
            Slot& stale = m_slots[s % kSlots];
            if (stale.seq == s)
            {
                stale.valid = false;
            }
        }
        m_playSeq = newPlay;
        m_avgDepth = static_cast<double>(m_targetFrames) * kFrameSamples;
        ++m_stats.skips;
    }

    float JitterBuffer::NextSampleLocked()
    {
        if (m_framePos >= kFrameSamples && !PopFrameLocked())
        {
            return 0.f;
        }
        return m_frame[m_framePos++];
    }

    void JitterBuffer::Read(float* out, uint32_t count)
    {
        std::lock_guard guard(m_lock);

        const double target = static_cast<double>(m_targetFrames) * kFrameSamples;
        double ratio = 1.0;
        const bool playing = m_hasStream && !m_buffering;
        if (playing)
        {
            const double depth =
                std::max(0, DepthFramesLocked()) * static_cast<double>(kFrameSamples) +
                (kFrameSamples - std::min(m_framePos, kFrameSamples));
            m_avgDepth += (depth - m_avgDepth) * kDepthSmoothing;
            ratio = 1.0 + std::clamp((m_avgDepth - target) * kDriftGain, -kMaxDrift, kMaxDrift);
        }
        else
        {
            m_avgDepth = target;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            while (m_frac >= 1.0)
            {
                m_prev = m_cur;
                m_cur = NextSampleLocked();
                m_frac -= 1.0;
            }
            out[i] = m_prev + (m_cur - m_prev) * static_cast<float>(m_frac);
            m_frac += ratio;
        }

        m_stats.playing = m_hasStream && !m_buffering;
        m_stats.targetMs = m_targetFrames * 10;
        m_stats.automatic = m_automatic;
        m_stats.depthMs = m_stats.playing ? m_avgDepth / (kSampleRate / 1000.0) : 0.0;
    }

    JitterBuffer::Stats JitterBuffer::GetStats()
    {
        std::lock_guard guard(m_lock);
        return m_stats;
    }
}
