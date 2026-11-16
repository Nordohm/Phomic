#include "pch.h"
#include "CableOutput.h"
#include "Protocol.h"

#include <cfgmgr32.h>
#include <ks.h>
#include <ksmedia.h>

namespace phomic
{
    namespace
    {
        constexpr uint32_t kChannels = 2;
        constexpr uint32_t kBytesPerFrame = kChannels * sizeof(int16_t);
        constexpr uint32_t kRequestedBufferBytes = kSampleRate * kBytesPerFrame / 10; // 100 ms ring
        constexpr uint32_t kLeadFrames = kSampleRate * 20 / 1000;                    // write 20 ms ahead
        constexpr LONG kTickMs = 2;

        bool Ioctl(HANDLE handle, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD returned = 0;
            BOOL ok = DeviceIoControl(handle, code, in, inSize, out, outSize, &returned, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                ok = GetOverlappedResult(handle, &overlapped, &returned, TRUE);
            }
            CloseHandle(overlapped.hEvent);
            return ok != FALSE;
        }

        // VB-CABLE's playback-side wave filter, e.g.
        // \\?\root#media#0001#{65e8773e-...}\vbaudiovacwdm2022_out1_wave_...
        std::wstring FindCableFilter()
        {
            GUID category = KSCATEGORY_RENDER;
            ULONG length = 0;
            if (CM_Get_Device_Interface_List_SizeW(&length, &category, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2)
            {
                return {};
            }
            std::vector<wchar_t> list(length + 1, L'\0');
            if (CM_Get_Device_Interface_ListW(&category, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
            {
                return {};
            }
            for (const wchar_t* path = list.data(); *path; path += wcslen(path) + 1)
            {
                std::wstring lower(path);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                if (lower.find(L"vbaudiovacwdm") != std::wstring::npos && lower.find(L"_out1_wave") != std::wstring::npos)
                {
                    return path;
                }
            }
            return {};
        }

        HANDLE OpenFilter(std::wstring const& path)
        {
            return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }

        bool PinSupportsLoopedStreaming(HANDLE filter)
        {
            KSP_PIN query{};
            query.Property.Set = KSPROPSETID_Pin;
            query.Property.Id = KSPROPERTY_PIN_INTERFACES;
            query.Property.Flags = KSPROPERTY_TYPE_GET;
            query.PinId = 0;
            alignas(8) BYTE buffer[512];
            if (!Ioctl(filter, IOCTL_KS_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer)))
            {
                return false;
            }
            auto const* items = reinterpret_cast<KSMULTIPLE_ITEM const*>(buffer);
            auto const* ids = reinterpret_cast<KSIDENTIFIER const*>(items + 1);
            const ULONG count = std::min<ULONG>(items->Count, (sizeof(buffer) - sizeof(KSMULTIPLE_ITEM)) / sizeof(KSIDENTIFIER));
            for (ULONG i = 0; i < count; ++i)
            {
                if (IsEqualGUID(ids[i].Set, KSINTERFACESETID_Standard) && ids[i].Id == KSINTERFACE_STANDARD_LOOPED_STREAMING)
                {
                    return true;
                }
            }
            return false;
        }

        bool SetState(HANDLE pin, KSSTATE state)
        {
            KSPROPERTY property{};
            property.Set = KSPROPSETID_Connection;
            property.Id = KSPROPERTY_CONNECTION_STATE;
            property.Flags = KSPROPERTY_TYPE_SET;
            return Ioctl(pin, IOCTL_KS_PROPERTY, &property, sizeof(property), &state, sizeof(state));
        }

        bool PlayPosition(HANDLE pin, uint64_t& offset)
        {
            KSPROPERTY property{};
            property.Set = KSPROPSETID_Audio;
            property.Id = KSPROPERTY_AUDIO_POSITION;
            property.Flags = KSPROPERTY_TYPE_GET;
            KSAUDIO_POSITION position{};
            if (!Ioctl(pin, IOCTL_KS_PROPERTY, &property, sizeof(property), &position, sizeof(position)))
            {
                return false;
            }
            offset = position.PlayOffset;
            return true;
        }
    }

    bool CableOutput::IsAvailable()
    {
        const std::wstring path = FindCableFilter();
        if (path.empty())
        {
            return false;
        }
        HANDLE filter = OpenFilter(path);
        if (filter == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const bool looped = PinSupportsLoopedStreaming(filter);
        CloseHandle(filter);
        return looped;
    }

    CableOutput::CableOutput(Source source) : m_source(std::move(source))
    {
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    CableOutput::~CableOutput()
    {
        Stop();
        if (m_wake)
        {
            CloseHandle(m_wake);
        }
    }

    bool CableOutput::Start()
    {
        Stop();
        m_stop = false;
        m_failed = false;
        {
            std::lock_guard guard(m_errorLock);
            m_error.clear();
        }
        ResetEvent(m_wake);

        std::promise<HRESULT> ready;
        auto started = ready.get_future();
        m_thread = std::thread(&CableOutput::Run, this, std::move(ready));
        if (FAILED(started.get()))
        {
            m_thread.join();
            return false;
        }
        return true;
    }

    void CableOutput::Stop()
    {
        if (m_thread.joinable())
        {
            m_stop = true;
            SetEvent(m_wake);
            m_thread.join();
        }
        m_running = false;
    }

    std::wstring CableOutput::LastError()
    {
        std::lock_guard guard(m_errorLock);
        return m_error;
    }

    void CableOutput::SetError(HRESULT hr, std::wstring_view what)
    {
        std::lock_guard guard(m_errorLock);
        m_error = std::format(L"{} (0x{:08X})", what, static_cast<uint32_t>(hr));
    }

    void CableOutput::Run(std::promise<HRESULT> ready)
    {
        bool signaled = false;
        const HRESULT hr = StreamLoop(ready, signaled);
        if (!signaled)
        {
            ready.set_value(FAILED(hr) ? hr : E_FAIL);
        }
        else if (FAILED(hr))
        {
            m_failed = true;
        }
        m_running = false;
    }

    HRESULT CableOutput::StreamLoop(std::promise<HRESULT>& ready, bool& signaled)
    {
        const std::wstring path = FindCableFilter();
        if (path.empty())
        {
            SetError(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"VB-CABLE isn't installed");
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        HANDLE filter = OpenFilter(path);
        if (filter == INVALID_HANDLE_VALUE)
        {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            SetError(hr, L"Couldn't open the VB-CABLE driver");
            return hr;
        }

        // Connect to the cable's input pin: 48 kHz, stereo, 16-bit PCM, WaveRT (looped) streaming.
        struct
        {
            KSPIN_CONNECT connect;
            KSDATAFORMAT_WAVEFORMATEX format;
        } request{};
        request.connect.Interface.Set = KSINTERFACESETID_Standard;
        request.connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
        request.connect.Medium.Set = KSMEDIUMSETID_Standard;
        request.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        request.connect.PinId = 0;
        request.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
        request.connect.Priority.PrioritySubClass = 1;
        request.format.DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        request.format.DataFormat.SampleSize = kBytesPerFrame;
        request.format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        request.format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        request.format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        request.format.WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
        request.format.WaveFormatEx.nChannels = kChannels;
        request.format.WaveFormatEx.nSamplesPerSec = kSampleRate;
        request.format.WaveFormatEx.wBitsPerSample = 16;
        request.format.WaveFormatEx.nBlockAlign = kBytesPerFrame;
        request.format.WaveFormatEx.nAvgBytesPerSec = kSampleRate * kBytesPerFrame;

        HANDLE pin = nullptr;
        const DWORD created = KsCreatePin(filter, &request.connect, GENERIC_READ | GENERIC_WRITE, &pin);
        if (created != ERROR_SUCCESS)
        {
            CloseHandle(filter);
            SetError(HRESULT_FROM_WIN32(created), L"VB-CABLE refused the connection");
            return HRESULT_FROM_WIN32(created);
        }

        // The driver maps its cyclic buffer into this process.
        KSRTAUDIO_BUFFER_PROPERTY bufferRequest{};
        bufferRequest.Property.Set = KSPROPSETID_RtAudio;
        bufferRequest.Property.Id = KSPROPERTY_RTAUDIO_BUFFER;
        bufferRequest.Property.Flags = KSPROPERTY_TYPE_GET;
        bufferRequest.BaseAddress = nullptr;
        bufferRequest.RequestedBufferSize = kRequestedBufferBytes;
        KSRTAUDIO_BUFFER buffer{};
        if (!Ioctl(pin, IOCTL_KS_PROPERTY, &bufferRequest, sizeof(bufferRequest), &buffer, sizeof(buffer)) ||
            !buffer.BufferAddress || buffer.ActualBufferSize < kBytesPerFrame * kLeadFrames * 2)
        {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(pin);
            CloseHandle(filter);
            SetError(hr, L"VB-CABLE didn't provide an audio buffer");
            return FAILED(hr) ? hr : E_FAIL;
        }
        auto* ring = static_cast<BYTE*>(buffer.BufferAddress);
        const uint32_t ringFrames = buffer.ActualBufferSize / kBytesPerFrame;
        std::memset(ring, 0, static_cast<size_t>(ringFrames) * kBytesPerFrame);

        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer)
        {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due{};
        due.QuadPart = -10000LL * kTickMs;
        SetWaitableTimer(timer, &due, kTickMs, nullptr, nullptr, FALSE);

        SetState(pin, KSSTATE_ACQUIRE);
        SetState(pin, KSSTATE_PAUSE);
        uint64_t playOffset = 0;
        if (!SetState(pin, KSSTATE_RUN) || !PlayPosition(pin, playOffset))
        {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            SetState(pin, KSSTATE_STOP);
            CloseHandle(timer);
            CloseHandle(pin);
            CloseHandle(filter);
            SetError(hr, L"Couldn't start streaming into VB-CABLE");
            return FAILED(hr) ? hr : E_FAIL;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        m_running = true;
        ready.set_value(S_OK);
        signaled = true;

        std::vector<float> mono(ringFrames);
        uint32_t writeFrame = static_cast<uint32_t>((playOffset / kBytesPerFrame + kLeadFrames) % ringFrames);
        HRESULT hr = S_OK;
        HANDLE waits[2] = { timer, m_wake };
        while (!m_stop)
        {
            WaitForMultipleObjects(2, waits, FALSE, 50);
            if (m_stop)
            {
                break;
            }
            if (!PlayPosition(pin, playOffset))
            {
                hr = HRESULT_FROM_WIN32(GetLastError());
                SetError(FAILED(hr) ? hr : E_FAIL, L"VB-CABLE stopped responding");
                hr = FAILED(hr) ? hr : E_FAIL;
                break;
            }
            // Fill from where we are up to 20 ms ahead of what the driver is playing.
            const uint32_t playFrame = static_cast<uint32_t>((playOffset / kBytesPerFrame) % ringFrames);
            const uint32_t target = (playFrame + kLeadFrames) % ringFrames;
            uint32_t frames = (target + ringFrames - writeFrame) % ringFrames;
            if (frames > ringFrames / 2)
            {
                // The driver overtook us (this thread was starved): start over just behind its position.
                writeFrame = playFrame;
                frames = kLeadFrames;
            }
            if (frames == 0)
            {
                continue; // nothing played since the last tick
            }
            m_source(mono.data(), frames);
            for (uint32_t i = 0; i < frames; ++i)
            {
                const int16_t sample = static_cast<int16_t>(std::lrint(std::clamp(mono[i], -1.f, 1.f) * 32767.f));
                auto* frame = reinterpret_cast<int16_t*>(ring + static_cast<size_t>(writeFrame) * kBytesPerFrame);
                frame[0] = sample;
                frame[1] = sample;
                writeFrame = (writeFrame + 1) % ringFrames;
            }
        }

        SetState(pin, KSSTATE_PAUSE);
        SetState(pin, KSSTATE_ACQUIRE);
        SetState(pin, KSSTATE_STOP);
        if (mmcss)
        {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        CancelWaitableTimer(timer);
        CloseHandle(timer);
        CloseHandle(pin); // also unmaps the ring buffer
        CloseHandle(filter);
        return hr;
    }
}
