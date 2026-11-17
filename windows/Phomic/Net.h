#pragma once

// Small Winsock helpers shared by the Wi-Fi server and the USB receiver.
namespace phomic::net
{
    inline void SetNonBlocking(SOCKET s, bool on = true)
    {
        u_long value = on ? 1 : 0;
        ioctlsocket(s, FIONBIO, &value);
    }

    inline void CloseSocket(SOCKET& s)
    {
        if (s != INVALID_SOCKET)
        {
            shutdown(s, SD_BOTH);
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }

    // Sends one control line on a non-blocking socket, waiting briefly while its buffer is full.
    inline void SendLine(SOCKET s, std::string const& json)
    {
        if (s == INVALID_SOCKET)
        {
            return;
        }
        const std::string data = json + "\n";
        const char* p = data.data();
        int left = static_cast<int>(data.size());
        for (int attempt = 0; left > 0 && attempt < 50; ++attempt)
        {
            const int n = send(s, p, left, 0);
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
}
