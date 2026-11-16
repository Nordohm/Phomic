#include "pch.h"
#include "MainWindow.xaml.h"
#include "Startup.h"
#include "resource.h"
#include "Version.h"
#include <winrt/Microsoft.UI.Interop.h>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "Settings.h"
#include "VirtualMic.h"
#include "CableOutput.h"
#include "Pairing.h"

#include <shellapi.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace
{
    // Buffering choices: 0 = Auto (adapts to the measured jitter), then fixed targets in 10 ms frames.
    constexpr uint32_t kBufferPresets[] = { 0, 3, 6, 12 };
    constexpr int kBufferChoices = 4;
    constexpr int kDefaultBufferPreset = 0; // Auto
    // Bluetooth delivers packets in bursts (one burst per connection interval), so it
    // needs at least the "Balanced" cushion.
    constexpr uint32_t kMinBluetoothFrames = 6;
    constexpr int kTicksPerSecond = 20; // UI timer runs at 50 ms
    // Output target meaning "write straight into VB-CABLE" (see CableOutput).
    constexpr wchar_t kCableTarget[] = L"phomic:cable";

    float DbToGain(double db)
    {
        return static_cast<float>(std::pow(10.0, db / 20.0));
    }

    // A closed InfoBar keeps a zero-height slot that still gets the StackPanel's spacing,
    // so hidden banners are also collapsed.
    void ShowBar(InfoBar const& bar, bool open)
    {
        bar.IsOpen(open);
        bar.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return s;
    }
}

namespace winrt::Phomic::implementation
{
    MainWindow::~MainWindow()
    {
        Shutdown();
        m_tray.reset();
        if (m_trayIcon)
        {
            DestroyIcon(m_trayIcon);
        }
        if (m_appIcon)
        {
            DestroyIcon(m_appIcon);
        }
    }

    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();
        ResizeForDpi();
        // Follow Windows' light/dark mode in the title bar too.
        AppWindow().TitleBar().PreferredTheme(winrt::Microsoft::UI::Windowing::TitleBarTheme::UseDefaultAppMode);
        SetUpTray();

        namespace settings = phomic::settings;
        m_bufferPreset = std::clamp(settings::GetInt(settings::kBufferMode, kDefaultBufferPreset), 0, kBufferChoices - 1);
        const int gainDb = std::clamp(settings::GetInt(settings::kGainDb, 0), -12, 24);
        const bool bluetoothEnabled = settings::GetInt(settings::kBluetooth, 1) != 0;
        m_wifiEnabled = settings::GetInt(settings::kWiFi, 1) != 0;

        m_jitter = std::make_unique<phomic::JitterBuffer>();
        m_gain = DbToGain(gainDb);
        ApplyBufferTarget();

