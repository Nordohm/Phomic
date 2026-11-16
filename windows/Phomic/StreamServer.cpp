#include "pch.h"
#include "StreamServer.h"
#include "JitterBuffer.h"
#include "JsonUtil.h"
#include "Pairing.h"

using namespace winrt::Windows::Data::Json;

namespace phomic
{
    namespace
    {
        uint64_t Now()
        {
            return GetTickCount64();
        }

        std::wstring AddressToString(sockaddr_storage const& addr)
        {
            wchar_t buf[INET6_ADDRSTRLEN] = {};
            if (addr.ss_family == AF_INET6)
            {
                auto const& a6 = reinterpret_cast<sockaddr_in6 const&>(addr);
                if (IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr))
                {
                    in_addr v4{};
                    std::memcpy(&v4, &a6.sin6_addr.s6_addr[12], sizeof(v4));
                    InetNtopW(AF_INET, &v4, buf, ARRAYSIZE(buf));
                }
                else
                {
                    InetNtopW(AF_INET6, &a6.sin6_addr, buf, ARRAYSIZE(buf));
                }
            }
            else if (addr.ss_family == AF_INET)
            {
                InetNtopW(AF_INET, &reinterpret_cast<sockaddr_in const&>(addr).sin_addr, buf, ARRAYSIZE(buf));
            }
            return buf;
        }

        // One dual-stack IPv6 socket accepts IPv4 too; fall back to IPv4-only.
        SOCKET OpenSocket(int type, int protocol, uint16_t port, int& error)
        {
            const BOOL exclusive = TRUE;
            SOCKET s = socket(AF_INET6, type, protocol);
            if (s != INVALID_SOCKET)
            {
                const DWORD off = 0;
                setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
                setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
                sockaddr_in6 addr{};
                addr.sin6_family = AF_INET6;
                addr.sin6_port = htons(port);
                if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                {
                    return s;
                }
                error = WSAGetLastError();
                closesocket(s);
                if (error == WSAEADDRINUSE)
                {
                    return INVALID_SOCKET;
                }
            }

            s = socket(AF_INET, type, protocol);
            if (s == INVALID_SOCKET)
            {
                error = WSAGetLastError();
                return INVALID_SOCKET;
            }
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            {
                return s;
            }
            error = WSAGetLastError();
            closesocket(s);
            return INVALID_SOCKET;
        }

        void SetNonBlocking(SOCKET s)
        {
            u_long on = 1;
            ioctlsocket(s, FIONBIO, &on);
        }

