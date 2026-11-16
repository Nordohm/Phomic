#pragma once

#include "Protocol.h"
#include "Resampler.h"
#include "SourceArbiter.h"

struct OpusDecoder;

namespace phomic
{
    class JitterBuffer;

    // Receives audio from an iPhone over Bluetooth LE.
    //
    // The PC is the GATT server (peripheral) and the iPhone connects to it, the way a phone
    // connects to headphones or a watch:
    //   - The Phomic service is published and advertised under a UUID that carries this PC's
    //     id and whether it wants a pairing code. iPhones list PCs from that advertisement
    //     (with the PC's Bluetooth name) and look for exactly the PC the user picked.
    //     It's the only advertisement: many adapters send just one at a time.
    //   - The iPhone subscribes to the control characteristic and writes "hello"; the PC
    //     answers "welcome" (or a pairing error) as a notification.
    //   - Audio arrives as write-without-response packets (10 ms Opus frames), which are
    //     decoded into the shared JitterBuffer.
    // After an unexpected drop the iPhone reconnects by itself (iOS keeps a pending
    // connection, even in the background), so the PC never scans or retries.
    //
    // Create with std::make_shared (async callbacks hold weak references).
    class BluetoothReceiver : public std::enable_shared_from_this<BluetoothReceiver>
    {
    public:
        struct Status
        {
            bool supported = true;     // false: no Bluetooth LE adapter, or it can't accept connections
            bool enabled = true;       // user setting
            bool listening = false;    // the Phomic service is published and advertised
            bool partialAdvertisement = false; // Windows left something out of the advertisement
            bool connecting = false;   // an iPhone is connected but hasn't said hello yet
            bool connected = false;
            std::wstring deviceName;
            bool muted = false;
            uint32_t sampleRate = 0;
            std::wstring codec;        // "Opus", or "ADPCM" (fallback)
            uint64_t packets = 0;
            uint64_t msSinceAudio = UINT64_MAX;
            double intervalMs = 0;     // Bluetooth connection interval, when Windows reports it
            std::wstring problem;      // e.g. Bluetooth turned off
            std::wstring lastDisconnect;
            std::wstring advertisingState;  // for Show details: what Windows says about the advertisement
        };

        BluetoothReceiver(JitterBuffer& jitter, SourceArbiter& arbiter, std::wstring pcName);
        ~BluetoothReceiver();
        BluetoothReceiver(BluetoothReceiver const&) = delete;
        BluetoothReceiver& operator=(BluetoothReceiver const&) = delete;

        void SetEnabled(bool enabled);
        // Keeps the service published and watches the connection. UI thread, every ~50 ms.
        void Tick();
        void Disconnect();
        // Ask the connected phone to move the session to Wi-Fi.
        void RequestSwitchToWiFi();
        // The pairing setting changed: advertise it (once no iPhone is connected). UI thread.
        void RefreshAdvertisement();
        void Shutdown();

        Status GetStatus();
        float TakePeak() { return m_peak.exchange(0.f); }

    private:
        // The iPhone that said hello, and what keeps track of its link.
        struct Client
        {
            std::wstring id;   // BluetoothDeviceId
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession session{ nullptr };
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession::SessionStatusChanged_revoker sessionRevoker;
            winrt::Windows::Devices::Bluetooth::BluetoothLEDevice device{ nullptr };
            winrt::Windows::Devices::Bluetooth::BluetoothLEPreferredConnectionParametersRequest parameters{ nullptr };
            winrt::Windows::Devices::Bluetooth::BluetoothLEDevice::ConnectionParametersChanged_revoker parametersRevoker;
            ~Client();
        };

        winrt::fire_and_forget CheckAdapterAsync();
        void EnsureProvider();
        void StopProvider();
        winrt::fire_and_forget CreateProviderAsync();
        void StartAdvertising(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProvider const& provider);
        void OnAdvertisementStatus(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProviderAdvertisementStatus status,
            winrt::Windows::Devices::Bluetooth::BluetoothError error);

        winrt::fire_and_forget OnWriteAsync(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteRequestedEventArgs args, bool audio);
        void OnControl(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession const& session,
            winrt::Windows::Storage::Streams::IBuffer const& value);
        void OnHello(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession const& session,
            std::wstring const& clientId, winrt::Windows::Data::Json::JsonObject const& hello);
        void OnAudio(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession const& session,
            winrt::Windows::Storage::Streams::IBuffer const& value);
        void CheckSubscribers();
        winrt::fire_and_forget WatchLinkAsync(std::shared_ptr<Client> client, uint64_t generation);
        void UpdateInterval(winrt::Windows::Devices::Bluetooth::BluetoothLEDevice const& device, uint64_t generation);

        // Ends the session `generation` (if it's still the current one).
        bool Drop(uint64_t generation, std::wstring const& reason, bool sendBye);
        void Notify(std::wstring const& clientId, std::string text);
        static winrt::fire_and_forget NotifyClient(
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic control,
            std::wstring clientId, std::string text);

        JitterBuffer& m_jitter;
        SourceArbiter& m_arbiter;
        const std::wstring m_pcName;

        std::mutex m_lock;          // m_status, m_client, the provider and its characteristics
        Status m_status;
        std::shared_ptr<Client> m_client;
        uint64_t m_connectedAt = 0;

        std::atomic<uint64_t> m_generation{ 0 };
        std::atomic<bool> m_shutdown{ false };
        std::atomic<bool> m_adapterChecked{ false };
        bool m_adapterCheckStarted = false;      // UI thread only

        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProvider m_provider{ nullptr };
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic m_controlCharacteristic{ nullptr };
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic m_audioCharacteristic{ nullptr };
        std::atomic<bool> m_creatingProvider{ false };
        uint64_t m_lastProviderAttempt = 0;      // UI thread only
        uint64_t m_stopProviderAt = 0;           // UI thread only: lets a last "bye" go out first

        bool m_providerRequiresCode = false;     // what the published service UUID says
        std::atomic<bool> m_wantRequiresCode{ false };

        // Decoding state (audio writes).
        std::mutex m_decodeLock;
        Upsampler m_upsampler;
        OpusDecoder* m_opus = nullptr;
        bool m_haveSequence = false;
        uint16_t m_lastSequence16 = 0;
        uint32_t m_sequence32 = 0;
        std::atomic<float> m_peak{ 0.f };
        std::atomic<uint64_t> m_lastAudioTick{ 0 };
    };
}
