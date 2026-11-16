#include "pch.h"
#include "AudioOutput.h"
#include "Protocol.h"

namespace phomic
{
    namespace
    {
        // PKEY_Device_FriendlyName, spelled out to avoid the INITGUID dance.
        constexpr PROPERTYKEY kDeviceFriendlyName{
            { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

        constexpr REFERENCE_TIME kBufferDuration = 20 * 10000; // 20 ms in 100 ns units

        std::wstring ToLower(std::wstring_view s)
        {
            std::wstring out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return out;
        }

        std::wstring DeviceIdOf(IMMDevice* device)
        {
            LPWSTR raw = nullptr;
            if (FAILED(device->GetId(&raw)) || !raw)
            {
                return {};
            }
            std::wstring id(raw);
            CoTaskMemFree(raw);
            return id;
        }

        std::wstring FriendlyNameOf(IMMDevice* device)
        {
            winrt::com_ptr<IPropertyStore> props;
            if (FAILED(device->OpenPropertyStore(STGM_READ, props.put())))
            {
                return L"Unknown device";
            }
            PROPVARIANT value;
            PropVariantInit(&value);
            std::wstring name = L"Unknown device";
            if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            {
                name = value.pwszVal;
            }
            PropVariantClear(&value);
            return name;
        }
    }

    std::vector<AudioDevice> EnumerateRenderDevices()
    {
        auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));

