#pragma once

#include "Protocol.h"

namespace phomic
{
    // Reorders incoming 10 ms frames by sequence number, hides packet loss, and
    // feeds a continuous 48 kHz mono stream to the audio device.
    //
    // The phone's audio clock and the PC's audio clock never run at exactly the
    // same speed, so Read() resamples by up to +/-0.5% to keep the buffer near its
    // target depth instead of periodically glitching (underrun) or drifting into
    // seconds of latency (overrun).
    //
    // Push() runs on the network thread, Read() on the audio thread.
    class JitterBuffer
    {
    public:
        struct Stats
        {
            uint64_t received = 0;   // frames that arrived in time
            uint64_t concealed = 0;  // frames that never arrived and were patched over
            uint64_t late = 0;       // frames that arrived after their play time
            uint64_t underruns = 0;  // buffer ran dry and had to rebuffer
            uint64_t skips = 0;      // buffer got too deep and old audio was dropped
            double depthMs = 0;      // current smoothed buffer depth
            uint32_t targetMs = 0;   // what the buffer aims for (changes by itself in Auto)
            bool automatic = false;
            bool playing = false;
        };

        JitterBuffer();

        // Forget everything (new session / disconnect).
        void Reset();

        // Fixed buffering, in 10 ms frames.
        void SetTargetFrames(uint32_t frames);
        // Auto: follow the measured arrival jitter, never below `minFrames`.
        void SetAutomatic(uint32_t minFrames);

        // `measureTiming`: false for frames that didn't arrive by themselves (e.g. concealment).
        void Push(uint32_t seq, const int16_t* samples, bool measureTiming = true);

        // Fill `count` mono float samples. Always succeeds; produces silence when
        // there is nothing to play.
        void Read(float* out, uint32_t count);

        Stats GetStats();

    private:
        static constexpr uint32_t kSlots = 128;

        struct Slot
        {
            uint32_t seq = 0;
            bool valid = false;
            std::array<int16_t, kFrameSamples> data{};
        };

        static int32_t SeqDiff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

        void ResetLocked();
        int32_t DepthFramesLocked() const;
        bool PopFrameLocked();
        void SkipToTargetLocked();
        void ApplyTargetLocked(uint32_t frames);
        void MeasureArrivalLocked(uint32_t seq);
        void UpdateAutoTargetLocked();
        double NowMs() const;
        float NextSampleLocked();

        std::mutex m_lock;
        std::vector<Slot> m_slots;

        bool m_hasStream = false;
        bool m_buffering = true;
        uint32_t m_playSeq = 0;
        uint32_t m_highSeq = 0;
        uint32_t m_targetFrames = 6;
        uint32_t m_maxFrames = 16;

        // Current frame being played out.
        std::array<float, kFrameSamples> m_frame{};
        uint32_t m_framePos = kFrameSamples;
        std::array<float, kFrameSamples> m_lastGood{};
        uint32_t m_concealRun = 0;

        // Drift-correcting linear resampler state.
        double m_frac = 1.0;
        float m_prev = 0.f;
        float m_cur = 0.f;
        double m_avgDepth = 0;

        // Auto buffering: arrival offset of each packet against its 10 ms slot.
        static constexpr size_t kTimingWindow = 500; // 5 s of packets
        bool m_automatic = false;
        uint32_t m_minFrames = 2;
        std::vector<double> m_offsets = std::vector<double>(kTimingWindow, 0.0);
        size_t m_offsetCount = 0;
        size_t m_offsetNext = 0;
        bool m_haveTiming = false;
        uint32_t m_timingBaseSeq = 0;
        uint32_t m_measuredSinceUpdate = 0;
        uint32_t m_boostFrames = 0;     // extra cushion after recent underruns
        double m_boostAt = 0;
        double m_lowSince = -1;         // when a smaller target first looked enough
        std::vector<double> m_delays = std::vector<double>(kTimingWindow); // UpdateAutoTargetLocked's scratch: no allocating under the lock

        Stats m_stats;
    };
}
