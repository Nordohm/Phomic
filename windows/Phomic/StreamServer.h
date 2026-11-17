#pragma once

#include "Protocol.h"
#include "SourceArbiter.h"

namespace phomic
{
    class JitterBuffer;

    // Accepts one iPhone at a time.
    //  * TCP (control): hello/welcome handshake, pings, mute state, bye.
    //  * UDP (audio):   10 ms PCM packets, fed into the JitterBuffer.
    //  * DNS-SD:        advertises "<PC name>._phomic._tcp.local" so the phone finds us.
    // All socket work happens on one background thread.
    class StreamServer
    {
    public:
        struct Status
        {
            bool listening = false;
            bool advertised = false;
            std::wstring error;
            bool portInUse = false;    // Start failed because the port is taken (or still held)

            bool connected = false;
            std::wstring clientName;
            std::wstring clientAddress;
            bool muted = false;
            double rttMs = -1;
            uint64_t packets = 0;      // valid audio packets this session
            uint64_t msSinceAudio = 0; // time since the last audio packet
            std::wstring lastDisconnect;
        };

        StreamServer(JitterBuffer& jitter, SourceArbiter& arbiter);
        ~StreamServer();
        StreamServer(StreamServer const&) = delete;
        StreamServer& operator=(StreamServer const&) = delete;

        bool Start(uint16_t port);
        void Stop();

        // Ask the connected phone to disconnect (thread-safe).
        void Disconnect();
        // Ask the connected phone to move the session to another transport (thread-safe).
        void RequestSwitch(Source to) { m_switchTo = static_cast<int>(to); }
        // Re-publish the Bonjour record (after the pairing setting changed). UI thread.
        void RefreshAdvertisement();
        // Finishes re-publishing (see RefreshAdvertisement). UI thread, every ~50 ms.
        void TickAdvertisement();

        Status GetStatus();
        uint16_t Port() const { return m_port; }
        std::wstring const& ServiceName() const { return m_serviceName; }

        // Highest absolute sample value (0..1) since the last call.
        float TakePeak() { return m_peak.exchange(0.f); }

    private:
        struct Connection
        {
            SOCKET socket = INVALID_SOCKET;
            std::string rx;
            uint64_t openedAt = 0;
            uint64_t lastRx = 0;
            std::wstring address;
        };

        void Run();
        void AcceptClient();
        void DrainUdp();
        void ReadFrom(bool active);
        void ProcessLines(bool active);
        bool HandleHello(std::string const& line);
        void HandleMessage(std::string const& line);
        void CheckTimeouts();
        void DropActive(std::wstring const& reason, bool sendBye);
        void ClosePending();
        void CloseSockets();

        void RegisterService();
        void UnregisterService();
        static void WINAPI OnServiceRegistered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);

        JitterBuffer& m_jitter;
        SourceArbiter& m_arbiter;
        uint16_t m_port = kDefaultPort;
        std::wstring m_computerName;
        std::wstring m_serviceName;

        std::thread m_thread;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_disconnectRequested{ false };
        std::atomic<int> m_switchTo{ 0 };   // a Source, 0 = none
        std::atomic<float> m_peak{ 0.f };

        SOCKET m_listen = INVALID_SOCKET;
        SOCKET m_udp = INVALID_SOCKET;
        Connection m_active;
        Connection m_pending;
        uint32_t m_session = 0;
        std::mt19937 m_rng{ std::random_device{}() };

        std::mutex m_statusLock;
        Status m_status;
        uint64_t m_lastAudioTick = 0;

        // DNS-SD registration (must outlive the async register/deregister calls).
        PDNS_SERVICE_INSTANCE m_dnsInstance = nullptr;
        DNS_SERVICE_REGISTER_REQUEST m_dnsRequest{};
        HANDLE m_dnsDone = nullptr;
        bool m_dnsRegistered = false;
        std::atomic<bool> m_dnsDeregistering{ false };
        // What Windows actually registered: on a name conflict it registers "NAME (1)" instead,
        // and deregistering has to use that name.
        std::mutex m_dnsLock;
        PDNS_SERVICE_INSTANCE m_dnsActual = nullptr;
        std::wstring m_dnsRequestedName;
        std::atomic<bool> m_dnsRenamed{ false };
        uint64_t m_registerAt = 0;   // UI thread: when to register again (0 = not pending)
        int m_renameRetries = 0;     // UI thread
    };

    // IPv4 addresses of this PC's active network adapters (for manual connection).
    std::vector<std::wstring> LocalIPv4Addresses();
}
