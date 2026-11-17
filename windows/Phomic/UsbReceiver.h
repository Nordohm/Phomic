#pragma once

#include "PhoneProtocol.h"

namespace phomic
{
    class JitterBuffer;

    // Why USB can't work right now, as far as Windows can tell (see AppleUsb.h).
    enum class UsbIssue
    {
        None,
        NoAppleDevices,         // neither Apple Devices nor Apple Mobile Device Support is installed
        StartingService,        // waiting for the service to start (by itself, or because Phomic asked)
        ServiceNotRunning,      // Apple Devices is installed, but its USB service didn't start
        StartupDisabled,        // ... and it's turned off under Settings > Apps > Startup
        PortTaken,              // another program listens on Apple's port
        PortForward,            // a netsh portproxy rule listens on Apple's port
        ClassicServiceStopped,  // only Apple Mobile Device Support, and its service isn't running
        NotSeenByApple,         // Windows sees an iPhone that Apple's service doesn't list
        DeviceUnreachable,      // Apple's service lists the iPhone but can't open anything on it
    };

    // Receives audio from an iPhone plugged into this PC with a cable.
    //
    // Windows has no USB networking to an iPhone of its own. Apple's USB service does: the
    // Apple Devices app from the Microsoft Store (or Apple Mobile Device Support, which
    // comes with iTunes) runs "usbmux" on 127.0.0.1:kUsbmuxPort, which reports iPhones as
    // they're plugged in and opens TCP connections to ports on them, through the cable.
    //   - A "Listen" connection to usbmux tells us which iPhones are plugged in.
    //   - For each one, we try to connect to kUsbPort on the iPhone a few times a second.
    //     Phomic on the iPhone listens there while USB is picked in the app, so the
    //     connection works as soon as both are true; until then the iPhone refuses it.
    //   - The iPhone says hello, the PC answers welcome, and then the same control lines
    //     as over Wi-Fi go both ways, with the audio packets in between (see Protocol.h).
    // The cable and the iPhone's "Trust This Computer?" already prove whose iPhone it is,
    // so USB doesn't ask for the pairing code.
    //
    // All socket work happens on one background thread.
    class UsbReceiver
    {
    public:
        struct Status
        {
            bool enabled = true;           // user setting
            bool checked = false;          // looked for Apple's USB service at least once
            bool serviceRunning = false;   // Apple's USB service answers
            uint32_t devices = 0;          // Apple devices plugged in with a cable
            uint32_t windowsDevices = 0;   // iPhones and iPads Windows itself sees on USB
            UsbIssue issue = UsbIssue::None;
            std::wstring portHolder;       // PortTaken / PortForward: the program, if Windows says
            std::wstring forwardAddress;   // PortForward: the rule's listen address

            bool connected = false;
            std::wstring deviceName;
            bool muted = false;
            double rttMs = -1;
            uint64_t packets = 0;
            uint64_t msSinceAudio = UINT64_MAX;
            std::wstring lastDisconnect;
        };

        UsbReceiver(JitterBuffer& jitter, SourceArbiter& arbiter, std::wstring pcName);
        ~UsbReceiver();
        UsbReceiver(UsbReceiver const&) = delete;
        UsbReceiver& operator=(UsbReceiver const&) = delete;

        void SetEnabled(bool enabled);
        // Ends the session; the iPhone stops listening when it gets "bye" (thread-safe).
        void Disconnect() { m_disconnectRequested = true; }
        // Ask the connected phone to move the session to another transport (thread-safe).
        void RequestSwitch(Source to) { m_switchTo = static_cast<int>(to); }
        // Start Apple Devices' USB service now, without its window (thread-safe).
        void StartAppleService()
        {
            m_startRequested = true;
            m_wake.notify_all();
        }
        void Shutdown();

        Status GetStatus();
        float TakePeak() { return m_peak.exchange(0.f); }

    private:
        struct Device
        {
            uint32_t id = 0;
            uint64_t nextAttempt = 0;
            // When Apple's service started closing our Connect requests without an answer.
            // (A plain "refused" is normal: Phomic on the iPhone isn't on USB.)
            uint64_t failingSince = 0;
        };

        void Run();
        bool OpenListen();
        void ReadListen();
        void CloseListen(bool serviceGone);
        void TryDevices();
        void Diagnose();
        void StartServiceNow();
        SOCKET ConnectToDevice(uint32_t deviceId, int& result);

        void ReadSession();
        void ProcessSession();
        bool HandleHello(std::string const& line);
        void HandleMessage(std::string const& line);
        void HandleAudio(protocol::Audio const& audio);
        void CheckSession();
        void EndSession(std::wstring const& reason, bool sendBye, uint64_t retryAfterMs);
        void SendLine(std::string const& json);

        JitterBuffer& m_jitter;
        SourceArbiter& m_arbiter;
        const std::wstring m_pcName;

        std::thread m_thread;
        std::mutex m_wakeLock;
        std::condition_variable m_wake;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_enabled{ true };
        std::atomic<bool> m_disconnectRequested{ false };
        std::atomic<int> m_switchTo{ 0 };   // a Source, 0 = none
        std::atomic<bool> m_startRequested{ false };
        std::atomic<float> m_peak{ 0.f };

        // usbmux "Listen" connection: Attached / Detached events.
        SOCKET m_listen = INVALID_SOCKET;
        std::string m_listenRx;
        uint64_t m_nextServiceTry = 0;
        uint64_t m_nextDiagnosis = 0;
        uint64_t m_unseenSince = 0;     // when Windows started seeing more iPhones than usbmux
        uint64_t m_nextAutoStart = 0;   // when Phomic may start Apple's service by itself again
        uint64_t m_startedServiceAt = 0;
        bool m_triedStart = false;      // since the service was last seen
        std::vector<Device> m_devices;

        // The tunnel to Phomic on the iPhone, before and after hello.
        SOCKET m_session = INVALID_SOCKET;
        uint32_t m_sessionDevice = 0;
        bool m_welcomed = false;
        std::string m_rx;
        uint64_t m_openedAt = 0;
        uint64_t m_lastRx = 0;
        uint64_t m_lastAudioTick = 0;
        uint32_t m_sessionId = 0;
        std::mt19937 m_rng{ std::random_device{}() };

        std::mutex m_statusLock;
        Status m_status;
    };
}
