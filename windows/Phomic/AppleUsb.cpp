#include "pch.h"
#include "AppleUsb.h"

#include <appmodel.h>
#include <cfgmgr32.h>
#include <shellapi.h>

namespace phomic::appleusb
{
    namespace
    {
        constexpr wchar_t kAppleDevicesFamily[] = L"AppleInc.AppleDevices_nzyj5cx40ttqa";
        constexpr wchar_t kAppleDevicesStoreId[] = L"9NP83LWLPZ9K";

        std::wstring Lower(std::wstring s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return s;
        }

        std::wstring ProcessName(DWORD pid)
        {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return {};
            }
            wchar_t path[MAX_PATH] = {};
            DWORD size = ARRAYSIZE(path);
            std::wstring name;
            if (QueryFullProcessImageNameW(process, 0, path, &size))
            {
                name = path;
                const size_t slash = name.find_last_of(L"\\/");
                if (slash != std::wstring::npos)
                {
                    name.erase(0, slash + 1);
                }
            }
            CloseHandle(process);
            return name;
        }

        // A portproxy rule forwarding `port` from an address that also covers 127.0.0.1.
        std::wstring PortForwardOn(uint16_t port)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PortProxy\\v4tov4\\tcp",
                    0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return {};
            }
            std::wstring found;
            const std::wstring suffix = std::format(L"/{}", port);
            for (DWORD i = 0;; ++i)
            {
                wchar_t name[256] = {};
                DWORD length = ARRAYSIZE(name);
                if (RegEnumValueW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                {
                    break;
                }
                // Value names look like "0.0.0.0/27015" (listen address / port).
                const std::wstring entry = name;
                if (entry.size() > suffix.size() && entry.ends_with(suffix))
                {
                    const std::wstring address = entry.substr(0, entry.size() - suffix.size());
                    if (address == L"0.0.0.0" || address == L"*" || address == L"127.0.0.1" || Lower(address) == L"localhost")
                    {
                        found = address;
                        break;
                    }
                }
            }
            RegCloseKey(key);
            return found;
        }

        void Open(wchar_t const* target)
        {
            ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    bool AppleDevicesInstalled()
    {
        UINT32 count = 0;
        UINT32 length = 0;
        const LONG result = GetPackagesByPackageFamily(kAppleDevicesFamily, &count, nullptr, &length, nullptr);
        return (result == ERROR_SUCCESS || result == ERROR_INSUFFICIENT_BUFFER) && count > 0;
    }

    Startup AppleDevicesStartup()
    {
        // Windows keeps the state of a packaged app's startup task here once it has one:
        // 0 disabled, 1 turned off by the user, 2 enabled, 3 disabled by policy, 4 enabled by policy.
        DWORD state = 0;
        DWORD size = sizeof(state);
        const LONG result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\SystemAppData\\"
            L"AppleInc.AppleDevices_nzyj5cx40ttqa\\AppleInc.AppleDevices.AppleMobileDeviceLauncher",
            L"State", RRF_RT_REG_DWORD, nullptr, &state, &size);
        if (result != ERROR_SUCCESS)
        {
            return Startup::Unknown;
        }
        return state == 2 || state == 4 ? Startup::Enabled : Startup::Disabled;
    }

    ClassicService ClassicServiceState()
    {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
        {
            return ClassicService::Missing;
        }
        ClassicService state = ClassicService::Missing;
        if (SC_HANDLE service = OpenServiceW(manager, L"Apple Mobile Device Service", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG))
        {
            SERVICE_STATUS status{};
            state = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING
                ? ClassicService::Running : ClassicService::Stopped;
            if (state == ClassicService::Stopped)
            {
                DWORD needed = 0;
                QueryServiceConfigW(service, nullptr, 0, &needed);
                std::vector<uint8_t> buffer(needed);
                auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
                if (needed && QueryServiceConfigW(service, config, needed, &needed) && config->dwStartType == SERVICE_DISABLED)
                {
                    state = ClassicService::Disabled;
                }
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        return state;
    }

    PortHolder WhoHoldsPort(uint16_t port)
    {
        PortHolder holder;
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        std::vector<uint8_t> buffer(size);
        auto table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        if (size == 0 || GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        {
            return holder;
        }
        const DWORD loopback = htonl(INADDR_LOOPBACK);
        for (DWORD i = 0; i < table->dwNumEntries; ++i)
        {
            auto const& row = table->table[i];
            if (ntohs(static_cast<uint16_t>(row.dwLocalPort)) != port ||
                (row.dwLocalAddr != INADDR_ANY && row.dwLocalAddr != loopback))
            {
                continue;
            }
            holder.held = true;
            holder.process = ProcessName(row.dwOwningPid);
            const std::wstring name = Lower(holder.process);
            holder.apple = name.starts_with(L"applemobiledevice");
            if (holder.apple)
            {
                break; // Apple's service is there: that's the one that counts
            }
        }
        if (holder.held && !holder.apple)
        {
            holder.forwardAddress = PortForwardOn(port);
        }
        return holder;
    }

    uint32_t AppleDevicesPluggedIn()
    {
        // Instance ids of present USB devices, e.g. "USB\VID_05AC&PID_12A8\<serial>". iPhones and
        // iPads use product ids 0x1290-0x12AF; their interfaces ("&MI_00") are counted once.
        ULONG length = 0;
        if (CM_Get_Device_ID_List_SizeW(&length, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS ||
            length == 0)
        {
            return 0;
        }
        std::vector<wchar_t> ids(length);
        if (CM_Get_Device_ID_ListW(L"USB", ids.data(), length, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        {
            return 0;
        }
        uint32_t count = 0;
        for (const wchar_t* id = ids.data(); *id; id += wcslen(id) + 1)
        {
            const std::wstring upper = [&]
            {
                std::wstring s = id;
                std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
                return s;
            }();
            if (upper.starts_with(L"USB\\VID_05AC&PID_12") && upper.find(L"&MI_") == std::wstring::npos)
            {
                const unsigned long product = std::wcstoul(upper.c_str() + 17, nullptr, 16);
                if (product >= 0x1290 && product <= 0x12AF)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    bool StartAppleService()
    {
        // AMPDevicesAgent.exe's class "AMP.Devices.IDiscovery", from Apple Devices' AppxManifest.xml.
        constexpr GUID kAgent{ 0x054AA48D, 0x7681, 0x4C3C, { 0x95, 0x54, 0xAE, 0x09, 0x3D, 0x87, 0x98, 0xDB } };
        IUnknown* agent = nullptr;
        const HRESULT hr = CoCreateInstance(kAgent, nullptr, CLSCTX_LOCAL_SERVER, IID_IUnknown, reinterpret_cast<void**>(&agent));
        if (agent)
        {
            agent->Release(); // the service keeps running without it
        }
        return SUCCEEDED(hr);
    }

    void OpenAppleDevices()
    {
        Open(L"shell:AppsFolder\\AppleInc.AppleDevices_nzyj5cx40ttqa!App");
    }

    void OpenStorePage()
    {
        // The Microsoft Store app; its web page if the Store app can't open.
        const std::wstring store = std::format(L"ms-windows-store://pdp/?ProductId={}", kAppleDevicesStoreId);
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", store.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
        {
            Open(std::format(L"https://apps.microsoft.com/detail/{}", kAppleDevicesStoreId).c_str());
        }
    }
}