        auto source = [this](float* out, uint32_t frames)
        {
            m_jitter->Read(out, frames);
            const float gain = m_gain.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < frames; ++i)
            {
                out[i] = std::clamp(out[i] * gain, -1.f, 1.f);
            }
        };
        m_output = std::make_unique<phomic::AudioOutput>(source);
        m_cableOutput = std::make_unique<phomic::CableOutput>(source);
        m_server = std::make_unique<phomic::StreamServer>(*m_jitter, m_arbiter);
        m_bluetooth = std::make_shared<phomic::BluetoothReceiver>(*m_jitter, m_arbiter, m_server->ServiceName());
        m_bluetooth->SetEnabled(bluetoothEnabled);
        m_watcher = std::make_unique<phomic::DeviceWatcher>();

        BufferCombo().SelectedIndex(m_bufferPreset);
        GainSlider().Value(gainDb);
        GainSlider().Header(box_value(hstring(std::format(L"Gain ({:+d} dB)", gainDb))));
        BluetoothToggle().IsOn(bluetoothEnabled);
        WiFiToggle().IsOn(m_wifiEnabled);
        PairingCheck().IsChecked(phomic::pairing::Required());
        UpdatePairing();
        phomic::startup::Repair();
        StartupCheck().IsChecked(phomic::startup::IsEnabled());
        m_closeToTray = settings::GetInt(settings::kCloseToTray, 0) != 0;
        CloseToTrayCheck().IsChecked(m_closeToTray);

        RefreshDevices();

        if (m_wifiEnabled)
        {
            StartWiFi();
        }

        m_timer = DispatcherQueue().CreateTimer();
        m_timer.Interval(std::chrono::milliseconds(1000 / kTicksPerSecond));
        m_timer.Tick([weak = get_weak()](auto&&, auto&&)
        {
            if (auto self = weak.get())
            {
                self->OnTick();
            }
        });
        m_timer.Start();

        Closed([this](auto&&, auto&&) { Shutdown(); });
        for (auto bar : { ErrorBar(), DefaultOutputBar() })
        {
            bar.Closed([](InfoBar const& sender, auto&&) { sender.Visibility(Visibility::Collapsed); });
        }

        m_initialized = true;
        const auto wifi = m_server->GetStatus();
        const auto bluetooth = m_bluetooth->GetStatus();
        UpdateConnection(wifi, bluetooth);
        UpdateTransports(wifi, bluetooth);
        UpdateDetails(wifi, bluetooth);
    }

    void MainWindow::StartWiFi()
    {
        m_wifiLastTry = m_tick;
        if (m_server->Start(phomic::kDefaultPort))
        {
            if (m_wifiErrorShown)
            {
                ShowBar(ErrorBar(), false);
            }
            m_wifiRetrying = false;
            m_wifiErrorShown = false;
            return;
        }
        // OnTick keeps trying every few seconds. Right after Phomic restarts, Windows can hold
        // the port for up to two minutes (the old connections' TIME_WAIT), so a busy port
        // only counts as a problem once that's over.
        if (!m_wifiRetrying)
        {
            m_wifiRetrying = true;
            m_wifiRetrySince = m_tick;
        }
        const auto status = m_server->GetStatus();
        if (!m_wifiErrorShown && (!status.portInUse || m_tick - m_wifiRetrySince > 150 * kTicksPerSecond))
        {
            m_wifiErrorShown = true;
            ShowError(L"Can't accept Wi-Fi connections", status.error);
        }
    }

    void MainWindow::Shutdown()
    {
        if (m_timer)
        {
            m_timer.Stop();
        }
        if (m_bluetooth)
        {
            m_bluetooth->Shutdown();
        }
        if (m_server)
        {
            m_server->Stop();
        }
        if (m_output)
        {
            m_output->Stop();
        }
        if (m_cableOutput)
        {
            m_cableOutput->Stop();
        }
    }

    HWND MainWindow::WindowHandle()
    {
        HWND hwnd{};
        auto windowNative{ this->m_inner.as<::IWindowNative>() };
        windowNative->get_WindowHandle(&hwnd);
        return hwnd;
    }

    void MainWindow::ResizeForDpi()
    {
        const HWND hwnd = WindowHandle();
        const double scale = (hwnd ? GetDpiForWindow(hwnd) : 96) / 96.0;
        // Full width (FitToContent allows no wider). The height is a first guess: FitToContent
        // sets it once the content is laid out.
        AppWindow().ResizeClient({ static_cast<int32_t>(680 * scale), static_cast<int32_t>(800 * scale) });
        // The content is at most 620 wide and centered. (Stretch + MaxWidth would center it by
        // its desired width, which pushes it sideways when nothing in it is that wide.)
        RootScroller().SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& args)
        {
            if (auto self = weak.get())
            {
                const double margins = 24 + 24;
                self->RootPanel().Width(std::clamp(args.NewSize().Width - margins, 0.0, 620.0));
            }
        });
        // After each layout pass: how tall does the content want to be? (Its actual height
        // is no use: the ScrollViewer stretches it to at least the window's height.) Also
        // catches a move to a display with another scale.
        RootPanel().LayoutUpdated([weak = get_weak()](auto&&, auto&&)
        {
            if (auto self = weak.get())
            {
                const double height = self->RootPanel().DesiredSize().Height; // includes its margin
                const double scale = GetDpiForWindow(self->WindowHandle()) / 96.0;
                if (height != self->m_contentHeight || scale != self->m_neededScale)
                {
                    self->m_contentHeight = height;
                    self->FitToContent(height);
                }
            }
        });
    }

    void MainWindow::FitToContent(double contentHeight)
    {
        // The window is as tall as its content at most (nothing but empty space beyond that),
        // and follows the content as it changes, unless the user made the window shorter.
        if (contentHeight <= 0)
        {
            return;
        }
        auto window = AppWindow();
        const auto client = window.ClientSize();
        const double scale = GetDpiForWindow(WindowHandle()) / 96.0;
        if (m_neededScale > 0 && m_neededScale != scale)
        {
            m_neededClientHeight = static_cast<int32_t>(std::lround(m_neededClientHeight * scale / m_neededScale));
        }
        const bool wasFull = m_neededClientHeight == 0 || client.Height + 2 >= m_neededClientHeight;

        int32_t needed = static_cast<int32_t>(std::ceil(contentHeight * scale));
        namespace windowing = winrt::Microsoft::UI::Windowing;
        const auto area = windowing::DisplayArea::GetFromWindowId(window.Id(), windowing::DisplayAreaFallback::Nearest).WorkArea();
        const int32_t frameWidth = window.Size().Width - client.Width;
        const int32_t frameHeight = window.Size().Height - client.Height; // title bar and border
        needed = std::min(needed, area.Height - frameHeight);

        // Size limits (outer window size, physical pixels). Too narrow and the text wraps
        // awkwardly; wider than the content's maximum width is just empty space.
        if (auto presenter = window.Presenter().try_as<windowing::OverlappedPresenter>())
        {
            constexpr double kMinWidth = 440, kMaxWidth = 680, kMinHeight = 360; // client area, in epx
            presenter.IsMaximizable(false);
            presenter.PreferredMinimumWidth(static_cast<int32_t>(std::lround(kMinWidth * scale)) + frameWidth);
            presenter.PreferredMaximumWidth(static_cast<int32_t>(std::lround(kMaxWidth * scale)) + frameWidth);
            presenter.PreferredMinimumHeight(std::min(static_cast<int32_t>(std::lround(kMinHeight * scale)), needed) + frameHeight);
            presenter.PreferredMaximumHeight(needed + frameHeight);
        }

        if (wasFull ? client.Height != needed : client.Height > needed)
        {
            window.ResizeClient({ client.Width, needed });
            // Growing past the bottom of the screen: move up.
            const auto position = window.Position();
            const int32_t bottom = position.Y + window.Size().Height;
            if (bottom > area.Y + area.Height)
            {
                window.Move({ position.X, std::max(area.Y, area.Y + area.Height - window.Size().Height) });
            }
        }
        m_neededClientHeight = needed;
        m_neededScale = scale;
    }

    // ---------------------------------------------------------------- tray

    void MainWindow::SetUpTray()
    {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        m_appIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_PHOMIC), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        if (m_appIcon)
        {
            AppWindow().SetIcon(winrt::Microsoft::UI::GetIconIdFromIcon(m_appIcon));
        }
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(WindowHandle()));
        m_trayIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_PHOMIC), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        m_tray = std::make_unique<phomic::TrayIcon>(m_trayIcon, [weak = get_weak()](phomic::TrayIcon::Command command)
        {
            if (auto self = weak.get())
            {
                if (command == phomic::TrayIcon::Command::Open)
                {
                    self->ShowFromTray();
                }
                else
                {
                    self->Quit();
                }
            }
        });

        // With "Keep running in the tray" on, closing the window only hides it.
        AppWindow().Closing([weak = get_weak()](auto&&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
        {
            if (auto self = weak.get(); self && self->m_closeToTray && !self->m_quitting)
            {
                args.Cancel(true);
                self->AppWindow().Hide();
            }
        });
    }

    void MainWindow::ShowFromTray()
    {
        const HWND hwnd = WindowHandle();
        AppWindow().Show(true);
        if (IsIconic(hwnd))
        {
            ShowWindow(hwnd, SW_RESTORE);
        }
        Activate();
        SetForegroundWindow(hwnd);
    }

    void MainWindow::Quit()
    {
        m_quitting = true;
        Close();
    }

    void MainWindow::StartupCheck_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        const auto checked = StartupCheck().IsChecked();
        phomic::startup::SetEnabled(checked && checked.Value());
        StartupCheck().IsChecked(phomic::startup::IsEnabled());
    }

    void MainWindow::CloseToTrayCheck_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        const auto checked = CloseToTrayCheck().IsChecked();
        m_closeToTray = checked && checked.Value();
        phomic::settings::SetInt(phomic::settings::kCloseToTray, m_closeToTray ? 1 : 0);
    }

    // ---------------------------------------------------------------- devices

    void MainWindow::RefreshDevices()
    {
        UpdateMicSetup(); // also finds VB-CABLE's playback endpoint (m_feedId)

        std::vector<phomic::AudioDevice> devices;
        try
        {
            devices = phomic::EnumerateRenderDevices();
        }
        catch (hresult_error const&)
        {
        }
        m_devices = std::move(devices);

        // Where Phomic can play: straight into the Phomic microphone (VB-CABLE, no playback
        // endpoint needed), or any playback device (handy for listening).
        const bool cable = phomic::CableOutput::IsAvailable();
        m_targets.clear();
        if (cable)
        {
            m_targets.push_back({ kCableTarget, L"Phomic microphone", L"", false });
        }
        for (auto const& device : m_devices)
        {
            if (cable && device.id == m_feedId)
            {
                continue; // the same cable, reached through its endpoint: the direct target replaces it
            }
            m_targets.push_back({ device.id,
                device.isDefault ? device.name + L"   (Windows default)" : device.name,
                device.name, device.isDefault });
        }

        // Preference: the user's saved choice, then the Phomic microphone, then any other
        // virtual cable, then the Windows default.
        const std::wstring saved = phomic::settings::GetString(phomic::settings::kOutputDevice);
        auto find = [&](auto&& predicate)
        {
            for (size_t i = 0; i < m_targets.size(); ++i)
            {
                if (predicate(m_targets[i]))
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        int index = saved.empty() ? -1 : find([&](auto const& t) { return t.id == saved; });
        if (index < 0) index = find([](auto const& t) { return t.id == kCableTarget; });
        if (index < 0 && !m_feedId.empty()) index = find([&](auto const& t) { return t.id == m_feedId; });
        if (index < 0) index = find([](auto const& t) { return Lower(t.name).find(L"cable input") != std::wstring::npos; });
        if (index < 0) index = find([](auto const& t) { return !t.name.empty() && phomic::IsVirtualCable(t.name); });
        if (index < 0) index = find([](auto const& t) { return t.isDefault; });
        if (index < 0 && !m_targets.empty()) index = 0;

        m_updatingDevices = true;
        auto items = OutputCombo().Items();
        items.Clear();
        for (auto const& target : m_targets)
        {
            items.Append(box_value(hstring(target.label)));
        }
        OutputCombo().SelectedIndex(index);
        m_updatingDevices = false;

        // If Windows itself plays into the cable, every system sound ends up in the microphone.
        const bool defaultIsFeed = !m_feedId.empty() &&
            std::any_of(m_devices.begin(), m_devices.end(), [&](auto const& d) { return d.isDefault && d.id == m_feedId; });
        ShowBar(DefaultOutputBar(), defaultIsFeed);

        if (index < 0)
        {
            m_output->Stop();
            m_cableOutput->Stop();
            m_activeTarget.clear();
            ShowError(L"No playback device",
                L"Windows has no active audio playback device. Connect speakers or headphones, or install VB-CABLE; Phomic picks it up automatically.");
            return;
        }
        auto const& id = m_targets[index].id;
        if (id != m_activeTarget || !OutputRunning())
        {
            StartOutput(id);
        }
    }

    bool MainWindow::OutputRunning() const
    {
        if (m_activeTarget == kCableTarget)
        {
            return m_cableOutput->IsRunning();
        }
        return m_output->IsRunning() && m_output->DeviceId() == m_activeTarget;
    }

    // ---------------------------------------------------------------- Phomic microphone (VB-CABLE)

    void MainWindow::UpdateMicSetup()
    {
        namespace vm = phomic::virtualmic;
        std::optional<vm::Endpoints> endpoints;
        try
        {
            endpoints = vm::Find();
        }
        catch (hresult_error const&)
        {
        }
        const vm::State state = vm::GetState(endpoints);
        m_feedId = endpoints ? endpoints->feedId : std::wstring();

        auto bar = MicBar();
        auto button = MicSetupButton();
        button.IsEnabled(!m_micBusy);
        button.Visibility(Visibility::Visible);
        bar.Severity(InfoBarSeverity::Informational);

        if (m_micBusy)
        {
            bar.Title(L"Setting up the Phomic microphone…");
            bar.Message(L"If Windows asks for permission, allow it.");
            button.Content(box_value(L"Working…"));
            ShowBar(bar, true);
            return;
        }

        switch (state)
        {
        case vm::State::Ready:
            ShowBar(bar, false);
            return;
        case vm::State::NeedsSetup:
            bar.Title(L"Finish setting up the Phomic microphone");
            bar.Message(L"Names VB-CABLE's microphone “Phomic” and hides its playback devices. Phomic feeds the microphone directly, so “Phomic” is the only device other apps see.");
            button.Content(box_value(L"Set up"));
            break;
        case vm::State::NotInstalled:
            // VB-CABLE isn't shipped with Phomic: people get it from VB-Audio themselves.
            bar.Title(L"Make your iPhone a microphone for other apps");
            bar.Message(L"Windows only lets drivers add microphones, so Phomic uses VB-CABLE, a free virtual audio cable by "
                        L"VB-Audio. Download it, unzip it, run VBCABLE_Setup_x64.exe as administrator and click "
                        L"“Install Driver”. Phomic notices it here (restart if Windows asks).");
            button.Content(box_value(L"Get VB-CABLE"));
            break;
        }
        ShowBar(bar, true);
    }

    void MainWindow::MicSetupButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        namespace vm = phomic::virtualmic;
        std::optional<vm::Endpoints> endpoints;
        try
        {
            endpoints = vm::Find();
        }
        catch (hresult_error const&)
        {
        }
        if (vm::GetState(endpoints) != vm::State::NotInstalled)
        {
            RunMicAction(MicAction::Setup);
        }
        else
        {
            ShellExecuteW(nullptr, L"open", vm::kWebsite, nullptr, nullptr, SW_SHOWNORMAL); // VB-Audio's download page
        }
    }

    void MainWindow::RunMicAction(MicAction action)
    {
        if (m_micBusy)
        {
            return;
        }
        m_micBusy = true;
        UpdateMicSetup();

        // UAC prompts and VB-CABLE's installer block, so do the work off the UI thread.
        auto dispatcher = DispatcherQueue();
        std::thread([action, dispatcher, weak = get_weak()]
        {
            const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const MicResult result = RunMicActionBlocking(action);
            if (SUCCEEDED(coInit))
            {
                CoUninitialize();
            }
            dispatcher.TryEnqueue([weak, result]
            {
                if (auto self = weak.get())
                {
                    self->OnMicActionDone(result);
                }
            });
        }).detach();
    }

    MainWindow::MicResult MainWindow::RunMicActionBlocking(MicAction action)
    {
        namespace vm = phomic::virtualmic;

        // Try without admin rights first; Windows may require them for renaming/disabling.
        auto applyOrElevate = []
        {
            HRESULT hr = vm::Apply();
            if (vm::IsAccessDenied(hr))
            {
                DWORD exitCode = 0;
                hr = vm::RunElevatedAndWait(vm::OwnExePath(), L"--setup-microphone", &exitCode);
                if (SUCCEEDED(hr))
                {
                    hr = static_cast<HRESULT>(exitCode);
                }
            }
            return hr;
        };
        MicResult result;
        switch (action)
        {
        case MicAction::Setup:
            result.hr = applyOrElevate();
            break;
        }
        return result;
    }

    void MainWindow::OnMicActionDone(MicResult const& result)
    {
        m_micBusy = false;
        if (FAILED(result.hr) && result.hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            ShowError(L"Couldn't set up the Phomic microphone", std::wstring(hresult_error(result.hr).message()));
        }
        RefreshDevices();
    }

    void MainWindow::StartOutput(std::wstring const& target)
    {
        m_activeTarget = target;
        bool ok = false;
        std::wstring error;
        if (target == kCableTarget)
        {
            m_output->Stop();
            ok = m_cableOutput->Start();
            error = m_cableOutput->LastError();
        }
        else
        {
            m_cableOutput->Stop();
            ok = m_output->Start(target);
            error = m_output->LastError();
        }
        if (ok)
        {
            ShowBar(ErrorBar(), false);
        }
        else
        {
            ShowError(L"Couldn't play to this device", error);
        }
    }

    void MainWindow::ShowError(std::wstring const& title, std::wstring const& message)
    {
        ErrorBar().Title(title);
        ErrorBar().Message(message);
        ShowBar(ErrorBar(), true);
    }

    void MainWindow::ApplyBufferTarget()
    {
        const bool bluetooth = m_arbiter.Owns(phomic::Source::Bluetooth);
        if (kBufferPresets[m_bufferPreset] == 0)
        {
            // Auto: Bluetooth delivers in bursts, so never go below 30 ms there.
            const uint32_t floor = bluetooth ? 3 : 2;
            if (!m_appliedAuto || floor != m_appliedTarget)
            {
                m_appliedAuto = true;
                m_appliedTarget = floor;
                m_jitter->SetAutomatic(floor);
            }
            return;
        }
        uint32_t frames = kBufferPresets[m_bufferPreset];
        if (bluetooth)
        {
            frames = std::max(frames, kMinBluetoothFrames);
        }
        if (m_appliedAuto || frames != m_appliedTarget)
        {
            m_appliedAuto = false;
            m_appliedTarget = frames;
            m_jitter->SetTargetFrames(frames);
        }
    }

    // ---------------------------------------------------------------- periodic UI update

    void MainWindow::OnTick()
    {
        ++m_tick;

        if (m_watcher->TakeChanged())
        {
            RefreshDevices();
        }
        const bool cableActive = m_activeTarget == kCableTarget;
        const bool failed = cableActive ? m_cableOutput->HasFailed() && !m_cableOutput->IsRunning()
                                        : m_output->HasFailed() && !m_output->IsRunning();
        if (failed && !m_activeTarget.empty() && m_tick - m_lastRestartTick > 2 * kTicksPerSecond)
        {
            m_lastRestartTick = m_tick;
            ShowError(L"Audio device problem", cableActive ? m_cableOutput->LastError() : m_output->LastError());
            StartOutput(m_activeTarget);
        }

        m_bluetooth->Tick();
        m_server->TickAdvertisement();
        ApplyBufferTarget();
        if (m_wifiEnabled && m_wifiRetrying && m_tick - m_wifiLastTry >= 3 * kTicksPerSecond)
        {
            StartWiFi();
        }

        const auto wifi = m_server->GetStatus();
        const auto bluetooth = m_bluetooth->GetStatus();
        const bool live = wifi.connected || bluetooth.connected;

        // Level meter: post-gain peak in dBFS, fast attack, slow release.
        float peak = std::max(m_server->TakePeak(), m_bluetooth->TakePeak()) * m_gain.load();
        if (!live)
        {
            peak = 0.f;
        }
        const float db = peak > 1e-5f ? 20.f * std::log10(std::min(peak, 1.f)) : -100.f;
        const float target = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
        m_meter = target > m_meter ? target : std::max(target, m_meter - 0.03f);
        LevelScale().ScaleX(m_meter);
        if (m_tick % 4 == 0)
        {
            LevelText().Text(!live ? std::wstring()
                : db <= -60.f ? std::wstring(L"silent")
                : std::format(L"{:.0f} dB{}", db, peak >= 0.99f ? L" · clipping" : L""));
        }

        UpdateConnection(wifi, bluetooth);
        if (m_tick % 5 == 0)
        {
            UpdateTransports(wifi, bluetooth);
        }
        if (m_tick % kTicksPerSecond == 0)
        {
            UpdateDetails(wifi, bluetooth);
        }
    }

    void MainWindow::UpdateConnection(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth)
    {
        const bool live = wifi.connected || bluetooth.connected;
        const bool muted = wifi.connected ? wifi.muted : bluetooth.muted;
        IdleBadge().Visibility(live ? Visibility::Collapsed : Visibility::Visible);
        LiveBadge().Visibility(live ? Visibility::Visible : Visibility::Collapsed);
        LiveBadge().Opacity(muted ? 0.4 : 1.0);
        DisconnectButton().Visibility(live ? Visibility::Visible : Visibility::Collapsed);

        std::wstring title;
        std::wstring detail;
        if (wifi.connected)
        {
            if (wifi.muted)
            {
                title = std::format(L"{} is muted", wifi.clientName);
                detail = L"Unmute on the iPhone to send audio again.";
            }
            else if (wifi.msSinceAudio > 1500)
            {
                title = std::format(L"Connected to {}", wifi.clientName);
                detail = L"No audio is arriving. Make sure Windows Firewall allows Phomic on private networks.";
            }
            else
            {
                title = std::format(L"Streaming from {}", wifi.clientName);
                detail = L"Wi-Fi · " + wifi.clientAddress;
                if (wifi.rttMs >= 0)
                {
                    detail += std::format(L" · {:.0f} ms round trip", wifi.rttMs);
                }
            }
        }
        else if (bluetooth.connected)
        {
            if (bluetooth.muted)
            {
                title = std::format(L"{} is muted", bluetooth.deviceName);
                detail = L"Unmute on the iPhone to send audio again.";
            }
            else if (bluetooth.msSinceAudio > 1500)
            {
                title = std::format(L"Connected to {}", bluetooth.deviceName);
                detail = L"Connected over Bluetooth, waiting for audio…";
            }
            else
            {
                title = std::format(L"Streaming from {}", bluetooth.deviceName);
                detail = bluetooth.sampleRate
                    ? std::format(L"Bluetooth · {} kHz", bluetooth.sampleRate / 1000)
                    : std::wstring(L"Bluetooth");
            }
        }
        else if (bluetooth.connecting)
        {
            title = L"Connecting over Bluetooth…";
            detail = L"An iPhone is connecting to this PC.";
        }
        else if (!(m_wifiEnabled && wifi.listening) && !(bluetooth.supported && bluetooth.enabled))
        {
            title = L"Not accepting connections";
            detail = m_wifiEnabled && !wifi.error.empty() ? wifi.error : std::wstring(L"Turn on Wi-Fi or Bluetooth under Connection.");
        }
        else
        {
            title = L"Waiting for your iPhone";
            detail = L"Open Phomic on your iPhone, choose Wi-Fi or Bluetooth, and connect.";
            const std::wstring& reason = !wifi.lastDisconnect.empty() ? wifi.lastDisconnect : bluetooth.lastDisconnect;
            if (!reason.empty())
            {
                detail = reason + L" " + detail;
            }
        }

        if (title != m_shownTitle)
        {
            m_shownTitle = title;
            StatusTitle().Text(title);
            if (m_tray)
            {
                m_tray->SetTooltip(L"Phomic · " + title);
            }
        }
        if (detail != m_shownDetail)
        {
            m_shownDetail = detail;
            StatusDetail().Text(detail);
        }
    }

    void MainWindow::UpdateTransports(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth)
    {
        // Wi-Fi row
        std::wstring wifiText;
        if (!m_wifiEnabled)
        {
            wifiText = L"Off";
        }
        else if (!wifi.listening)
        {
            wifiText = m_wifiRetrying && !m_wifiErrorShown ? std::wstring(L"Starting…")
                : wifi.error.empty() ? std::wstring(L"Not available") : wifi.error;
        }
        else if (wifi.connected)
        {
            wifiText = std::format(L"In use · {}", wifi.clientName);
            if (wifi.rttMs >= 0)
            {
                wifiText += std::format(L" · {:.0f} ms round trip", wifi.rttMs);
            }
        }
        else
        {
            wifiText = wifi.advertised
                ? std::format(L"Ready · the iPhone sees this PC as “{}”", m_server->ServiceName())
                : std::wstring(L"Ready · connect manually with an address under Show details");
        }

        // Bluetooth row
        std::wstring bluetoothText;
        if (!bluetooth.supported)
        {
            bluetoothText = bluetooth.problem.empty() ? std::wstring(L"Bluetooth LE isn't available on this PC") : bluetooth.problem;
        }
        else if (!bluetooth.enabled)
        {
            bluetoothText = L"Off";
        }
        else if (bluetooth.connected)
        {
            bluetoothText = std::format(L"In use · {}{}", bluetooth.deviceName,
                bluetooth.sampleRate ? std::format(L" · {} kHz", bluetooth.sampleRate / 1000) : std::wstring());
        }
        else if (bluetooth.connecting)
        {
            bluetoothText = L"An iPhone is connecting…";
        }
        else if (!bluetooth.problem.empty())
        {
            bluetoothText = bluetooth.problem;
        }
        else if (!bluetooth.listening)
        {
            bluetoothText = L"Starting…";
        }
        else
        {
            bluetoothText = std::format(L"Ready · iPhones nearby list this PC as “{}”", m_server->ServiceName());
        }

        if (wifiText != m_shownWiFi)
        {
            m_shownWiFi = wifiText;
            WiFiText().Text(wifiText);
        }
        if (bluetoothText != m_shownBluetooth)
        {
            m_shownBluetooth = bluetoothText;
            BluetoothText().Text(bluetoothText);
        }
        BluetoothToggle().IsEnabled(bluetooth.supported);

        // "Switch here" on the transport that isn't in use, if it can take over.
        const bool switching = m_switchRequestedTick != 0 && m_tick - m_switchRequestedTick < 4 * kTicksPerSecond;
        const bool canSwitchToBluetooth = wifi.connected && bluetooth.supported && bluetooth.enabled;
        const bool canSwitchToWiFi = bluetooth.connected && m_wifiEnabled && wifi.listening;
        SwitchToBluetoothButton().Visibility(canSwitchToBluetooth ? Visibility::Visible : Visibility::Collapsed);
        SwitchToWiFiButton().Visibility(canSwitchToWiFi ? Visibility::Visible : Visibility::Collapsed);
        SwitchToBluetoothButton().IsEnabled(!switching);
        SwitchToWiFiButton().IsEnabled(!switching);
    }

    void MainWindow::UpdateDetails(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth)
    {
        const uint64_t totalPackets = wifi.connected ? wifi.packets : bluetooth.packets;
        const auto stats = m_jitter->GetStats();
        const uint64_t packets = totalPackets >= m_lastPackets ? totalPackets - m_lastPackets : totalPackets;
        const uint64_t received = stats.received >= m_lastReceived ? stats.received - m_lastReceived : stats.received;
        const uint64_t concealed = stats.concealed >= m_lastConcealed ? stats.concealed - m_lastConcealed : stats.concealed;
        m_lastPackets = totalPackets;
        m_lastReceived = stats.received;
        m_lastConcealed = stats.concealed;

        // Stream: only while an iPhone is connected.
        const bool live = wifi.connected || bluetooth.connected;
        StreamSection().Visibility(live ? Visibility::Visible : Visibility::Collapsed);
        if (live)
        {
            std::wstring link;
            std::wstring audio;
            if (wifi.connected)
            {
                link = L"Wi-Fi · " + wifi.clientAddress;
                if (wifi.rttMs >= 0)
                {
                    link += std::format(L" · {:.0f} ms round trip", wifi.rttMs);
                }
                audio = L"48 kHz, uncompressed";
            }
            else
            {
                link = L"Bluetooth LE";
                if (bluetooth.intervalMs > 0)
                {
                    link += std::format(L" · {:g} ms interval", bluetooth.intervalMs);
                }
                audio = bluetooth.sampleRate
                    ? std::format(L"{}, {} kHz", bluetooth.codec, bluetooth.sampleRate / 1000)
                    : std::wstring(L"Waiting for audio");
            }
            const double loss = (received + concealed) > 0 ? 100.0 * concealed / static_cast<double>(received + concealed) : 0.0;
            std::wstring interruptions = L"None";
            if (stats.underruns || stats.skips)
            {
                interruptions = std::format(L"{} rebuffer{} · {} skip{}",
                    stats.underruns, stats.underruns == 1 ? L"" : L"s", stats.skips, stats.skips == 1 ? L"" : L"s");
            }
            SetDetailRows(StreamGrid(), {
                { L"Connection", link },
                { L"Audio", audio },
                { L"Packets", std::format(L"{} per second · {:.1f} % lost", packets, loss) },
                { L"Buffer", std::format(L"{:.0f} ms{}", stats.depthMs, stats.automatic ? L" (Auto)" : L"") },
                { L"Interruptions", interruptions },
            });
        }

        // This PC: what to type on the iPhone for a manual connection, and Bluetooth.
        std::wstring addresses;
        for (auto const& entry : phomic::LocalIPv4Addresses())
        {
            // "192.168.0.10   (Wi-Fi)" -> "192.168.0.10 (Wi-Fi)"
            const size_t gap = entry.find(L"   (");
            addresses += (addresses.empty() ? L"" : L"\n") +
                (gap == std::wstring::npos ? entry : entry.substr(0, gap) + L" " + entry.substr(gap + 3));
        }
        std::wstring bluetoothState;
        if (!bluetooth.supported)
        {
            bluetoothState = L"Not available";
        }
        else if (!bluetooth.enabled)
        {
            bluetoothState = L"Off";
        }
        else if (bluetooth.connected)
        {
            bluetoothState = L"In use";
        }
        else if (!bluetooth.advertisingState.empty())
        {
            bluetoothState = bluetooth.advertisingState;
            bluetoothState[0] = static_cast<wchar_t>(towupper(bluetoothState[0]));
        }
        SetDetailRows(PcGrid(), {
            { L"Name", m_server->ServiceName() },
            { L"Addresses", addresses.empty() ? std::wstring(L"No network") : addresses },
            { L"Port", std::format(L"{} (TCP and UDP)", m_server->Port()) },
            { L"Bluetooth", bluetoothState.empty() ? std::wstring(L"—") : bluetoothState },
            { L"Version", L"Phomic " PHOMIC_VERSION_STRING },
            // Credit for the driver the microphone runs on: the two facts VB-CABLE's readme asks
            // distributors to mention (Phomic doesn't distribute it, so this is a courtesy).
            { L"Mic driver", L"VB-CABLE by VB-Audio (www.vb-cable.com), donationware" },
        });
    }

    void MainWindow::SetDetailRows(Grid const& grid, std::vector<std::pair<std::wstring, std::wstring>> const& rows)
    {
        auto children = grid.Children();
        if (children.Size() != rows.size() * 2)
        {
            children.Clear();
            grid.RowDefinitions().Clear();
            const auto labelStyle = Application::Current().Resources().Lookup(box_value(L"DetailLabelStyle")).as<Style>();
            for (size_t i = 0; i < rows.size(); ++i)
            {
                RowDefinition row;
                row.Height(GridLength{ 0, GridUnitType::Auto });
                grid.RowDefinitions().Append(row);

                TextBlock label;
                label.Style(labelStyle);
                Grid::SetRow(label, static_cast<int32_t>(i));
                TextBlock value;
                value.IsTextSelectionEnabled(true);
                value.TextWrapping(TextWrapping::Wrap);
                Grid::SetRow(value, static_cast<int32_t>(i));
                Grid::SetColumn(value, 1);
                children.Append(label);
                children.Append(value);
            }
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto label = children.GetAt(static_cast<uint32_t>(2 * i)).as<TextBlock>();
            auto value = children.GetAt(static_cast<uint32_t>(2 * i + 1)).as<TextBlock>();
            if (label.Text() != rows[i].first)
            {
                label.Text(rows[i].first);
            }
            if (value.Text() != rows[i].second)
            {
                value.Text(rows[i].second); // unchanged text keeps any selection the user made
            }
        }
    }

    // ---------------------------------------------------------------- event handlers

    void MainWindow::DisconnectButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        m_server->Disconnect();
        m_bluetooth->Disconnect();
    }

    void MainWindow::GainSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        const double db = args.NewValue();
        m_gain = DbToGain(db);
        if (auto slider = sender.try_as<Slider>())
        {
            slider.Header(box_value(hstring(std::format(L"Gain ({:+.0f} dB)", db))));
        }
        if (m_initialized)
        {
            phomic::settings::SetInt(phomic::settings::kGainDb, static_cast<int>(std::lround(db)));
        }
    }

    void MainWindow::OutputCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_initialized || m_updatingDevices)
        {
            return;
        }
        const int index = OutputCombo().SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_targets.size()))
        {
            return;
        }
        phomic::settings::SetString(phomic::settings::kOutputDevice, m_targets[index].id);
        StartOutput(m_targets[index].id);
    }

    void MainWindow::BufferCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        const int index = BufferCombo().SelectedIndex();
        if (index < 0 || index >= kBufferChoices)
        {
            return;
        }
        m_bufferPreset = index;
        ApplyBufferTarget();
        phomic::settings::SetInt(phomic::settings::kBufferMode, index);
    }

    void MainWindow::BluetoothToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        const bool on = BluetoothToggle().IsOn();
        m_bluetooth->SetEnabled(on);
        phomic::settings::SetInt(phomic::settings::kBluetooth, on ? 1 : 0);
        UpdateTransports(m_server->GetStatus(), m_bluetooth->GetStatus());
    }

    void MainWindow::WiFiToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        m_wifiEnabled = WiFiToggle().IsOn();
        phomic::settings::SetInt(phomic::settings::kWiFi, m_wifiEnabled ? 1 : 0);
        if (m_wifiEnabled)
        {
            StartWiFi();
        }
        else
        {
            m_wifiRetrying = false;
            m_server->Stop(); // says "bye" to a connected iPhone
        }
        UpdateTransports(m_server->GetStatus(), m_bluetooth->GetStatus());
    }

    void MainWindow::UpdatePairing()
    {
        const bool required = phomic::pairing::Required();
        PairingText().Text(required
            ? std::wstring(L"Each iPhone asks for it once")
            : std::wstring(L"Any iPhone running Phomic nearby can connect"));
        PairingCodeText().Text(required ? phomic::pairing::Code() : std::wstring());
        PairingCodeText().Visibility(required ? Visibility::Visible : Visibility::Collapsed);
        NewCodeButton().Visibility(required ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::PairingCheck_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        const auto checked = PairingCheck().IsChecked();
        phomic::pairing::SetRequired(checked && checked.Value());
        // Let iPhones see the change (lock icon) before they try to connect.
        m_server->RefreshAdvertisement();
        m_bluetooth->RefreshAdvertisement();
        UpdatePairing();
    }

    void MainWindow::DetailsButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        const bool show = DetailsPanel().Visibility() == Visibility::Collapsed;
        DetailsPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        DetailsButton().Content(box_value(show ? L"Hide details" : L"Show details"));
    }

    void MainWindow::NewCodeButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // Every iPhone that knew the old code has to enter the new one next time it connects.
        phomic::pairing::NewCode();
        UpdatePairing();
    }

    void MainWindow::SwitchToBluetoothButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // The iPhone hangs up Wi-Fi and starts advertising; the Bluetooth scanner then connects.
        m_switchRequestedTick = m_tick;
        m_server->RequestSwitchToBluetooth();
        UpdateTransports(m_server->GetStatus(), m_bluetooth->GetStatus());
    }

    void MainWindow::SwitchToWiFiButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // The iPhone stops Bluetooth and connects to this PC over Wi-Fi.
        m_switchRequestedTick = m_tick;
        m_bluetooth->RequestSwitchToWiFi();
        UpdateTransports(m_server->GetStatus(), m_bluetooth->GetStatus());
    }
}
