#include "pch.h"
#include "UsbReceiver.h"
#include "AppleUsb.h"
#include "JitterBuffer.h"
#include "Net.h"
#include "PhoneProtocol.h"

#include <unordered_map>

namespace phomic
{
    namespace
    {
        // A plugged-in iPhone that isn't listening (Phomic closed, or not on USB) is asked
        // again this often, so switching to USB on the iPhone takes effect at once. Each try
        // is one small local request to Apple's service.
        constexpr uint64_t kRetryMs = 300;
        // How often to look for Apple's USB service while it isn't running.
        constexpr uint64_t kServiceRetryMs = 3000;
        // After the PC or the iPhone ended a session on purpose, wait a bit longer.
        constexpr uint64_t kAfterByeMs = 3000;
        // How often to look for what's wrong (all cheap, local queries).
        constexpr uint64_t kDiagnoseMs = 2000;
        // usbmux takes a moment to list a freshly plugged-in iPhone.
        constexpr uint64_t kUnseenMs = 6000;
        // Connect requests closed without an answer this long: the iPhone is out of reach.
        constexpr uint64_t kUnreachableMs = 5000;
        // Apple Devices starts its USB service at sign-in. If it isn't running this long after
        // Phomic started (or after it stopped), Phomic starts it, then at most once a minute.
        constexpr uint64_t kAutoStartGraceMs = 20000;
        constexpr uint64_t kAutoStartEveryMs = 60000;
        // The service listens within a few seconds of being started.
        constexpr uint64_t kStartingMs = 10000;
        // Apple's service answers on this PC within milliseconds; don't hold the worker up longer.
        constexpr int kRequestTimeoutMs = 1500;

        uint64_t Now()
        {
            return GetTickCount64();
        }

        using net::CloseSocket;
        using net::SetNonBlocking;

        // ------------------------------------------------------------ usbmux messages

        // usbmux frames each message with a 16-byte little-endian header: total length,
        // version (1 = plist), message type (8 = plist) and a tag echoed in the reply.
        constexpr size_t kHeaderBytes = 16;
        constexpr uint32_t kPlistVersion = 1;
        constexpr uint32_t kPlistMessage = 8;
        constexpr uint32_t kMaxMessageBytes = 1 << 20;

        std::string Request(std::string_view messageType, std::string_view extra, uint32_t tag)
        {
            const std::string body = std::format(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                "<plist version=\"1.0\">\n<dict>\n"
                "<key>MessageType</key><string>{}</string>\n"
                "<key>ClientVersionString</key><string>Phomic</string>\n"
                "<key>ProgName</key><string>Phomic</string>\n"
                "<key>kLibUSBMuxVersion</key><integer>3</integer>\n"
                "{}</dict>\n</plist>\n",
                messageType, extra);
            const uint32_t header[4] = { static_cast<uint32_t>(kHeaderBytes + body.size()), kPlistVersion, kPlistMessage, tag };
            std::string message(reinterpret_cast<const char*>(header), sizeof(header));
            message += body;
            return message;
        }