        std::wstring defaultId;
        winrt::com_ptr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultDevice.put())))
        {
            defaultId = DeviceIdOf(defaultDevice.get());
        }

        winrt::com_ptr<IMMDeviceCollection> collection;
        winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()));
        UINT count = 0;
        winrt::check_hresult(collection->GetCount(&count));

        std::vector<AudioDevice> devices;
        for (UINT i = 0; i < count; ++i)
        {
            winrt::com_ptr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put())))
            {
                continue;
            }
            AudioDevice info;
            info.id = DeviceIdOf(device.get());
            info.name = FriendlyNameOf(device.get());
            info.isDefault = !defaultId.empty() && info.id == defaultId;
            if (!info.id.empty())
            {
                devices.push_back(std::move(info));
            }
        }
        std::sort(devices.begin(), devices.end(), [](AudioDevice const& a, AudioDevice const& b) { return a.name < b.name; });
        return devices;
    }

    bool IsVirtualCable(std::wstring_view name)
    {
        const std::wstring lower = ToLower(name);
        for (auto needle : { L"cable", L"vb-audio", L"voicemeeter" })
        {
            if (lower.find(needle) != std::wstring::npos)
            {
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- DeviceWatcher

    struct DeviceWatcher::Client : winrt::implements<Client, IMMNotificationClient>
    {
        explicit Client(std::shared_ptr<std::atomic<bool>> flag) : m_flag(std::move(flag)) {}

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) noexcept override { return Raise(); }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override { return Raise(); }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) noexcept override { return Raise(); }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) noexcept override
        {
            return flow == eRender ? Raise() : S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) noexcept override
        {
            return (key.fmtid == kDeviceFriendlyName.fmtid && key.pid == kDeviceFriendlyName.pid) ? Raise() : S_OK;
        }

    private:
        HRESULT Raise() noexcept
        {
            m_flag->store(true);
            return S_OK;
        }
        std::shared_ptr<std::atomic<bool>> m_flag;
    };

    DeviceWatcher::DeviceWatcher() : m_changed(std::make_shared<std::atomic<bool>>(false))
    {
        try
        {
            m_enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
            m_client = winrt::make_self<Client>(m_changed).as<IMMNotificationClient>();
            if (FAILED(m_enumerator->RegisterEndpointNotificationCallback(m_client.get())))
            {
                m_client = nullptr;
            }
        }
        catch (...)
        {
            m_client = nullptr;
        }
    }

    DeviceWatcher::~DeviceWatcher()
    {
        if (m_enumerator && m_client)
        {
            m_enumerator->UnregisterEndpointNotificationCallback(m_client.get());
        }
    }

    // ---------------------------------------------------------------- AudioOutput

    AudioOutput::AudioOutput(Source source) : m_source(std::move(source))
    {
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    AudioOutput::~AudioOutput()
    {
        Stop();
        if (m_wake)
        {
            CloseHandle(m_wake);
        }
    }

    bool AudioOutput::Start(std::wstring const& deviceId)
    {
        Stop();
        m_stop = false;
        m_failed = false;
        {
            std::lock_guard guard(m_errorLock);
            m_error.clear();
        }
        m_deviceId = deviceId;
        ResetEvent(m_wake);

        std::promise<HRESULT> ready;
        auto started = ready.get_future();
        m_thread = std::thread(&AudioOutput::Run, this, deviceId, std::move(ready));
        if (FAILED(started.get()))
        {
            m_thread.join();
            return false;
        }
        return true;
    }

    void AudioOutput::Stop()
    {
        if (m_thread.joinable())
        {
            m_stop = true;
            SetEvent(m_wake);
            m_thread.join();
        }
        m_running = false;
    }

    std::wstring AudioOutput::LastError()
    {
        std::lock_guard guard(m_errorLock);
        return m_error;
    }

    void AudioOutput::SetError(HRESULT hr, std::wstring_view what)
    {
        std::lock_guard guard(m_errorLock);
        m_error = std::format(L"{} (0x{:08X})", what, static_cast<uint32_t>(hr));
    }

    void AudioOutput::Run(std::wstring deviceId, std::promise<HRESULT> ready)
    {
        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool signaled = false;
        HRESULT hr = RenderLoop(deviceId, ready, signaled);
        if (!signaled)
        {
            ready.set_value(FAILED(hr) ? hr : E_FAIL);
        }
        else if (FAILED(hr))
        {
            m_failed = true;
        }
        m_running = false;
        if (SUCCEEDED(coInit))
        {
            CoUninitialize();
        }
    }

    HRESULT AudioOutput::RenderLoop(std::wstring const& deviceId, std::promise<HRESULT>& ready, bool& signaled)
    {
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()));
        if (FAILED(hr))
        {
            SetError(hr, L"Couldn't access Windows audio");
            return hr;
        }

        winrt::com_ptr<IMMDevice> device;
        hr = deviceId.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put())
                              : enumerator->GetDevice(deviceId.c_str(), device.put());
        if (FAILED(hr))
        {
            SetError(hr, L"The selected audio device isn't available");
            return hr;
        }

        winrt::com_ptr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
        if (FAILED(hr))
        {
            SetError(hr, L"Couldn't open the audio device");
            return hr;
        }

        // Stereo float at the stream rate; AUTOCONVERTPCM lets the audio engine
        // resample / remap to whatever the endpoint actually runs at.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        format.cbSize = 0;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            kBufferDuration, 0, &format, nullptr);
        if (FAILED(hr))
        {
            SetError(hr, L"The audio device refused the stream format");
            return hr;
        }

        HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audioEvent)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            SetError(hr, L"Couldn't create an audio event");
            return hr;
        }

        UINT32 bufferFrames = 0;
        winrt::com_ptr<IAudioRenderClient> render;
        hr = client->SetEventHandle(audioEvent);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(render.put()));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr))
        {
            CloseHandle(audioEvent);
            SetError(hr, L"Couldn't start the audio device");
            return hr;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        m_running = true;
        ready.set_value(S_OK);
        signaled = true;

        std::vector<float> mono(bufferFrames);
        HANDLE waits[2] = { audioEvent, m_wake };
        while (!m_stop)
        {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 500);
            if (m_stop)
            {
                break;
            }
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
            {
                continue;
            }

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr))
            {
                SetError(hr, L"The audio device was disconnected");
                break;
            }
            const UINT32 available = bufferFrames - padding;
            if (available == 0)
            {
                continue;
            }

            BYTE* data = nullptr;
            hr = render->GetBuffer(available, &data);
            if (FAILED(hr))
            {
                SetError(hr, L"The audio device was disconnected");
                break;
            }

            m_source(mono.data(), available);
            float* out = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < available; ++i)
            {
                out[2 * i] = mono[i];
                out[2 * i + 1] = mono[i];
            }
            render->ReleaseBuffer(available, 0);
            hr = S_OK;
        }

        client->Stop();
        if (mmcss)
        {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        CloseHandle(audioEvent);
        return hr;
    }
}
