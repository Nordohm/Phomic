#pragma once

namespace phomic
{
    struct AudioDevice
    {
        std::wstring id;
        std::wstring name;
        bool isDefault = false;
    };

    // Active playback endpoints, sorted by name. Throws winrt::hresult_error on failure.
    std::vector<AudioDevice> EnumerateRenderDevices();

    // True for endpoints of virtual audio cable drivers (VB-CABLE, VoiceMeeter, VAC...),
    // whose playback side is looped back into a recording device other apps can use.
    bool IsVirtualCable(std::wstring_view name);

    // Raises a flag whenever playback devices are added, removed or changed.
    class DeviceWatcher
    {
    public:
        DeviceWatcher();
        ~DeviceWatcher();
        DeviceWatcher(DeviceWatcher const&) = delete;
        DeviceWatcher& operator=(DeviceWatcher const&) = delete;

        // Returns true once after any change.
        bool TakeChanged() { return m_changed->exchange(false); }

    private:
        struct Client;
        std::shared_ptr<std::atomic<bool>> m_changed;
        winrt::com_ptr<IMMDeviceEnumerator> m_enumerator;
        winrt::com_ptr<IMMNotificationClient> m_client;
    };

    // Event-driven WASAPI shared-mode playback. Pulls 48 kHz mono float samples
    // from `source` on a dedicated MMCSS ("Pro Audio") thread; Windows converts
    // to the device's own rate and channel layout.
    class AudioOutput
    {
    public:
        using Source = std::function<void(float* mono, uint32_t frames)>;

        explicit AudioOutput(Source source);
        ~AudioOutput();
        AudioOutput(AudioOutput const&) = delete;
        AudioOutput& operator=(AudioOutput const&) = delete;

        // Opens `deviceId` (empty = the default playback device) and starts playing.
        bool Start(std::wstring const& deviceId);
        void Stop();

        bool IsRunning() const { return m_running; }
        // The device went away or errored after it had started.
        bool HasFailed() const { return m_failed; }
        std::wstring LastError();
        std::wstring const& DeviceId() const { return m_deviceId; }

    private:
        void Run(std::wstring deviceId, std::promise<HRESULT> ready);
        HRESULT RenderLoop(std::wstring const& deviceId, std::promise<HRESULT>& ready, bool& signaled);
        void SetError(HRESULT hr, std::wstring_view what);

        Source m_source;
        std::thread m_thread;
        HANDLE m_wake = nullptr;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_failed{ false };
        std::mutex m_errorLock;
        std::wstring m_error;
        std::wstring m_deviceId;
    };
}