        // Flattens the plists usbmux sends into "Key" / "Parent.Key" -> text. They're small
        // dicts of strings and numbers; arrays are skipped.
        std::unordered_map<std::string, std::string> ParsePlist(std::string_view xml)
        {
            std::unordered_map<std::string, std::string> out;
            std::vector<std::string> path;   // the keys of the dicts we're inside
            std::string key;
            size_t pos = 0;

            auto prefix = [&]
            {
                std::string result;
                for (auto const& part : path)
                {
                    if (!part.empty())
                    {
                        result += part + ".";
                    }
                }
                return result;
            };
            auto unescape = [](std::string_view text)
            {
                std::string result;
                for (size_t i = 0; i < text.size(); ++i)
                {
                    if (text[i] == '&')
                    {
                        static constexpr std::pair<std::string_view, char> kEntities[] = {
                            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
                        bool matched = false;
                        for (auto const& [entity, ch] : kEntities)
                        {
                            if (text.substr(i, entity.size()) == entity)
                            {
                                result += ch;
                                i += entity.size() - 1;
                                matched = true;
                                break;
                            }
                        }
                        if (matched)
                        {
                            continue;
                        }
                    }
                    result += text[i];
                }
                return result;
            };

            for (;;)
            {
                const size_t open = xml.find('<', pos);
                if (open == std::string_view::npos)
                {
                    break;
                }
                const size_t close = xml.find('>', open);
                if (close == std::string_view::npos)
                {
                    break;
                }
                const std::string_view tag = xml.substr(open + 1, close - open - 1);
                pos = close + 1;
                if (tag.empty() || tag[0] == '?' || tag[0] == '!')
                {
                    continue;
                }

                if (tag == "dict")
                {
                    path.push_back(key);
                    key.clear();
                }
                else if (tag == "/dict")
                {
                    if (!path.empty())
                    {
                        path.pop_back();
                    }
                }
                else if (tag == "array")
                {
                    // Skip the whole array (usbmux doesn't nest anything we need in one).
                    int depth = 1;
                    while (depth > 0)
                    {
                        const size_t next = xml.find("array>", pos);
                        if (next == std::string_view::npos || next < 1)
                        {
                            return out;
                        }
                        depth += xml[next - 1] == '/' ? -1 : (xml[next - 1] == '<' ? 1 : 0);
                        pos = next + 6;
                    }
                    key.clear();
                }
                else if (tag == "true/" || tag == "false/")
                {
                    if (!key.empty())
                    {
                        out[prefix() + key] = tag == "true/" ? "true" : "false";
                    }
                    key.clear();
                }
                else if (tag.back() == '/')
                {
                    if (!key.empty())
                    {
                        out[prefix() + key] = ""; // e.g. <string/>
                    }
                    key.clear();
                }
                else if (tag[0] != '/')
                {
                    // <key>, <string>, <integer>, <real>, <date>, <data>, <plist>
                    if (tag.starts_with("plist"))
                    {
                        continue;
                    }
                    const std::string end = std::format("</{}>", tag);
                    const size_t finish = xml.find(end, pos);
                    if (finish == std::string_view::npos)
                    {
                        break;
                    }
                    const std::string text = unescape(xml.substr(pos, finish - pos));
                    pos = finish + end.size();
                    if (tag == "key")
                    {
                        key = text;
                    }
                    else
                    {
                        if (!key.empty())
                        {
                            out[prefix() + key] = text;
                        }
                        key.clear();
                    }
                }
            }
            return out;
        }

        uint32_t ToNumber(std::unordered_map<std::string, std::string> const& plist, std::string const& key, uint32_t fallback)
        {
            const auto it = plist.find(key);
            if (it == plist.end() || it->second.empty())
            {
                return fallback;
            }
            return static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
        }

        // Apple's port, unless PHOMIC_USBMUX_PORT points Phomic at a stand-in for testing
        // (a fake usbmux with fake iPhones, so USB can be tested without one plugged in).
        uint16_t ServicePort()
        {
            static const uint16_t port = []
            {
                wchar_t value[16] = {};
                const DWORD length = GetEnvironmentVariableW(L"PHOMIC_USBMUX_PORT", value, ARRAYSIZE(value));
                const unsigned long parsed = length > 0 && length < ARRAYSIZE(value) ? std::wcstoul(value, nullptr, 10) : 0;
                return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed) : kUsbmuxPort;
            }();
            return port;
        }

        // Connects to Apple's USB service. It only listens on IPv4 loopback.
        SOCKET ConnectToService(int timeoutMs)
        {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET)
            {
                return INVALID_SOCKET;
            }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(ServicePort());
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            // Non-blocking connect: with nothing listening, Windows takes a while to give up.
            SetNonBlocking(s, true);
            if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
            {
                closesocket(s);
                return INVALID_SOCKET;
            }
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            if (select(0, nullptr, &writable, &failed, &timeout) != 1 || !FD_ISSET(s, &writable))
            {
                closesocket(s);
                return INVALID_SOCKET;
            }
            SetNonBlocking(s, false);
            const DWORD ms = kRequestTimeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            return s;
        }

