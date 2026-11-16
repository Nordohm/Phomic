#include "pch.h"
#include "BluetoothReceiver.h"
#include "Adpcm.h"
#include "JitterBuffer.h"
#include "JsonUtil.h"
#include "Pairing.h"

#include <opus.h>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage::Streams;

namespace phomic
{
    namespace
    {
        // Must match BluetoothLink.swift on the iPhone.
        constexpr guid kAudioUuid{ 0x8e5a1c51, 0x3b7d, 0x4f62, { 0x9a, 0x1e, 0x6c, 0x2d, 0x4b, 0x8f, 0x0a, 0x11 } };
        constexpr guid kControlUuid{ 0x8e5a1c52, 0x3b7d, 0x4f62, { 0x9a, 0x1e, 0x6c, 0x2d, 0x4b, 0x8f, 0x0a, 0x11 } };

        // 8E5A1C6<c>-3B7D-4F62-9A1E-6C2D<PC id>, with c = 1 if a pairing code is required.
        // The advertisement is all an iPhone needs to list this PC (with its lock icon) and
        // to find exactly the PC the user picked.
        guid ServiceUuid(bool requiresCode)
        {
            const uint32_t id = pairing::PcIdValue();
            return guid{ 0x8e5a1c60u | (requiresCode ? 1u : 0u), 0x3b7d, 0x4f62,
                { 0x9a, 0x1e, 0x6c, 0x2d,
                  static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16),
                  static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id) } };
        }

        constexpr uint8_t kFormat24k = 0xA1;   // IMA-ADPCM fallback
        constexpr uint8_t kFormat16k = 0xA2;
        constexpr uint8_t kFormatOpus = 0xB1;  // Opus, 48 kHz mono, 10 ms frames
        constexpr int16_t kMaxConcealFrames = 5;
        constexpr size_t kHeaderBytes = 6;

        constexpr uint64_t kRetryMs = 3000;
        // Connected and not muted, but no audio for this long: the link is gone.
        constexpr uint64_t kSilentDropMs = 10000;
        // After "bye", keep the service up briefly so the notification goes out.
        constexpr uint64_t kByeGraceMs = 500;

        uint64_t Now()
        {
            return GetTickCount64();
        }

        IBuffer MakeBuffer(std::string_view text)
        {
            Buffer buffer(static_cast<uint32_t>(text.size()));
            std::memcpy(buffer.data(), text.data(), text.size());
            buffer.Length(static_cast<uint32_t>(text.size()));
            return buffer;
        }

        JsonObject ParseJson(IBuffer const& value)
        {
            if (!value || value.Length() == 0)
            {
                return nullptr;
            }
            std::string_view text(reinterpret_cast<const char*>(value.data()), value.Length());
            JsonObject object{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(text), object))
            {
                return nullptr;
            }
            return object;
        }

        std::wstring ClientIdOf(GattSession const& session)
        {
            return session ? std::wstring(session.DeviceId().Id()) : std::wstring();
        }

        std::wstring ErrorText(BluetoothError error)
        {
            switch (error)
            {
            case BluetoothError::RadioNotAvailable:
                return L"Bluetooth is turned off on this PC.";
            case BluetoothError::DisabledByPolicy:
            case BluetoothError::DisabledByUser:
                return L"Bluetooth is disabled on this PC.";
            case BluetoothError::NotSupported:
                return L"This PC's Bluetooth can't accept connections from iPhones.";
            case BluetoothError::ResourceInUse:
                return L"Bluetooth is busy: another copy of Phomic may be running.";
            default:
                return std::format(L"Bluetooth isn't working (error {}).", static_cast<int>(error));
            }
        }
    }

    BluetoothReceiver::Client::~Client()
    {
        try
        {
            if (parameters)
            {
                parameters.Close();
            }
            if (device)
            {
                device.Close();
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    BluetoothReceiver::BluetoothReceiver(JitterBuffer& jitter, SourceArbiter& arbiter, std::wstring pcName)
        : m_jitter(jitter), m_arbiter(arbiter), m_pcName(std::move(pcName))
    {
        m_wantRequiresCode = pairing::Required();
    }

    BluetoothReceiver::~BluetoothReceiver()
    {
        Shutdown();
        std::lock_guard decodeGuard(m_decodeLock);
        if (m_opus)
        {
            opus_decoder_destroy(m_opus);
            m_opus = nullptr;
        }
    }

    void BluetoothReceiver::SetEnabled(bool enabled)
    {
        {
            std::lock_guard guard(m_lock);
            m_status.enabled = enabled;
            if (!enabled)
            {
                m_status.problem.clear();
                m_status.connecting = false;
            }
        }
        if (!enabled)
        {
            Drop(m_generation.load(), L"Bluetooth was turned off in Phomic.", true);
            m_stopProviderAt = Now() + kByeGraceMs; // Tick unpublishes the service after that
        }
    }

    void BluetoothReceiver::RefreshAdvertisement()
    {
        m_wantRequiresCode = pairing::Required(); // Tick republishes under the new UUID
    }

    void BluetoothReceiver::Tick()
    {
        if (m_shutdown)
        {
            return;
        }
        if (!m_adapterCheckStarted)
        {
            m_adapterCheckStarted = true;
            CheckAdapterAsync();
            return;
        }

        bool enabled = false;
        bool supported = false;
        bool connected = false;
        bool muted = false;
        uint64_t connectedAt = 0;
        {
            std::lock_guard guard(m_lock);
            enabled = m_status.enabled;
            supported = m_status.supported;
            connected = m_status.connected;
            muted = m_status.muted;
            connectedAt = m_connectedAt;
        }

        if (!m_adapterChecked || !supported || !enabled)
        {
            if (Now() >= m_stopProviderAt)
            {
                StopProvider();
            }
            return;
        }
        {
            // The pairing setting changed: republish under the matching UUID, but don't cut
            // off an iPhone that's connected right now.
            bool stale = false;
            {
                std::lock_guard guard(m_lock);
                stale = m_provider && m_providerRequiresCode != m_wantRequiresCode.load();
            }
            if (stale && !connected)
            {
                StopProvider();
                m_lastProviderAttempt = 0;
            }
        }
        EnsureProvider();

        if (connected)
        {
            const uint64_t generation = m_generation.load();
            if (!m_arbiter.Owns(Source::Bluetooth))
            {
                // Newest connection wins: Wi-Fi took over.
                Drop(generation, L"Switched to Wi-Fi.", true);
                return;
            }
            const uint64_t lastAudio = m_lastAudioTick.load();
            if (!muted && Now() - std::max(lastAudio, connectedAt) > kSilentDropMs)
            {
                Drop(generation, L"The iPhone stopped sending audio.", false);
            }
        }
    }

    void BluetoothReceiver::Disconnect()
    {
        Drop(m_generation.load(), L"Disconnected from this PC.", true);
    }

    void BluetoothReceiver::RequestSwitchToWiFi()
    {
        std::wstring clientId;
        {
            std::lock_guard guard(m_lock);
            if (!m_status.connected || !m_client)
            {
                return;
            }
            clientId = m_client->id;
        }
        Notify(clientId, R"({"type":"switch","transport":"wifi"})");
    }

    void BluetoothReceiver::Shutdown()
    {
        if (m_shutdown.exchange(true))
        {
            return;
        }
        if (Drop(m_generation.load(), L"Phomic on the PC was closed.", true))
        {
            Sleep(150); // let "bye" go out, so the iPhone doesn't wait for this PC to come back
        }
        ++m_generation;
        StopProvider();
    }

    BluetoothReceiver::Status BluetoothReceiver::GetStatus()
    {
        std::lock_guard guard(m_lock);
        Status status = m_status;
        static constexpr const wchar_t* kProviderStates[] = { L"created", L"stopped", L"advertising", L"aborted", L"advertising (partly)" };
        try
        {
            const auto provider = m_provider ? static_cast<int>(m_provider.AdvertisementStatus()) : -1;
            status.advertisingState = provider >= 0 && provider < 5 ? kProviderStates[provider] : L"off";
        }
        catch (hresult_error const&)
        {
        }
        const uint64_t last = m_lastAudioTick.load();
        status.msSinceAudio = last ? Now() - last : UINT64_MAX;
        return status;
    }

    // ---------------------------------------------------------------- adapter and advertising

    winrt::fire_and_forget BluetoothReceiver::CheckAdapterAsync()
    {
        auto self = shared_from_this();
        bool supported = false;
        std::wstring problem;
        try
        {
            auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
            if (!adapter)
            {
                problem = L"This PC has no Bluetooth.";
            }
            else if (!adapter.IsLowEnergySupported())
            {
                problem = L"This PC's Bluetooth doesn't support Bluetooth LE.";
            }
            else if (!adapter.IsPeripheralRoleSupported())
            {
                problem = L"This PC's Bluetooth can't accept connections from iPhones.";
            }
            else
            {
                supported = true;
            }
        }
        catch (hresult_error const& e)
        {
            problem = std::wstring(L"Bluetooth isn't available: ") + e.message().c_str();
        }
        {
            std::lock_guard guard(m_lock);
            m_status.supported = supported;
            if (!supported)
            {
                m_status.problem = problem;
            }
        }
        m_adapterChecked = true;
    }

    void BluetoothReceiver::EnsureProvider()
    {
        GattServiceProvider provider{ nullptr };
        {
            std::lock_guard guard(m_lock);
            provider = m_provider;
        }
        const uint64_t now = Now();
        if (!provider)
        {
            if (!m_creatingProvider && now - m_lastProviderAttempt >= kRetryMs)
            {
                m_lastProviderAttempt = now;
                m_creatingProvider = true;
                CreateProviderAsync();
            }
            return;
        }
        const auto status = provider.AdvertisementStatus();
        if (status == GattServiceProviderAdvertisementStatus::Started ||
            status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData)
        {
            return;
        }
        if (now - m_lastProviderAttempt >= kRetryMs)
        {
            m_lastProviderAttempt = now;
            StartAdvertising(provider);
        }
    }

    void BluetoothReceiver::StopProvider()
    {
        GattServiceProvider provider{ nullptr };
        {
            std::lock_guard guard(m_lock);
            provider = std::exchange(m_provider, nullptr);
            m_audioCharacteristic = nullptr;
            m_controlCharacteristic = nullptr;
            m_status.listening = false;
            m_status.connecting = false;
        }
        if (provider)
        {
            try
            {
                provider.StopAdvertising();
            }
            catch (hresult_error const&)
            {
            }
        }
        // Releasing the provider removes the service from this PC's GATT database.
    }

    winrt::fire_and_forget BluetoothReceiver::CreateProviderAsync()
    {
        auto self = shared_from_this();
        std::wstring problem;
        const bool requiresCode = m_wantRequiresCode.load();
        try
        {
            auto result = co_await GattServiceProvider::CreateAsync(ServiceUuid(requiresCode));
            if (result.Error() != BluetoothError::Success)
            {
                throw hresult_error(E_FAIL, ErrorText(result.Error()));
            }
            auto provider = result.ServiceProvider();

            GattLocalCharacteristicParameters audioParameters;
            audioParameters.CharacteristicProperties(GattCharacteristicProperties::WriteWithoutResponse);
            audioParameters.WriteProtectionLevel(GattProtectionLevel::Plain);
            auto audio = co_await provider.Service().CreateCharacteristicAsync(kAudioUuid, audioParameters);
            if (audio.Error() != BluetoothError::Success)
            {
                throw hresult_error(E_FAIL, ErrorText(audio.Error()));
            }

            GattLocalCharacteristicParameters controlParameters;
            controlParameters.CharacteristicProperties(GattCharacteristicProperties::Write | GattCharacteristicProperties::Notify);
            controlParameters.WriteProtectionLevel(GattProtectionLevel::Plain);
            controlParameters.ReadProtectionLevel(GattProtectionLevel::Plain);
            auto control = co_await provider.Service().CreateCharacteristicAsync(kControlUuid, controlParameters);
            if (control.Error() != BluetoothError::Success)
            {
                throw hresult_error(E_FAIL, ErrorText(control.Error()));
            }

            std::weak_ptr<BluetoothReceiver> weak = self;
            auto audioCharacteristic = audio.Characteristic();
            auto controlCharacteristic = control.Characteristic();
            audioCharacteristic.WriteRequested([weak](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args)
            {
                if (auto receiver = weak.lock())
                {
                    receiver->OnWriteAsync(args, true);
                }
            });
            controlCharacteristic.WriteRequested([weak](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args)
            {
                if (auto receiver = weak.lock())
                {
                    receiver->OnWriteAsync(args, false);
                }
            });
            controlCharacteristic.SubscribedClientsChanged([weak](GattLocalCharacteristic const&, winrt::Windows::Foundation::IInspectable const&)
            {
                if (auto receiver = weak.lock())
                {
                    receiver->CheckSubscribers();
                }
            });
            provider.AdvertisementStatusChanged([weak](GattServiceProvider const&, GattServiceProviderAdvertisementStatusChangedEventArgs const& args)
            {
                if (auto receiver = weak.lock())
                {
                    receiver->OnAdvertisementStatus(args.Status(), args.Error());
                }
            });

            bool keep = false;
            {
                std::lock_guard guard(m_lock);
                if (!m_shutdown && m_status.enabled)
                {
                    m_provider = provider;
                    m_providerRequiresCode = requiresCode;
                    m_audioCharacteristic = audioCharacteristic;
                    m_controlCharacteristic = controlCharacteristic;
                    keep = true;
                }
            }
            if (keep)
            {
                StartAdvertising(provider);
            }
        }
        catch (hresult_error const& e)
        {
            problem = e.message().c_str();
        }
        if (!problem.empty())
        {
            std::lock_guard guard(m_lock);
            m_status.listening = false;
            m_status.problem = problem;
        }
        m_creatingProvider = false;
    }

    void BluetoothReceiver::StartAdvertising(GattServiceProvider const& provider)
    {
        try
        {
            // Connectable + discoverable: the advertisement carries the service UUID.
            GattServiceProviderAdvertisingParameters parameters;
            parameters.IsConnectable(true);
            parameters.IsDiscoverable(true);
            provider.StartAdvertising(parameters);
        }
        catch (hresult_error const& e)
        {
            std::lock_guard guard(m_lock);
            m_status.listening = false;
            m_status.problem = std::wstring(L"iPhones can't connect over Bluetooth: ") + e.message().c_str();
        }
    }

    void BluetoothReceiver::OnAdvertisementStatus(GattServiceProviderAdvertisementStatus status, BluetoothError error)
    {
        std::lock_guard guard(m_lock);
        m_status.listening = status == GattServiceProviderAdvertisementStatus::Started ||
                             status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData;
        m_status.partialAdvertisement = status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData;
        if (m_status.listening)
        {
            m_status.problem.clear();
        }
        else if (status == GattServiceProviderAdvertisementStatus::Aborted)
        {
            m_status.problem = ErrorText(error);
        }
    }

    // ---------------------------------------------------------------- the connected iPhone

    winrt::fire_and_forget BluetoothReceiver::OnWriteAsync(GattWriteRequestedEventArgs args, bool audio)
    {
        auto self = shared_from_this();
        auto deferral = args.GetDeferral();
        try
        {
            const GattSession session = args.Session();
            const GattWriteRequest request = co_await args.GetRequestAsync();
            if (request)
            {
                const IBuffer value = request.Value();
                if (request.Option() == GattWriteOption::WriteWithResponse)
                {
                    request.Respond();
                }
                if (audio)
                {
                    OnAudio(session, value);
                }
                else
                {
                    OnControl(session, value);
                }
            }
        }
        catch (hresult_error const&)
        {
        }
        deferral.Complete();
    }

    void BluetoothReceiver::OnControl(GattSession const& session, IBuffer const& value)
    {
        if (m_shutdown)
        {
            return;
        }
        auto object = ParseJson(value);
        if (!object)
        {
            return;
        }
        try
        {
            const std::wstring clientId = ClientIdOf(session);
            const auto type = object.GetNamedString(L"type", L"");
            if (type == L"hello")
            {
                OnHello(session, clientId, object);
                return;
            }

            uint64_t generation = 0;
            {
                std::lock_guard guard(m_lock);
                if (!m_status.connected || !m_client || m_client->id != clientId)
                {
                    return; // not the iPhone we're listening to
                }
                generation = m_generation.load();
                if (type == L"state")
                {
                    m_status.deviceName = object.GetNamedString(L"name", L"iPhone").c_str();
                    m_status.muted = object.GetNamedBoolean(L"muted", false);
                    return;
                }
            }
            if (type == L"bye")
            {
                Drop(generation, L"The iPhone disconnected.", false);
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void BluetoothReceiver::OnHello(GattSession const& session, std::wstring const& clientId, JsonObject const& hello)
    {
        const std::wstring name = hello.GetNamedString(L"name", L"iPhone").c_str();
        const std::wstring code = hello.GetNamedString(L"code", L"").c_str();
        const bool muted = hello.GetNamedBoolean(L"muted", false);
        {
            std::lock_guard guard(m_lock);
            if (!m_status.enabled || m_shutdown)
            {
                return;
            }
        }

        if (!pairing::Accepts(code))
        {
            Notify(clientId, std::format(
                R"({{"type":"error","reason":"pairing","pcId":"{}","name":"{}","message":"{}"}})",
                JsonEscape(std::wstring_view(pairing::PcId())), JsonEscape(std::wstring_view(m_pcName)),
                code.empty() ? "This PC needs a pairing code." : "Wrong pairing code."));
            std::lock_guard guard(m_lock);
            m_status.connecting = false;
            m_status.lastDisconnect = std::format(L"{} was turned away: {}.", name,
                code.empty() ? L"no pairing code" : L"wrong pairing code");
            return;
        }

        auto client = std::make_shared<Client>();
        client->id = clientId;
        client->session = session;
        std::shared_ptr<Client> previous;
        uint64_t generation = 0;
        {
            std::lock_guard guard(m_lock);
            if (m_status.connected && m_client && m_client->id != clientId)
            {
                previous = m_client;
            }
            generation = ++m_generation;
            m_client = client;
            m_connectedAt = Now();
            m_status.connected = true;
            m_status.connecting = false;
            m_status.deviceName = name;
            m_status.muted = muted;
            m_status.packets = 0;
            m_status.sampleRate = 0;
            m_status.codec.clear();
            m_status.intervalMs = 0;
            m_status.problem.clear();
            m_status.lastDisconnect.clear();
        }
        if (previous)
        {
            Notify(previous->id, R"({"type":"bye","reason":"Another iPhone connected to this PC."})");
        }

        // Newest connection wins: a Wi-Fi session (if any) hangs up when it sees this.
        m_arbiter.Force(Source::Bluetooth);
        {
            std::lock_guard decodeGuard(m_decodeLock);
            m_haveSequence = false;
            m_upsampler.SetFactor(0);
            if (m_opus)
            {
                opus_decoder_destroy(m_opus);
            }
            int opusError = 0;
            m_opus = opus_decoder_create(kSampleRate, 1, &opusError);
            if (opusError != OPUS_OK)
            {
                m_opus = nullptr;
            }
        }
        m_jitter.Reset();
        m_lastAudioTick = 0;

        Notify(clientId, std::format(R"({{"type":"welcome","v":2,"name":"{}","pcId":"{}"}})",
            JsonEscape(std::wstring_view(m_pcName)), JsonEscape(std::wstring_view(pairing::PcId()))));
        WatchLinkAsync(client, generation);
    }

    winrt::fire_and_forget BluetoothReceiver::WatchLinkAsync(std::shared_ptr<Client> client, uint64_t generation)
    {
        auto self = shared_from_this();
        std::weak_ptr<BluetoothReceiver> weak = self;
        try
        {
            // The link closing is the main sign that the iPhone is gone.
            client->sessionRevoker = client->session.SessionStatusChanged(auto_revoke,
                [weak, generation](GattSession const&, GattSessionStatusChangedEventArgs const& args)
            {
                if (args.Status() == GattSessionStatus::Closed)
                {
                    if (auto receiver = weak.lock())
                    {
                        receiver->Drop(generation, L"The Bluetooth connection was lost.", false);
                    }
                }
            });

            // Ask for short connection intervals (lower latency), and show what we got.
            auto device = co_await BluetoothLEDevice::FromIdAsync(client->session.DeviceId().Id());
            if (device && generation == m_generation.load())
            {
                client->device = device;
                try
                {
                    client->parameters = device.RequestPreferredConnectionParameters(
                        BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
                }
                catch (hresult_error const&)
                {
                    // Not supported in this role or on this Windows version.
                }
                try
                {
                    client->parametersRevoker = device.ConnectionParametersChanged(auto_revoke,
                        [weak, generation](BluetoothLEDevice const& sender, auto&&)
                    {
                        if (auto receiver = weak.lock())
                        {
                            receiver->UpdateInterval(sender, generation);
                        }
                    });
                    UpdateInterval(device, generation);
                }
                catch (hresult_error const&)
                {
                }
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void BluetoothReceiver::UpdateInterval(BluetoothLEDevice const& device, uint64_t generation)
    {
        try
        {
            const double intervalMs = device.GetConnectionParameters().ConnectionInterval() * 1.25;
            std::lock_guard guard(m_lock);
            if (generation == m_generation.load())
            {
                m_status.intervalMs = intervalMs;
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void BluetoothReceiver::CheckSubscribers()
    {
        GattLocalCharacteristic control{ nullptr };
        std::wstring clientId;
        bool connected = false;
        uint64_t generation = 0;
        {
            std::lock_guard guard(m_lock);
            control = m_controlCharacteristic;
            connected = m_status.connected;
            clientId = m_client ? m_client->id : std::wstring();
            generation = m_generation.load();
        }
        if (!control)
        {
            return;
        }
        bool clientSubscribed = false;
        bool othersSubscribed = false;
        try
        {
            for (auto const& subscriber : control.SubscribedClients())
            {
                if (connected && ClientIdOf(subscriber.Session()) == clientId)
                {
                    clientSubscribed = true;
                }
                else
                {
                    othersSubscribed = true;
                }
            }
        }
        catch (hresult_error const&)
        {
            return;
        }
        {
            // An iPhone that subscribed but hasn't said hello yet is on its way in.
            std::lock_guard guard(m_lock);
            m_status.connecting = !m_status.connected && othersSubscribed;
        }
        if (connected && !clientSubscribed)
        {
            Drop(generation, L"The Bluetooth connection was lost.", false);
        }
    }

    bool BluetoothReceiver::Drop(uint64_t generation, std::wstring const& reason, bool sendBye)
    {
        std::shared_ptr<Client> client;
        {
            std::lock_guard guard(m_lock);
            if (!m_status.connected || generation != m_generation.load())
            {
                return false;
            }
            ++m_generation;
            client = std::move(m_client);
            m_status.connected = false;
            m_status.muted = false;
            m_status.sampleRate = 0;
            m_status.intervalMs = 0;
            m_status.lastDisconnect = reason;
        }
        if (m_arbiter.Owns(Source::Bluetooth))
        {
            m_arbiter.Release(Source::Bluetooth);
            m_jitter.Reset();
        }
        m_lastAudioTick = 0;
        if (sendBye && client)
        {
            // The iPhone hangs up when it gets this, and doesn't try to come back.
            Notify(client->id, R"({"type":"bye"})");
        }
        return true;
    }

    void BluetoothReceiver::Notify(std::wstring const& clientId, std::string text)
    {
        GattLocalCharacteristic control{ nullptr };
        {
            std::lock_guard guard(m_lock);
            control = m_controlCharacteristic;
        }
        if (control && !clientId.empty())
        {
            NotifyClient(control, clientId, std::move(text));
        }
    }

    winrt::fire_and_forget BluetoothReceiver::NotifyClient(GattLocalCharacteristic control, std::wstring clientId, std::string text)
    {
        // Windows can list a new subscription a moment after the iPhone's first write (its
        // "hello"), so wait for it rather than drop the "welcome": up to 2 s.
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            GattSubscribedClient target{ nullptr };
            try
            {
                for (auto const& subscriber : control.SubscribedClients())
                {
                    if (ClientIdOf(subscriber.Session()) == clientId)
                    {
                        target = subscriber;
                        break;
                    }
                }
            }
            catch (hresult_error const&)
            {
                co_return;
            }
            if (target)
            {
                try
                {
                    co_await control.NotifyValueAsync(MakeBuffer(text), target);
                }
                catch (hresult_error const&)
                {
                }
                co_return;
            }
            co_await winrt::resume_after(std::chrono::milliseconds(50));
        }
    }

    // ---------------------------------------------------------------- audio

    void BluetoothReceiver::OnAudio(GattSession const& session, IBuffer const& value)
    {
        if (m_shutdown || !value || !m_arbiter.Owns(Source::Bluetooth))
        {
            return;
        }
        {
            const std::wstring clientId = ClientIdOf(session);
            std::lock_guard guard(m_lock);
            if (!m_status.connected || !m_client || m_client->id != clientId)
            {
                return;
            }
        }
        const uint8_t* p = value.data();
        const uint32_t length = value.Length();
        if (length < 3)
        {
            return;
        }
        const uint8_t format = p[0];
        const uint16_t sequence16 = static_cast<uint16_t>(p[1] | (p[2] << 8));

        std::lock_guard decodeGuard(m_decodeLock);

        // Unwrap the 16-bit sequence number into the jitter buffer's 32-bit space.
        uint32_t sequence32 = 0;
        int16_t gap = 0;
        if (!m_haveSequence)
        {
            m_haveSequence = true;
            m_sequence32 = sequence16;
            m_lastSequence16 = sequence16;
            sequence32 = sequence16;
        }
        else
        {
            const int16_t delta = static_cast<int16_t>(sequence16 - m_lastSequence16);
            if (delta <= 0 && format == kFormatOpus)
            {
                return; // late or repeated: already concealed, and Opus must decode in order
            }
            sequence32 = m_sequence32 + delta;
            if (delta > 0)
            {
                gap = static_cast<int16_t>(delta - 1);
                m_sequence32 = sequence32;
                m_lastSequence16 = sequence16;
            }
        }

        float frame[kFrameSamples];
        uint32_t sampleRate = 0;
        if (format == kFormatOpus)
        {
            if (!m_opus)
            {
                return;
            }
            // A few packets went missing: let Opus conceal them (keeps its state in step).
            if (gap > 0 && gap <= kMaxConcealFrames)
            {
                for (int16_t i = gap; i > 0; --i)
                {
                    float concealed[kFrameSamples];
                    if (opus_decode_float(m_opus, nullptr, 0, concealed, kFrameSamples, 0) == static_cast<int>(kFrameSamples))
                    {
                        int16_t pcm[kFrameSamples];
                        for (uint32_t s = 0; s < kFrameSamples; ++s)
                        {
                            pcm[s] = static_cast<int16_t>(std::lrint(std::clamp(concealed[s], -1.f, 1.f) * 32767.f));
                        }
                        m_jitter.Push(sequence32 - static_cast<uint32_t>(i), pcm, false);
                    }
                }
            }
            const int decoded = opus_decode_float(m_opus, p + 3, static_cast<opus_int32>(length - 3), frame, kFrameSamples, 0);
            if (decoded != static_cast<int>(kFrameSamples))
            {
                return; // corrupt packet or unexpected frame size: the jitter buffer conceals it
            }
            sampleRate = kSampleRate;
        }
        else if (format == kFormat24k || format == kFormat16k)
        {
            // Fallback: IMA-ADPCM at 24/16 kHz, upsampled to 48 kHz.
            const int factor = format == kFormat24k ? 2 : 3;
            const uint32_t samples = kFrameSamples / factor;
            if (length != kHeaderBytes + samples / 2)
            {
                return;
            }
            adpcm::Decoder decoder;
            decoder.predictor = static_cast<int16_t>(p[3] | (p[4] << 8));
            decoder.index = std::min<int32_t>(p[5], 88);
            float decoded[kFrameSamples / 2];
            for (uint32_t i = 0; i < samples / 2; ++i)
            {
                const uint8_t byte = p[kHeaderBytes + i];
                decoded[2 * i] = decoder.Decode(byte & 0x0F) / 32768.f;
                decoded[2 * i + 1] = decoder.Decode(static_cast<uint8_t>(byte >> 4)) / 32768.f;
            }
            m_upsampler.SetFactor(factor);
            m_upsampler.Process(decoded, samples, frame);
            sampleRate = kSampleRate / factor;
        }
        else
        {
            return;
        }

        int16_t pcm[kFrameSamples];
        float peak = 0.f;
        for (uint32_t i = 0; i < kFrameSamples; ++i)
        {
            const float v = std::clamp(frame[i], -1.f, 1.f);
            peak = std::max(peak, std::abs(v));
            pcm[i] = static_cast<int16_t>(std::lrint(v * 32767.f));
        }
        m_jitter.Push(sequence32, pcm);
        if (peak > m_peak.load(std::memory_order_relaxed))
        {
            m_peak.store(peak, std::memory_order_relaxed);
        }
        m_lastAudioTick = Now();
        std::lock_guard guard(m_lock);
        ++m_status.packets;
        m_status.sampleRate = sampleRate;
        m_status.codec = format == kFormatOpus ? L"Opus" : L"ADPCM";
    }
}
