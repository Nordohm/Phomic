#include "pch.h"
#include "VirtualMic.h"
#include "Settings.h"
#include "CableOutput.h"

#include <devicetopology.h>
#include <shellapi.h>

namespace phomic::virtualmic
{
    namespace
    {
        constexpr wchar_t kOriginalMicName[] = L"CABLE Output";
        constexpr wchar_t kOriginalFeedName[] = L"CABLE Input";
        constexpr wchar_t kExtraEndpointSetting[] = L"VirtualMicExtraEndpoint";

        // PKEY_Device_DeviceDesc: the user-renamable part of an endpoint's name.
        constexpr PROPERTYKEY kDeviceDesc{ { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
        // PKEY_DeviceInterface_FriendlyName: the driver's label, e.g. "VB-Audio Virtual Cable".
        constexpr PROPERTYKEY kInterfaceName{ { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

        // Undocumented but long-stable interface behind the Sound control panel's
        // "Disable"/"Enable" (also used by SoundSwitch, AudioDeviceCmdlets, EarTrumpet...).
        // Only the vtable order matters; we call the last method.
        struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
            virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
        };
        class __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9")) CPolicyConfigClient;

        std::wstring ReadString(IPropertyStore* store, PROPERTYKEY const& key)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            std::wstring result;
            if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            {
                result = value.pwszVal;
            }
            PropVariantClear(&value);
            return result;
        }

        winrt::com_ptr<IMMDeviceEnumerator> Enumerator()
        {
            winrt::com_ptr<IMMDeviceEnumerator> enumerator;
            winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));
            return enumerator;
        }

        // "CABLE In 16 Ch" (driver spelling), "CABLE In 16ch" (older packages)...
        bool IsExtraName(std::wstring const& name)
        {
            std::wstring compact;
            for (wchar_t c : name)
            {
                if (c != L' ')
                {
                    compact += static_cast<wchar_t>(towlower(c));
                }
            }
            return compact.find(L"16ch") != std::wstring::npos;
        }

