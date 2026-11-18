#pragma once

#include "MainWindow.g.h"
#include "JitterBuffer.h"
#include "AudioOutput.h"
#include "CableOutput.h"
#include "BluetoothReceiver.h"
#include "SourceArbiter.h"
#include "StreamServer.h"
#include "UsbReceiver.h"
#include "AutoGain.h"
#include "Limiter.h"
#include "TrayIcon.h"
#include "Firewall.h"

namespace winrt::Phomic::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow()
        {
            // Xaml objects should not call InitializeComponent during construction.
            // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent
        }
        ~MainWindow();

        void InitializeComponent();

        void DisconnectButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void GainSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OutputCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void BufferCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void BluetoothToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void WiFiToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SwitchToWiFiButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SwitchToBluetoothButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void UsbToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SwitchToUsbButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void GetIPhoneAppLink_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void UsbActionButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void PairingCheck_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void NewCodeButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void DetailsButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void MicSetupButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void StartupCheck_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void CloseToTrayCheck_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void UpdateButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void AutoGainToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void WiFiAllowButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        enum class MicAction { Setup };
        struct MicResult
        {
            HRESULT hr = S_OK;
        };
        void UpdateMicSetup();
        winrt::fire_and_forget ShowAppStoreBadge();
        winrt::fire_and_forget ShowIPhoneAppCode();
        winrt::fire_and_forget CheckForUpdate();
        std::wstring m_updateVersion;   // newer release on GitHub, e.g. "1.2.0" (empty: none)
        std::wstring m_updateUrl;       // its release page
        double m_badgeScale = 0;        // display scale the App Store badge was drawn for
        void RunMicAction(MicAction action);
        static MicResult RunMicActionBlocking(MicAction action);
        void OnMicActionDone(MicResult const& result);

        void ResizeForDpi();
        HWND WindowHandle();
        void SetUpTray();
        void ShowFromTray();
        void Quit();
        void FitToContent(double contentHeight);
        bool SizesAreReal();
        void KeepShape();
        bool m_fitPending = false; // content changed while the window was hidden or minimized
        bool m_firstFocusDone = false;
        void RefreshDevices();
        void StartOutput(std::wstring const& target);
        bool OutputRunning() const;
        void OnTick();
        void ApplyBufferTarget();
        void UpdateConnection(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
            phomic::UsbReceiver::Status const& usb);
        void TrackLastDisconnect(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
            phomic::UsbReceiver::Status const& usb);
        void UpdateTransports(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
            phomic::UsbReceiver::Status const& usb);
        void RefreshTransports();
        void RequestSwitch(phomic::Source to);
        void StartWiFi();
        void UpdatePairing();
        void UpdateDetails(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
            phomic::UsbReceiver::Status const& usb);
        static void SetDetailRows(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid,
            std::vector<std::pair<std::wstring, std::wstring>> const& rows);
        void ShowError(std::wstring const& title, std::wstring const& message);
        void Shutdown();

        // Declaration order matters: members are destroyed in reverse order, and the
        // output, server and Bluetooth and USB receivers all use the jitter buffer and arbiter.
        phomic::SourceArbiter m_arbiter;
        std::unique_ptr<phomic::JitterBuffer> m_jitter;
        std::unique_ptr<phomic::StreamServer> m_server;
        std::shared_ptr<phomic::BluetoothReceiver> m_bluetooth;
        std::unique_ptr<phomic::UsbReceiver> m_usb;
        std::unique_ptr<phomic::AudioOutput> m_output;
        std::unique_ptr<phomic::CableOutput> m_cableOutput;
        std::unique_ptr<phomic::DeviceWatcher> m_watcher;
        int m_bufferPreset = 1;
        uint32_t m_appliedTarget = 0;
        bool m_appliedAuto = false;
        std::wstring m_shownBluetooth;
        std::wstring m_shownWiFi;
        std::wstring m_shownUsb;
        std::array<bool, 3> m_shownInUse{};  // Wi-Fi, Bluetooth, USB rows marked as in use
        // What the USB row's button does (it fixes the problem the row describes).
        enum class UsbAction { None, GetAppleDevices, StartService, OpenAppleDevices, CopyCommand };
        UsbAction m_usbAction = UsbAction::None;
        std::wstring m_usbCommand;      // CopyCommand: what goes on the clipboard
        std::wstring m_shownUsbButton;  // what the USB row's button says now, and its tooltip
        std::wstring m_shownUsbTip;
        uint64_t m_usbCopiedTick = 0;
        uint64_t m_usbStartTick = 0;    // StartService clicked: if it doesn't help, offer the window
        bool m_showingAppCode = false;  // WinUI allows one ContentDialog at a time
        bool m_wifiEnabled = true;
        uint64_t m_switchRequestedTick = 0;
        // Wi-Fi server start, retried while the port is busy (see StartWiFi).
        bool m_wifiRetrying = false;
        bool m_wifiErrorShown = false;
        uint64_t m_wifiRetrySince = 0;
        uint64_t m_wifiLastTry = 0;
        // Windows Firewall: does it let iPhones reach Phomic over Wi-Fi (see CheckFirewall)?
        winrt::fire_and_forget CheckFirewall();
        phomic::firewall::Verdict m_firewall = phomic::firewall::Verdict::Unknown;
        bool m_firewallChecking = false;
        bool m_firewallFixing = false;  // the Allow button's UAC prompt is open

        // Virtual microphone (VB-CABLE) setup.
        std::wstring m_feedId;          // endpoint Phomic plays into, once found
        bool m_micBusy = false;

        std::vector<phomic::AudioDevice> m_devices;
        struct OutputTarget
        {
            std::wstring id;     // endpoint id, or kCableTarget for direct VB-CABLE output
            std::wstring label;
            std::wstring name;
            bool isDefault = false;
        };
        std::vector<OutputTarget> m_targets;
        std::wstring m_activeTarget;
        std::atomic<float> m_gain{ 1.f };          // manual gain (automatic gain off)
        std::atomic<bool> m_autoGainOn{ true };
        std::atomic<float> m_appliedGainDb{ 0.f }; // automatic gain's current gain, for the UI
        // Audio thread only (the output's source, see InitializeComponent):
        std::unique_ptr<phomic::AutoGain> m_autoGain;
        phomic::Limiter m_limiter;
        void UpdateGain(bool live);
        bool m_initialized = false;
        bool m_updatingDevices = false;

        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        uint64_t m_tick = 0;
        uint64_t m_lastRestartTick = 0;
        float m_meter = 0.f;
        std::wstring m_shownTitle;
        std::wstring m_shownDetail;
        std::wstring m_seenDisconnect[3];   // Wi-Fi, Bluetooth, USB: last reason each reported
        std::wstring m_lastDisconnect;      // the newest of them (see TrackLastDisconnect)

        uint64_t m_lastPackets = 0;
        uint64_t m_lastReceived = 0;
        uint64_t m_lastConcealed = 0;
        uint64_t m_packetsPerSecond = 0;
        double m_lossPercent = 0;
        void MeasureStream(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
            phomic::UsbReceiver::Status const& usb);

        // Tray and window size.
        std::unique_ptr<phomic::TrayIcon> m_tray;
        HICON m_appIcon = nullptr;
        HICON m_trayIcon = nullptr;
        bool m_closeToTray = false;
        bool m_quitting = false;
        double m_contentHeight = 0;        // RootPanel's height, epx
        int32_t m_neededClientHeight = 0;  // client height that shows all content, physical px
        double m_neededScale = 0;          // display scale m_neededClientHeight was computed for
    };
}

namespace winrt::Phomic::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
