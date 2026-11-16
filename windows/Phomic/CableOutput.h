#pragma once

namespace phomic
{
    // Writes audio straight into VB-CABLE's playback-side driver pin with kernel
    // streaming (WaveRT: the driver maps a cyclic buffer into this process and we stay
    // a little ahead of its play position), instead of through a Windows playback
    // endpoint. That lets VB-CABLE's playback endpoints stay disabled, so the only
    // device other apps see is the "Phomic" microphone.
    class CableOutput
    {
    public:
        using Source = std::function<void(float* mono, uint32_t frames)>;

        explicit CableOutput(Source source);
        ~CableOutput();
        CableOutput(CableOutput const&) = delete;
        CableOutput& operator=(CableOutput const&) = delete;

        // VB-CABLE is installed and its pin supports direct (WaveRT) streaming.
        static bool IsAvailable();

        bool Start();
        void Stop();

        bool IsRunning() const { return m_running; }
        bool HasFailed() const { return m_failed; }
        std::wstring LastError();

    private:
        void Run(std::promise<HRESULT> ready);
        HRESULT StreamLoop(std::promise<HRESULT>& ready, bool& signaled);
        void SetError(HRESULT hr, std::wstring_view what);

        Source m_source;
        std::thread m_thread;
        HANDLE m_wake = nullptr;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_failed{ false };
        std::mutex m_errorLock;
        std::wstring m_error;
    };
}