        bool SendAll(SOCKET s, std::string const& data)
        {
            size_t sent = 0;
            while (sent < data.size())
            {
                const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
                if (n <= 0)
                {
                    return false;
                }
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        bool ReceiveAll(SOCKET s, char* buffer, size_t length)
        {
            size_t received = 0;
            while (received < length)
            {
                const int n = recv(s, buffer + received, static_cast<int>(length - received), 0);
                if (n <= 0)
                {
                    return false;
                }
                received += static_cast<size_t>(n);
            }
            return true;
        }

        // One request and its reply on a blocking socket. Returns the reply's plist.
        std::optional<std::unordered_map<std::string, std::string>> Exchange(SOCKET s, std::string const& request)
        {
            if (!SendAll(s, request))
            {
                return std::nullopt;
            }
            uint32_t header[4] = {};
            if (!ReceiveAll(s, reinterpret_cast<char*>(header), sizeof(header)) ||
                header[0] < kHeaderBytes || header[0] > kMaxMessageBytes)
            {
                return std::nullopt;
            }
            std::string body(header[0] - kHeaderBytes, '\0');
            if (!body.empty() && !ReceiveAll(s, body.data(), body.size()))
            {
                return std::nullopt;
            }
            return ParsePlist(body);
        }
    }

    UsbReceiver::UsbReceiver(JitterBuffer& jitter, SourceArbiter& arbiter, std::wstring pcName)
        : m_jitter(jitter), m_arbiter(arbiter), m_pcName(std::move(pcName))
    {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        m_nextAutoStart = Now() + kAutoStartGraceMs;
        m_thread = std::thread([this] { Run(); });
    }

    UsbReceiver::~UsbReceiver()
    {
        Shutdown();
        WSACleanup();
    }

    void UsbReceiver::SetEnabled(bool enabled)
    {
        m_enabled = enabled;
        {
            std::lock_guard guard(m_statusLock);
            m_status.enabled = enabled;
        }
        m_wake.notify_all();
    }

    void UsbReceiver::Shutdown()
    {
        m_stop = true;
        m_wake.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    UsbReceiver::Status UsbReceiver::GetStatus()
    {
        std::lock_guard guard(m_statusLock);
        return m_status;
    }

    // ---------------------------------------------------------------- worker thread

    void UsbReceiver::Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        while (!m_stop)
        {
            if (!m_enabled)
            {
                EndSession(L"USB was turned off in Phomic.", true, 0);
                CloseListen(false);
                {
                    std::lock_guard guard(m_statusLock);
                    m_status.checked = false;
                    m_status.serviceRunning = false;
                    m_status.devices = 0;
                    m_status.issue = UsbIssue::None;
                }
                m_nextDiagnosis = 0;
                m_unseenSince = 0;
                std::unique_lock lock(m_wakeLock);
                m_wake.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_stop || m_enabled; });
                continue;
            }

            if (m_startRequested.exchange(false))
            {
                StartServiceNow();
            }
            if (m_listen == INVALID_SOCKET && Now() >= m_nextServiceTry && !OpenListen())
            {
                m_nextServiceTry = Now() + kServiceRetryMs;
            }

            WSAPOLLFD fds[2]{};
            ULONG count = 0;
            int listenIndex = -1;
            int sessionIndex = -1;
            if (m_listen != INVALID_SOCKET)
            {
                listenIndex = static_cast<int>(count);
                fds[count++] = { m_listen, POLLRDNORM, 0 };
            }
            if (m_session != INVALID_SOCKET)
            {
                sessionIndex = static_cast<int>(count);
                fds[count++] = { m_session, POLLRDNORM, 0 };
            }
            if (count == 0)
            {
                std::unique_lock lock(m_wakeLock);
                m_wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_stop || !m_enabled; });
            }
            else
            {
                const int ready = WSAPoll(fds, count, 100);
                constexpr SHORT kReadable = POLLRDNORM | POLLHUP | POLLERR | POLLNVAL;
                if (ready > 0)
                {
                    // The listen connection first: if Apple's service went away, that's the
                    // reason the session ends too.
                    if (listenIndex >= 0 && (fds[listenIndex].revents & kReadable))
                    {
                        ReadListen();
                    }
                    if (sessionIndex >= 0 && m_session != INVALID_SOCKET && (fds[sessionIndex].revents & kReadable))
                    {
                        ReadSession();
                    }
                }
                else if (ready == SOCKET_ERROR)
                {
                    Sleep(20);
                }
            }

            if (m_disconnectRequested.exchange(false))
            {
                EndSession(L"Disconnected from this PC.", true, kAfterByeMs);
            }
            if (const int to = m_switchTo.exchange(0); to != 0 && m_welcomed)
            {
                SendLine(protocol::Switch(static_cast<Source>(to)));
            }
            CheckSession();
            if (m_session == INVALID_SOCKET)
            {
                TryDevices();
            }
            if (Now() >= m_nextDiagnosis)
            {
                Diagnose();
                m_nextDiagnosis = Now() + kDiagnoseMs;
            }

            std::lock_guard guard(m_statusLock);
            m_status.devices = static_cast<uint32_t>(m_devices.size());
            m_status.msSinceAudio = m_lastAudioTick ? Now() - m_lastAudioTick : UINT64_MAX;
        }

        // No "bye": like a cable pulled out, so the iPhone waits a moment for Phomic to be back.
        EndSession(L"Phomic on the PC was closed.", false, 0);
        CloseListen(false);
        winrt::uninit_apartment();
    }

    // ---------------------------------------------------------------- Apple's USB service

    bool UsbReceiver::OpenListen()
    {
        SOCKET s = ConnectToService(1000);
        bool ok = false;
        if (s != INVALID_SOCKET)
        {
            const auto reply = Exchange(s, Request("Listen", "", 1));
            ok = reply && ToNumber(*reply, "Number", 1) == 0;
            if (!ok)
            {
                closesocket(s);
            }
        }

        {
            std::lock_guard guard(m_statusLock);
            m_status.checked = true;
            m_status.serviceRunning = ok;
        }
        if (!ok)
        {
            return false;
        }
        m_startedServiceAt = 0;
        m_triedStart = false;
        // From here on the service sends "Attached" for every Apple device that's plugged
        // in (including the ones already there), and "Detached" when one goes.
        SetNonBlocking(s, true);
        m_listen = s;
        m_listenRx.clear();
        m_devices.clear();
        return true;
    }

    void UsbReceiver::ReadListen()
    {
        char buf[8192];
        const int n = recv(m_listen, buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            return;
        }
        if (n <= 0)
        {
            // Apple's service stopped (Apple Devices was updated or uninstalled...).
            CloseListen(true);
            return;
        }
        m_listenRx.append(buf, static_cast<size_t>(n));

        while (m_listenRx.size() >= kHeaderBytes)
        {
            uint32_t header[4] = {};
            std::memcpy(header, m_listenRx.data(), sizeof(header));
            if (header[0] < kHeaderBytes || header[0] > kMaxMessageBytes)
            {
                CloseListen(true);
                return;
            }
            if (m_listenRx.size() < header[0])
            {
                return;
            }
            const auto message = ParsePlist(std::string_view(m_listenRx).substr(kHeaderBytes, header[0] - kHeaderBytes));
            m_listenRx.erase(0, header[0]);

            const auto type = message.find("MessageType");
            if (type == message.end())
            {
                continue;
            }
            const uint32_t id = ToNumber(message, "DeviceID", 0);
            if (type->second == "Attached" && id != 0)
            {
                // Over the network (Wi-Fi sync) usbmux reports devices too: USB only.
                const auto connection = message.find("Properties.ConnectionType");
                if (connection != message.end() && connection->second != "USB")
                {
                    continue;
                }
                if (std::none_of(m_devices.begin(), m_devices.end(), [id](Device const& d) { return d.id == id; }))
                {
                    // Give a freshly plugged-in iPhone a moment before the first try.
                    m_devices.push_back({ id, Now() + 300 });
                }
            }
            else if (type->second == "Detached" && id != 0)
            {
                std::erase_if(m_devices, [id](Device const& d) { return d.id == id; });
                if (m_session != INVALID_SOCKET && m_sessionDevice == id)
                {
                    EndSession(L"The iPhone was unplugged.", false, 0);
                }
            }
        }
    }

    void UsbReceiver::CloseListen(bool serviceGone)
    {
        if (serviceGone)
        {
            EndSession(L"Apple's USB service stopped.", false, 0);
            m_nextDiagnosis = 0;
            // Give it time to come back by itself (Apple Devices updating, say).
            m_nextAutoStart = std::max(m_nextAutoStart, Now() + kAutoStartGraceMs);
        }
        CloseSocket(m_listen);
        m_listenRx.clear();
        m_devices.clear();
        if (serviceGone)
        {
            m_nextServiceTry = Now() + kServiceRetryMs;
            std::lock_guard guard(m_statusLock);
            m_status.serviceRunning = false;
        }
    }

    // Works out why USB can't work, for the window to explain. Runs on the worker thread.
    void UsbReceiver::Diagnose()
    {
        bool checked = false;
        {
            std::lock_guard guard(m_statusLock);
            checked = m_status.checked;
        }
        const uint32_t windowsDevices = appleusb::AppleDevicesPluggedIn();
        UsbIssue issue = UsbIssue::None;
        appleusb::PortHolder holder;

        if (m_listen == INVALID_SOCKET)
        {
            m_unseenSince = 0;
            if (checked)
            {
                holder = appleusb::WhoHoldsPort(ServicePort());
                if (holder.held && !holder.apple)
                {
                    issue = holder.forwardAddress.empty() ? UsbIssue::PortTaken : UsbIssue::PortForward;
                }
                else if (appleusb::AppleDevicesInstalled())
                {
                    // Start it if it should be running. Not if the user turned it off under
                    // Startup apps: then the window offers to.
                    const bool startupOff = appleusb::AppleDevicesStartup() == appleusb::Startup::Disabled;
                    if (!startupOff && Now() >= m_nextAutoStart)
                    {
                        StartServiceNow();
                    }
                    // Until it's had its chance (its own start at sign-in, or ours), it's on its way.
                    const bool waiting = !startupOff && !m_triedStart;
                    const bool starting = m_startedServiceAt != 0 && Now() - m_startedServiceAt < kStartingMs;
                    issue = waiting || starting ? UsbIssue::StartingService
                        : startupOff ? UsbIssue::StartupDisabled : UsbIssue::ServiceNotRunning;
                }
                else
                {
                    switch (appleusb::ClassicServiceState())
                    {
                    case appleusb::ClassicService::Missing: issue = UsbIssue::NoAppleDevices; break;
                    case appleusb::ClassicService::Running: issue = UsbIssue::ServiceNotRunning; break;
                    default: issue = UsbIssue::ClassicServiceStopped; break;
                    }
                }
            }
        }
        else if (std::any_of(m_devices.begin(), m_devices.end(), [](Device const& d)
                 { return d.failingSince != 0 && Now() - d.failingSince > kUnreachableMs; }))
        {
            m_unseenSince = 0;
            issue = UsbIssue::DeviceUnreachable;
        }
        else if (windowsDevices > m_devices.size())
        {
            // E.g. Apple's service missed an iPhone that was plugged in while it started.
            if (m_unseenSince == 0)
            {
                m_unseenSince = Now();
            }
            if (Now() - m_unseenSince > kUnseenMs)
            {
                issue = UsbIssue::NotSeenByApple;
            }
        }
        else
        {
            m_unseenSince = 0;
        }

        std::lock_guard guard(m_statusLock);
        m_status.windowsDevices = windowsDevices;
        m_status.issue = issue;
        m_status.portHolder = holder.process;
        m_status.forwardAddress = holder.forwardAddress;
    }

    void UsbReceiver::StartServiceNow()
    {
        appleusb::StartAppleService();
        m_startedServiceAt = Now();
        m_triedStart = true;
        m_nextAutoStart = Now() + kAutoStartEveryMs;
        m_nextServiceTry = std::min(m_nextServiceTry, Now() + 1500);
        std::lock_guard guard(m_statusLock);
        m_status.issue = UsbIssue::StartingService;
    }

    void UsbReceiver::TryDevices()
    {
        const uint64_t now = Now();
        for (auto& device : m_devices)
        {
            if (m_stop || !m_enabled)
            {
                return;
            }
            if (now < device.nextAttempt)
            {
                continue;
            }
            int result = 0;
            const SOCKET s = ConnectToDevice(device.id, result);
            if (s == INVALID_SOCKET)
            {
                // Usually 3 (refused): Phomic on that iPhone isn't on USB right now. No answer
                // at all means Apple's service can't open anything on the iPhone; seen once,
                // with an iPhone whose own system port didn't answer either.
                device.nextAttempt = Now() + kRetryMs;
                if (result == 3)
                {
                    device.failingSince = 0;
                }
                else if (device.failingSince == 0)
                {
                    device.failingSince = Now();
                }
                continue;
            }
            device.failingSince = 0;
            m_session = s;
            m_sessionDevice = device.id;
            m_welcomed = false;
            m_rx.clear();
            m_openedAt = m_lastRx = Now();
            return;
        }
    }

    SOCKET UsbReceiver::ConnectToDevice(uint32_t deviceId, int& result)
    {
        result = -1;
        SOCKET s = ConnectToService(1000);
        if (s == INVALID_SOCKET)
        {
            return INVALID_SOCKET;
        }
        // usbmux wants the port in network byte order, inside a plist integer.
        const auto reply = Exchange(s, Request("Connect",
            std::format("<key>DeviceID</key><integer>{}</integer>\n<key>PortNumber</key><integer>{}</integer>\n",
                deviceId, htons(kUsbPort)), 2));
        result = reply ? static_cast<int>(ToNumber(*reply, "Number", 1)) : -1;
        if (result != 0)
        {
            // Close without TIME_WAIT: these tries repeat all day while an iPhone charges here,
            // and each would otherwise hold a local port for minutes.
            const linger now{ 1, 0 };
            setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&now), sizeof(now));
            closesocket(s);
            return INVALID_SOCKET;
        }
        // From now on the socket is a plain TCP stream to the iPhone.
        const DWORD none = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&none), sizeof(none));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&none), sizeof(none));
        const BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        SetNonBlocking(s, true);
        return s;
    }

    // ---------------------------------------------------------------- the session

    void UsbReceiver::ReadSession()
    {
        char buf[16384];
        const int n = recv(m_session, buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            return;
        }
        if (n <= 0)
        {
            EndSession(L"The iPhone disconnected.", false, kRetryMs);
            return;
        }
        m_lastRx = Now();
        m_rx.append(buf, static_cast<size_t>(n));
        ProcessSession();
    }

    void UsbReceiver::ProcessSession()
    {
        size_t pos = 0;
        while (m_session != INVALID_SOCKET && pos < m_rx.size())
        {
            if (m_rx[pos] == static_cast<char>(kPacketMagic[0]))
            {
                // An audio packet: the same bytes as a Wi-Fi datagram.
                if (m_rx.size() - pos < kPacketBytes)
                {
                    break;
                }
                protocol::Audio audio;
                if (!protocol::ParseAudio(reinterpret_cast<const uint8_t*>(m_rx.data() + pos), kPacketBytes, audio))
                {
                    EndSession(L"The iPhone sent invalid data.", true, kAfterByeMs);
                    return;
                }
                if (m_welcomed)
                {
                    HandleAudio(audio);
                }
                pos += kPacketBytes;
                continue;
            }

            const size_t newline = m_rx.find('\n', pos);
            if (newline == std::string::npos)
            {
                if (m_rx.size() - pos > 64 * 1024)
                {
                    EndSession(L"The iPhone sent invalid data.", true, kAfterByeMs);
                    return;
                }
                break;
            }
            std::string line = m_rx.substr(pos, newline - pos);
            pos = newline + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            if (!m_welcomed)
            {
                if (!HandleHello(line))
                {
                    return; // the session is closed
                }
            }
            else
            {
                HandleMessage(line);
            }
        }
        if (m_session != INVALID_SOCKET)
        {
            m_rx.erase(0, pos);
        }
    }

    // USB doesn't check the pairing code: the cable and the iPhone's "Trust" already did.
    bool UsbReceiver::HandleHello(std::string const& line)
    {
        const protocol::Hello hello = protocol::ParseHello(line);
        if (!hello.valid)
        {
            EndSession(L"", false, kAfterByeMs);
            return false;
        }
        if (!hello.refusal.empty())
        {
            SendLine(protocol::Error(hello.refusal));
            EndSession(L"", false, kAfterByeMs);
            std::lock_guard guard(m_statusLock);
            m_status.lastDisconnect = std::format(L"{} was turned away: {}", hello.name, hello.refusal);
            return false;
        }

        m_sessionId = protocol::NewSession(m_rng);
        m_welcomed = true;
        m_lastAudioTick = 0;
        m_arbiter.Force(Source::Usb); // newest connection wins; Wi-Fi or Bluetooth hangs up on its own
        m_jitter.Reset();
        SendLine(protocol::Welcome(m_pcName, m_sessionId, 0)); // no UDP port: audio comes on this connection

        std::lock_guard guard(m_statusLock);
        m_status.connected = true;
        m_status.deviceName = hello.name;
        m_status.muted = false;
        m_status.rttMs = -1;
        m_status.packets = 0;
        m_status.lastDisconnect.clear();
        return true;
    }

    void UsbReceiver::HandleMessage(std::string const& line)
    {
        const protocol::Message message = protocol::ParseMessage(line);
        switch (message.type)
        {
        case protocol::Message::Type::Ping:
        {
            SendLine(protocol::Pong(message.pingId));
            std::lock_guard guard(m_statusLock);
            m_status.rttMs = message.rttMs;
            break;
        }
        case protocol::Message::Type::Mute:
        {
            std::lock_guard guard(m_statusLock);
            m_status.muted = message.muted;
            break;
        }
        case protocol::Message::Type::Bye:
            // It moved to Wi-Fi or Bluetooth and stopped listening; keep trying at the usual
            // pace so moving back to USB is instant too.
            EndSession(L"The iPhone disconnected.", false, kRetryMs);
            break;
        default:
            break;
        }
    }

    void UsbReceiver::HandleAudio(protocol::Audio const& audio)
    {
        if (audio.session != m_sessionId || !m_arbiter.Owns(Source::Usb))
        {
            return;
        }
        protocol::RaisePeak(m_peak, audio.peak);
        m_jitter.Push(audio.sequence, audio.samples);
        m_lastAudioTick = Now();
        std::lock_guard guard(m_statusLock);
        ++m_status.packets;
    }

    void UsbReceiver::CheckSession()
    {
        if (m_session == INVALID_SOCKET)
        {
            return;
        }
        const uint64_t now = Now();
        if (!m_welcomed)
        {
            if (now - m_openedAt > kHelloTimeoutMs)
            {
                EndSession(L"", false, kAfterByeMs);
            }
            return;
        }
        if (now - m_lastRx > kClientTimeoutMs)
        {
            EndSession(L"Lost contact with the iPhone.", true, kRetryMs);
        }
        else if (!m_arbiter.Owns(Source::Usb))
        {
            // Newest connection wins: Wi-Fi or Bluetooth took over. "switched" tells the iPhone
            // not to wait for this PC again, or the two connections would keep swapping.
            SendLine(protocol::kByeSwitched);
            EndSession(SwitchedToText(m_arbiter.Owner()), false, kAfterByeMs);
        }
    }

    // `reason` is empty for a tunnel that never became a session (no hello).
    void UsbReceiver::EndSession(std::wstring const& reason, bool sendBye, uint64_t retryAfterMs)
    {
        if (m_session == INVALID_SOCKET)
        {
            return;
        }
        const bool wasSession = m_welcomed;
        if (sendBye && wasSession)
        {
            SendLine(protocol::kBye);
        }
        CloseSocket(m_session);
        for (auto& device : m_devices)
        {
            if (device.id == m_sessionDevice)
            {
                device.nextAttempt = Now() + retryAfterMs;
            }
        }
        m_sessionDevice = 0;
        m_welcomed = false;
        m_rx.clear();
        m_sessionId = 0;
        m_lastAudioTick = 0;
        if (!wasSession)
        {
            return;
        }
        if (m_arbiter.Owns(Source::Usb))
        {
            m_arbiter.Release(Source::Usb);
            m_jitter.Reset();
        }

        std::lock_guard guard(m_statusLock);
        m_status.connected = false;
        m_status.deviceName.clear();
        m_status.muted = false;
        m_status.rttMs = -1;
        if (!reason.empty())
        {
            m_status.lastDisconnect = reason;
        }
    }

    void UsbReceiver::SendLine(std::string const& json)
    {
        net::SendLine(m_session, json);
    }
}
