#pragma once

#include "MainWindow.g.h"
#include "JitterBuffer.h"
#include "AudioOutput.h"
#include "CableOutput.h"
#include "BluetoothReceiver.h"
#include "SourceArbiter.h"
#include "StreamServer.h"
#include "TrayIcon.h"

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

    private:
        enum class MicAction { Setup };
        struct MicResult
        {
            HRESULT hr = S_OK;
        };
        void UpdateMicSetup();
        void RunMicAction(MicAction action);
        static MicResult RunMicActionBlocking(MicAction action);
        void OnMicActionDone(MicResult const& result);

        void ResizeForDpi();
        HWND WindowHandle();
        void SetUpTray();
        void ShowFromTray();
        void Quit();
        void FitToContent(double contentHeight);
        void RefreshDevices();
        void StartOutput(std::wstring const& target);
        bool OutputRunning() const;
        void OnTick();
        void ApplyBufferTarget();
        void UpdateConnection(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth);
        void UpdateTransports(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth);
        void StartWiFi();
        void UpdatePairing();
        void UpdateDetails(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth);
        static void SetDetailRows(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid,
            std::vector<std::pair<std::wstring, std::wstring>> const& rows);
        void ShowError(std::wstring const& title, std::wstring const& message);
        void Shutdown();

        // Declaration order matters: members are destroyed in reverse order, and the
        // output, server and Bluetooth receiver all use the jitter buffer and arbiter.
        phomic::SourceArbiter m_arbiter;
        std::unique_ptr<phomic::JitterBuffer> m_jitter;
        std::unique_ptr<phomic::StreamServer> m_server;
        std::shared_ptr<phomic::BluetoothReceiver> m_bluetooth;
        std::unique_ptr<phomic::AudioOutput> m_output;
        std::unique_ptr<phomic::CableOutput> m_cableOutput;
        std::unique_ptr<phomic::DeviceWatcher> m_watcher;
        int m_bufferPreset = 1;
        uint32_t m_appliedTarget = 0;
        bool m_appliedAuto = false;
        std::wstring m_shownBluetooth;
        std::wstring m_shownWiFi;
        bool m_wifiEnabled = true;
        uint64_t m_switchRequestedTick = 0;
        // Wi-Fi server start, retried while the port is busy (see StartWiFi).
        bool m_wifiRetrying = false;
        bool m_wifiErrorShown = false;
        uint64_t m_wifiRetrySince = 0;
        uint64_t m_wifiLastTry = 0;

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
        std::atomic<float> m_gain{ 1.f };
        bool m_initialized = false;
        bool m_updatingDevices = false;

        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        uint64_t m_tick = 0;
        uint64_t m_lastRestartTick = 0;
        float m_meter = 0.f;
        std::wstring m_shownTitle;
        std::wstring m_shownDetail;

        uint64_t m_lastPackets = 0;
        uint64_t m_lastReceived = 0;
        uint64_t m_lastConcealed = 0;

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