        void CloseSocket(SOCKET& s)
        {
            if (s != INVALID_SOCKET)
            {
                shutdown(s, SD_BOTH);
                closesocket(s);
                s = INVALID_SOCKET;
            }
        }
    }

    StreamServer::StreamServer(JitterBuffer& jitter, SourceArbiter& arbiter) : m_jitter(jitter), m_arbiter(arbiter)
    {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);

        wchar_t name[256] = {};
        DWORD size = ARRAYSIZE(name);
        if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) && size > 0)
        {
            m_computerName = name;
        }
        else
        {
            m_computerName = L"Windows-PC";
        }
        m_serviceName = m_computerName;
        m_dnsDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    StreamServer::~StreamServer()
    {
        Stop();
        if (m_dnsDone)
        {
            CloseHandle(m_dnsDone);
        }
        WSACleanup();
    }

    bool StreamServer::Start(uint16_t port)
    {
        Stop();
        m_port = port;

        int error = 0;
        m_listen = OpenSocket(SOCK_STREAM, IPPROTO_TCP, port, error);
        if (m_listen != INVALID_SOCKET && listen(m_listen, 4) != 0)
        {
            error = WSAGetLastError();
            CloseSocket(m_listen);
        }
        if (m_listen != INVALID_SOCKET)
        {
            m_udp = OpenSocket(SOCK_DGRAM, IPPROTO_UDP, port, error);
        }
        if (m_listen == INVALID_SOCKET || m_udp == INVALID_SOCKET)
        {
            CloseSockets();
            std::lock_guard guard(m_statusLock);
            m_status = {};
            m_status.portInUse = error == WSAEADDRINUSE;
            m_status.error = (error == WSAEADDRINUSE)
                ? std::format(L"Network port {} is already in use. Is Phomic already running?", port)
                : std::format(L"Couldn't open network port {} (Winsock error {}).", port, error);
            return false;
        }

        SetNonBlocking(m_listen);
        SetNonBlocking(m_udp);
        const int receiveBuffer = 512 * 1024;
        setsockopt(m_udp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));

        {
            std::lock_guard guard(m_statusLock);
            m_status = {};
            m_status.listening = true;
        }

        RegisterService();

        m_stop = false;
        m_thread = std::thread(&StreamServer::Run, this);
        return true;
    }

    void StreamServer::Stop()
    {
        m_registerAt = 0;
        if (m_thread.joinable())
        {
            m_stop = true;
            m_thread.join();
        }
        UnregisterService();
        CloseSockets();
    }

    void StreamServer::Disconnect()
    {
        m_disconnectRequested = true;
    }

    StreamServer::Status StreamServer::GetStatus()
    {
        std::lock_guard guard(m_statusLock);
        return m_status;
    }

    void StreamServer::CloseSockets()
    {
        CloseSocket(m_active.socket);
        CloseSocket(m_pending.socket);
        CloseSocket(m_listen);
        CloseSocket(m_udp);
    }

    // ---------------------------------------------------------------- network thread

    void StreamServer::Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        std::vector<WSAPOLLFD> fds;
        while (!m_stop)
        {
            fds.clear();
            fds.push_back({ m_listen, POLLRDNORM, 0 });
            fds.push_back({ m_udp, POLLRDNORM, 0 });
            size_t activeIndex = SIZE_MAX;
            size_t pendingIndex = SIZE_MAX;
            if (m_active.socket != INVALID_SOCKET)
            {
                activeIndex = fds.size();
                fds.push_back({ m_active.socket, POLLRDNORM, 0 });
            }
            if (m_pending.socket != INVALID_SOCKET)
            {
                pendingIndex = fds.size();
                fds.push_back({ m_pending.socket, POLLRDNORM, 0 });
            }

            const int ready = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 100);
            if (ready == SOCKET_ERROR)
            {
                Sleep(20);
            }
            else if (ready > 0)
            {
                constexpr SHORT kReadable = POLLRDNORM | POLLHUP | POLLERR | POLLNVAL;
                if (fds[1].revents & POLLRDNORM)
                {
                    DrainUdp();
                }
                if (activeIndex != SIZE_MAX && (fds[activeIndex].revents & kReadable))
                {
                    ReadFrom(true);
                }
                if (pendingIndex != SIZE_MAX && (fds[pendingIndex].revents & kReadable))
                {
                    ReadFrom(false);
                }
                if (fds[0].revents & POLLRDNORM)
                {
                    AcceptClient();
                }
            }

            CheckTimeouts();
            if (m_disconnectRequested.exchange(false))
            {
                DropActive(L"Disconnected from this PC.", true);
            }
            if (m_switchToBluetooth.exchange(false) && m_active.socket != INVALID_SOCKET)
            {
                SendLine(m_active.socket, R"({"type":"switch","transport":"bluetooth"})");
            }
            if (m_active.socket != INVALID_SOCKET && m_session != 0 && !m_arbiter.Owns(Source::WiFi))
            {
                // A Bluetooth connection took over.
                DropActive(L"Switched to Bluetooth.", true);
            }
        }

        DropActive(L"Phomic on the PC was closed.", true);
        ClosePending();
        winrt::uninit_apartment();
    }

    void StreamServer::AcceptClient()
    {
        for (;;)
        {
            sockaddr_storage addr{};
            int length = sizeof(addr);
            SOCKET s = accept(m_listen, reinterpret_cast<sockaddr*>(&addr), &length);
            if (s == INVALID_SOCKET)
            {
                return; // WSAEWOULDBLOCK: nothing more to accept
            }
            const BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            SetNonBlocking(s);

            // Only one handshake at a time; the newest connection wins.
            ClosePending();
            m_pending.socket = s;
            m_pending.rx.clear();
            m_pending.openedAt = m_pending.lastRx = Now();
            m_pending.address = AddressToString(addr);
        }
    }

    void StreamServer::DrainUdp()
    {
        uint8_t buf[2048];
        for (int i = 0; i < 512; ++i)
        {
            sockaddr_storage from{};
            int fromLength = sizeof(from);
            const int n = recvfrom(m_udp, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (n == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error == WSAECONNRESET || error == WSAEMSGSIZE)
                {
                    continue;
                }
                return; // WSAEWOULDBLOCK or a real error
            }
            if (n != static_cast<int>(kPacketBytes) || std::memcmp(buf, kPacketMagic, sizeof(kPacketMagic)) != 0)
            {
                continue;
            }

            uint32_t session = 0;
            uint32_t seq = 0;
            std::memcpy(&session, buf + 4, sizeof(session));
            std::memcpy(&seq, buf + 8, sizeof(seq));
            if (m_active.socket == INVALID_SOCKET || session != m_session || !m_arbiter.Owns(Source::WiFi))
            {
                continue;
            }

            int16_t samples[kFrameSamples];
            std::memcpy(samples, buf + kPacketHeaderBytes, sizeof(samples));
            int peak = 0;
            for (int16_t sample : samples)
            {
                peak = std::max(peak, std::abs(static_cast<int>(sample)));
            }
            const float level = peak / 32768.f;
            if (level > m_peak.load(std::memory_order_relaxed))
            {
                m_peak.store(level, std::memory_order_relaxed);
            }

            m_jitter.Push(seq, samples);
            m_lastAudioTick = Now();
            std::lock_guard guard(m_statusLock);
            ++m_status.packets;
        }
    }

    void StreamServer::ReadFrom(bool active)
    {
        Connection& c = active ? m_active : m_pending;
        if (c.socket == INVALID_SOCKET)
        {
            return;
        }

        char buf[4096];
        const int n = recv(c.socket, buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            return;
        }
        if (n <= 0)
        {
            if (active)
            {
                DropActive(L"The iPhone disconnected.", false);
            }
            else
            {
                ClosePending();
            }
            return;
        }

        c.lastRx = Now();
        c.rx.append(buf, static_cast<size_t>(n));
        if (c.rx.size() > 64 * 1024)
        {
            if (active)
            {
                DropActive(L"The iPhone sent invalid data.", true);
            }
            else
            {
                ClosePending();
            }
            return;
        }
        ProcessLines(active);
    }

    void StreamServer::ProcessLines(bool active)
    {
        for (;;)
        {
            Connection& c = active ? m_active : m_pending;
            if (c.socket == INVALID_SOCKET)
            {
                return;
            }
            const size_t newline = c.rx.find('\n');
            if (newline == std::string::npos)
            {
                return;
            }
            std::string line = c.rx.substr(0, newline);
            c.rx.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }

            if (active)
            {
                HandleMessage(line);
            }
            else if (HandleHello(line))
            {
                active = true; // the pending connection was promoted; keep reading it
            }
            else
            {
                return;
            }
        }
    }

    bool StreamServer::HandleHello(std::string const& line)
    {
        auto reject = [&](std::wstring_view why)
        {
            SendLine(m_pending.socket, std::format(R"({{"type":"error","message":"{}"}})", JsonEscape(std::wstring_view(why))));
            ClosePending();
            return false;
        };

        std::wstring name;
        try
        {
            JsonObject msg{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(line), msg) || msg.GetNamedString(L"type", L"") != L"hello")
            {
                ClosePending();
                return false;
            }
            if (static_cast<int>(msg.GetNamedNumber(L"version", 0)) != kProtocolVersion)
            {
                return reject(L"The iPhone app and the PC app are different versions. Please update both.");
            }
            if (static_cast<uint32_t>(msg.GetNamedNumber(L"sampleRate", 0)) != kSampleRate ||
                static_cast<int>(msg.GetNamedNumber(L"channels", 0)) != 1 ||
                static_cast<uint32_t>(msg.GetNamedNumber(L"frameSamples", 0)) != kFrameSamples)
            {
                return reject(L"Unsupported audio format.");
            }
            name = msg.GetNamedString(L"name", L"iPhone");
            const std::wstring code{ msg.GetNamedString(L"code", L"") };
            if (!pairing::Accepts(code))
            {
                // The phone asks the user for the code shown in this window and tries again.
                SendLine(m_pending.socket, std::format(
                    R"({{"type":"error","reason":"pairing","pcId":"{}","name":"{}","message":"{}"}})",
                    JsonEscape(std::wstring_view(pairing::PcId())), JsonEscape(std::wstring_view(m_computerName)),
                    code.empty() ? "This PC needs a pairing code." : "Wrong pairing code."));
                ClosePending();
                std::lock_guard guard(m_statusLock);
                m_status.lastDisconnect = std::format(L"{} was turned away: {}.", name.empty() ? L"An iPhone" : name,
                    code.empty() ? L"no pairing code" : L"wrong pairing code");
                return false;
            }
        }
        catch (winrt::hresult_error const&)
        {
            ClosePending();
            return false;
        }
        if (name.empty())
        {
            name = L"iPhone";
        }

        // Newest phone wins: a reconnecting phone must not be locked out by its own stale connection.
        DropActive(L"Replaced by a new connection.", true);

        m_active = std::move(m_pending);
        m_pending = Connection{};
        do
        {
            m_session = static_cast<uint32_t>(m_rng());
        } while (m_session == 0);
        m_arbiter.Force(Source::WiFi); // newest connection wins; a Bluetooth link hangs up on its own
        m_jitter.Reset();
        m_lastAudioTick = 0;

        SendLine(m_active.socket, std::format(R"({{"type":"welcome","version":{},"name":"{}","pcId":"{}","udpPort":{},"session":{}}})",
            kProtocolVersion, JsonEscape(std::wstring_view(m_computerName)), JsonEscape(std::wstring_view(pairing::PcId())),
            m_port, m_session));

        std::lock_guard guard(m_statusLock);
        m_status.connected = true;
        m_status.clientName = name;
        m_status.clientAddress = m_active.address;
        m_status.muted = false;
        m_status.rttMs = -1;
        m_status.packets = 0;
        m_status.lastDisconnect.clear();
        return true;
    }

    void StreamServer::HandleMessage(std::string const& line)
    {
        try
        {
            JsonObject msg{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(line), msg))
            {
                return;
            }
            const auto type = msg.GetNamedString(L"type", L"");
            if (type == L"ping")
            {
                const auto id = static_cast<int64_t>(msg.GetNamedNumber(L"id", 0));
                SendLine(m_active.socket, std::format(R"({{"type":"pong","id":{}}})", id));
                const double rtt = msg.GetNamedNumber(L"rtt", -1);
                std::lock_guard guard(m_statusLock);
                m_status.rttMs = rtt;
            }
            else if (type == L"mute")
            {
                const bool muted = msg.GetNamedBoolean(L"muted", false);
                std::lock_guard guard(m_statusLock);
                m_status.muted = muted;
            }
            else if (type == L"bye")
            {
                DropActive(L"The iPhone disconnected.", false);
            }
        }
        catch (winrt::hresult_error const&)
        {
            // Ignore malformed messages.
        }
    }

    void StreamServer::CheckTimeouts()
    {
        const uint64_t now = Now();
        if (m_pending.socket != INVALID_SOCKET && now - m_pending.openedAt > kHelloTimeoutMs)
        {
            ClosePending();
        }
        if (m_active.socket != INVALID_SOCKET && now - m_active.lastRx > kClientTimeoutMs)
        {
            DropActive(L"Lost contact with the iPhone.", true);
        }
        std::lock_guard guard(m_statusLock);
        m_status.msSinceAudio = m_lastAudioTick ? now - m_lastAudioTick : UINT64_MAX;
    }

    void StreamServer::DropActive(std::wstring const& reason, bool sendBye)
    {
        if (m_active.socket == INVALID_SOCKET)
        {
            return;
        }
        if (sendBye)
        {
            SendLine(m_active.socket, R"({"type":"bye"})");
        }
        CloseSocket(m_active.socket);
        m_active = Connection{};
        m_session = 0;
        m_lastAudioTick = 0;
        if (m_arbiter.Owns(Source::WiFi))
        {
            m_arbiter.Release(Source::WiFi);
            m_jitter.Reset();
        }

        std::lock_guard guard(m_statusLock);
        m_status.connected = false;
        m_status.clientName.clear();
        m_status.clientAddress.clear();
        m_status.muted = false;
        m_status.rttMs = -1;
        m_status.lastDisconnect = reason;
    }

    void StreamServer::ClosePending()
    {
        CloseSocket(m_pending.socket);
        m_pending = Connection{};
    }

    void StreamServer::SendLine(SOCKET socket, std::string const& json)
    {
        if (socket == INVALID_SOCKET)
        {
            return;
        }
        const std::string data = json + "\n";
        const char* p = data.data();
        int left = static_cast<int>(data.size());
        for (int attempt = 0; left > 0 && attempt < 50; ++attempt)
        {
            const int n = send(socket, p, left, 0);
            if (n == SOCKET_ERROR)
            {
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                {
                    Sleep(1);
                    continue;
                }
                return;
            }
            p += n;
            left -= n;
        }
    }

    // ---------------------------------------------------------------- DNS-SD

    void StreamServer::RegisterService()
    {
        const std::wstring instanceName = m_computerName + L"." + kServiceType;
        const std::wstring hostName = m_computerName + L".local";
        m_dnsRequestedName = instanceName;
        // TXT: protocol version, this PC's id (same as over Bluetooth), and whether a code is needed.
        const std::wstring id = pairing::PcId();
        PCWSTR keys[] = { L"v", L"id", L"pair" };
        PCWSTR values[] = { L"1", id.c_str(), pairing::Required() ? L"1" : L"0" };

        m_dnsInstance = DnsServiceConstructInstance(instanceName.c_str(), hostName.c_str(), nullptr, nullptr,
            m_port, 0, 0, 3, keys, values);
        if (!m_dnsInstance)
        {
            return;
        }

        m_dnsRequest = {};
        m_dnsRequest.Version = DNS_QUERY_REQUEST_VERSION1;
        m_dnsRequest.InterfaceIndex = 0;
        m_dnsRequest.pServiceInstance = m_dnsInstance;
        m_dnsRequest.pRegisterCompletionCallback = &StreamServer::OnServiceRegistered;
        m_dnsRequest.pQueryContext = this;
        m_dnsRequest.hCredentials = nullptr;
        m_dnsRequest.unicastEnabled = FALSE;

        m_dnsDeregistering = false;
        ResetEvent(m_dnsDone);
        if (DnsServiceRegister(&m_dnsRequest, nullptr) != DNS_REQUEST_PENDING)
        {
            DnsServiceFreeInstance(m_dnsInstance);
            m_dnsInstance = nullptr;
            return;
        }
        m_dnsRegistered = true;
    }

    void StreamServer::UnregisterService()
    {
        if (m_dnsRegistered)
        {
            m_dnsDeregistering = true;
            ResetEvent(m_dnsDone);
            // Deregister under the name Windows really used, or a renamed record lingers on.
            DNS_SERVICE_REGISTER_REQUEST request = m_dnsRequest;
            {
                std::lock_guard guard(m_dnsLock);
                if (m_dnsActual && m_dnsActual->pszInstanceName)
                {
                    request.pServiceInstance = m_dnsActual;
                }
            }
            if (DnsServiceDeRegister(&request, nullptr) == DNS_REQUEST_PENDING)
            {
                WaitForSingleObject(m_dnsDone, 2000);
            }
            m_dnsRegistered = false;
        }
        if (m_dnsInstance)
        {
            DnsServiceFreeInstance(m_dnsInstance);
            m_dnsInstance = nullptr;
        }
        {
            std::lock_guard guard(m_dnsLock);
            if (m_dnsActual)
            {
                DnsServiceFreeInstance(m_dnsActual);
                m_dnsActual = nullptr;
            }
        }
        m_dnsRenamed = false;
        std::lock_guard guard(m_statusLock);
        m_status.advertised = false;
    }

    void WINAPI StreamServer::OnServiceRegistered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
    {
        auto* self = static_cast<StreamServer*>(context);
        if (!self || self->m_dnsDeregistering)
        {
            if (instance)
            {
                DnsServiceFreeInstance(instance);
            }
            if (self)
            {
                SetEvent(self->m_dnsDone);
            }
            return;
        }
        {
            // Keep what was registered: UnregisterService needs its exact name.
            std::lock_guard guard(self->m_dnsLock);
            if (self->m_dnsActual)
            {
                DnsServiceFreeInstance(self->m_dnsActual);
            }
            self->m_dnsActual = instance;
            if (status == ERROR_SUCCESS && instance && instance->pszInstanceName &&
                _wcsicmp(instance->pszInstanceName, self->m_dnsRequestedName.c_str()) != 0)
            {
                self->m_dnsRenamed = true; // "NAME (1)": TickAdvertisement tries for the real name again
            }
        }
        {
            std::lock_guard guard(self->m_statusLock);
            self->m_status.advertised = (status == ERROR_SUCCESS);
        }
        SetEvent(self->m_dnsDone);
    }

    void StreamServer::RefreshAdvertisement()
    {
        if (m_listen == INVALID_SOCKET)
        {
            return;
        }
        UnregisterService();
        // Windows holds on to the old name for a moment; registering again straight away gets
        // "NAME (1)". TickAdvertisement registers again shortly.
        m_renameRetries = 0;
        m_registerAt = GetTickCount64() + 1500;
    }

    void StreamServer::TickAdvertisement()
    {
        if (m_listen == INVALID_SOCKET)
        {
            m_registerAt = 0;
            return;
        }
        const uint64_t now = GetTickCount64();
        if (m_registerAt != 0)
        {
            if (now >= m_registerAt)
            {
                m_registerAt = 0;
                RegisterService();
            }
        }
        else if (m_dnsRenamed.exchange(false) && m_renameRetries < 5)
        {
            // Registered as "NAME (1)" because an old record was still around: try again.
            ++m_renameRetries;
            UnregisterService();
            m_registerAt = now + 3000;
        }
    }

    // ---------------------------------------------------------------- helpers

    std::vector<std::wstring> LocalIPv4Addresses()
    {
        std::vector<std::wstring> result;
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
        if (rc == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
        }
        if (rc != NO_ERROR)
        {
            return result;
        }

        for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL)
            {
                continue;
            }
            for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
            {
                auto const* sin = reinterpret_cast<sockaddr_in const*>(unicast->Address.lpSockaddr);
                if (!sin || sin->sin_family != AF_INET)
                {
                    continue;
                }
                if (sin->sin_addr.S_un.S_un_b.s_b1 == 169 && sin->sin_addr.S_un.S_un_b.s_b2 == 254)
                {
                    continue; // link-local / no DHCP
                }
                wchar_t text[INET_ADDRSTRLEN] = {};
                InetNtopW(AF_INET, &sin->sin_addr, text, ARRAYSIZE(text));
                result.push_back(std::format(L"{}   ({})", text, adapter->FriendlyName ? adapter->FriendlyName : L"network"));
            }
        }
        return result;
    }
}