        // Identifies the VB-CABLE device an endpoint belongs to, from the kernel-streaming
        // filter it connects to: "{2}.\?\root#media#0001#{...}\wave" -> "root#media#0001".
        std::wstring CableOf(IMMDevice* device)
        {
            winrt::com_ptr<IDeviceTopology> topology;
            if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topology.put_void())))
            {
                return {};
            }
            winrt::com_ptr<IConnector> connector;
            if (FAILED(topology->GetConnector(0, connector.put())))
            {
                return {};
            }
            LPWSTR raw = nullptr;
            if (FAILED(connector->GetDeviceIdConnectedTo(&raw)) || !raw)
            {
                return {};
            }
            std::wstring id(raw);
            CoTaskMemFree(raw);
            std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            // The device instance sits between the last '\' before "#{" and that "#{".
            const size_t end = id.find(L"#{");
            if (end == std::wstring::npos)
            {
                return id;
            }
            const size_t slash = id.rfind(L'\\', end);
            const size_t start = (slash == std::wstring::npos) ? 0 : slash + 1;
            return id.substr(start, end - start);
        }

        HRESULT Rename(IMMDeviceEnumerator* enumerator, std::wstring const& id, wchar_t const* name)
        {
            winrt::com_ptr<IMMDevice> device;
            HRESULT hr = enumerator->GetDevice(id.c_str(), device.put());
            if (FAILED(hr))
            {
                return hr;
            }
            winrt::com_ptr<IPropertyStore> reader;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, reader.put())) && ReadString(reader.get(), kDeviceDesc) == name)
            {
                return S_OK; // already named
            }
            winrt::com_ptr<IPropertyStore> store;
            hr = device->OpenPropertyStore(STGM_READWRITE, store.put());
            if (FAILED(hr))
            {
                return hr;
            }
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_LPWSTR;
            value.pwszVal = const_cast<LPWSTR>(name); // not owned: never PropVariantClear'd
            hr = store->SetValue(kDeviceDesc, value);
            if (SUCCEEDED(hr))
            {
                hr = store->Commit();
            }
            return hr;
        }

        HRESULT SetEnabled(std::wstring const& id, bool enabled)
        {
            winrt::com_ptr<IPolicyConfig> policy;
            HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), policy.put_void());
            if (FAILED(hr))
            {
                return hr;
            }
            return policy->SetEndpointVisibility(id.c_str(), enabled ? TRUE : FALSE);
        }
    }

    std::optional<Endpoints> Find()
    {
        auto enumerator = Enumerator();
        const std::wstring savedExtra = settings::GetString(kExtraEndpointSetting);

        struct Entry
        {
            std::wstring id;
            std::wstring name;
            std::wstring cable; // which VB-CABLE device it belongs to ("" if unknown)
            bool active = false;
            bool capture = false;
        };
        std::vector<Entry> entries;

        const EDataFlow flows[] = { eRender, eCapture };
        for (EDataFlow flow : flows)
        {
            winrt::com_ptr<IMMDeviceCollection> collection;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED, collection.put())))
            {
                continue;
            }
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                winrt::com_ptr<IMMDevice> device;
                winrt::com_ptr<IPropertyStore> store;
                if (FAILED(collection->Item(i, device.put())) || FAILED(device->OpenPropertyStore(STGM_READ, store.put())))
                {
                    continue;
                }
                if (ReadString(store.get(), kInterfaceName) != kDriverLabel)
                {
                    continue;
                }
                LPWSTR rawId = nullptr;
                if (FAILED(device->GetId(&rawId)) || !rawId)
                {
                    continue;
                }
                Entry entry;
                entry.id = rawId;
                CoTaskMemFree(rawId);
                DWORD state = 0;
                device->GetState(&state);
                entry.active = state == DEVICE_STATE_ACTIVE;
                entry.name = ReadString(store.get(), kDeviceDesc);
                entry.cable = CableOf(device.get());
                entry.capture = flow == eCapture;
                entries.push_back(std::move(entry));
            }
        }
        if (entries.empty())
        {
            return std::nullopt;
        }

        // If VB-CABLE got installed twice there are two cables. Use one whose recording
        // and playback ends belong together, preferring the one already named "Phomic".
        std::wstring cable;
        int bestScore = -1;
        for (auto const& mic : entries)
        {
            if (!mic.capture)
            {
                continue;
            }
            const bool hasFeed = std::any_of(entries.begin(), entries.end(), [&](Entry const& e)
            {
                return !e.capture && e.cable == mic.cable && !IsExtraName(e.name);
            });
            const int score = (hasFeed ? 4 : 0) + (mic.name == kMicName ? 2 : 0) + (mic.active ? 1 : 0);
            if (score > bestScore)
            {
                bestScore = score;
                cable = mic.cable;
            }
        }

        Endpoints endpoints;
        for (auto const& entry : entries)
        {
            // Disabled endpoints can't report their cable ("" key); accept them for any cable.
            const bool sameCable = entry.cable == cable || entry.cable.empty() || cable.empty();
            if (!sameCable)
            {
                continue;
            }
            if (entry.capture)
            {
                if (endpoints.micId.empty() || (entry.active && entry.cable == cable))
                {
                    endpoints.micId = entry.id;
                    endpoints.micName = entry.name;
                }
            }
            else if (IsExtraName(entry.name) || entry.id == savedExtra)
            {
                if (endpoints.extraId.empty() || entry.cable == cable)
                {
                    endpoints.extraId = entry.id;
                    endpoints.extraActive = entry.active;
                }
            }
            else if (endpoints.feedId.empty() || (entry.active && entry.cable == cable))
            {
                endpoints.feedId = entry.id;
                endpoints.feedName = entry.name;
                endpoints.feedActive = entry.active;
            }
        }
        return endpoints;
    }

    State GetState(std::optional<Endpoints> const& endpoints)
    {
        if (!endpoints || (endpoints->micId.empty() && endpoints->feedId.empty()))
        {
            return State::NotInstalled;
        }
        const bool named = endpoints->micName == kMicName && endpoints->feedName == kFeedName;
        const bool tidy = endpoints->extraId.empty() || !endpoints->extraActive;
        // When Phomic can stream straight into the driver, the playback end isn't needed either.
        const bool feedHidden = !endpoints->feedActive || !CableOutput::IsAvailable();
        return named && tidy && feedHidden ? State::Ready : State::NeedsSetup;
    }

    HRESULT Apply()
    {
        try
        {
            const auto endpoints = Find();
            if (!endpoints || endpoints->micId.empty() || endpoints->feedId.empty())
            {
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
            auto enumerator = Enumerator();
            HRESULT hr = Rename(enumerator.get(), endpoints->micId, kMicName);
            if (SUCCEEDED(hr))
            {
                hr = Rename(enumerator.get(), endpoints->feedId, kFeedName);
            }
            if (SUCCEEDED(hr) && !endpoints->extraId.empty())
            {
                // Remember it: once disabled and renamed by someone, "16ch" may be gone from its name.
                settings::SetString(kExtraEndpointSetting, endpoints->extraId);
                if (endpoints->extraActive)
                {
                    hr = SetEnabled(endpoints->extraId, false);
                }
            }
            if (SUCCEEDED(hr) && endpoints->feedActive && CableOutput::IsAvailable())
            {
                // Phomic writes straight into the driver, so hide the playback end too.
                hr = SetEnabled(endpoints->feedId, false);
            }
            return hr;
        }
        catch (winrt::hresult_error const& e)
        {
            return e.code();
        }
    }

    HRESULT Restore()
    {
        try
        {
            const auto endpoints = Find();
            if (!endpoints)
            {
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
            auto enumerator = Enumerator();
            HRESULT hr = S_OK;
            if (!endpoints->micId.empty())
            {
                hr = Rename(enumerator.get(), endpoints->micId, kOriginalMicName);
            }
            if (SUCCEEDED(hr) && !endpoints->feedId.empty())
            {
                hr = Rename(enumerator.get(), endpoints->feedId, kOriginalFeedName);
            }
            if (SUCCEEDED(hr) && !endpoints->extraId.empty() && !endpoints->extraActive)
            {
                hr = SetEnabled(endpoints->extraId, true);
            }
            if (SUCCEEDED(hr) && !endpoints->feedId.empty() && !endpoints->feedActive)
            {
                hr = SetEnabled(endpoints->feedId, true);
            }
            return hr;
        }
        catch (winrt::hresult_error const& e)
        {
            return e.code();
        }
    }

    std::wstring OwnExePath()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    HRESULT RunElevatedAndWait(std::wstring const& exe, std::wstring const& arguments, DWORD* exitCode)
    {
        std::wstring directory = exe.substr(0, exe.find_last_of(L'\\'));
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = exe.c_str();
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.lpDirectory = directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        DWORD code = 0;
        if (info.hProcess)
        {
            WaitForSingleObject(info.hProcess, INFINITE);
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
        }
        if (exitCode)
        {
            *exitCode = code;
        }
        return S_OK;
    }
}
