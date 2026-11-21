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
#include "AppleUsb.h"
#include "AutoGain.h"
#include "Limiter.h"

#include <shellapi.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

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
    constexpr double kWindowWidth = 600; // client area, in epx: fixed, only the height changes
    constexpr int kMinGainDb = -12, kMaxGainDb = 30; // the Gain slider's range
    // Where automatic gain starts the first time. The iPhone's raw microphone is quiet: talking
    // at arm's length peaks around -30 to -20 dBFS. After that it starts where it was last.
    constexpr int kAutoGainStartDb = 18;
    // Output target meaning "write straight into VB-CABLE" (see CableOutput).
    constexpr wchar_t kCableTarget[] = L"phomic:cable";

    float DbToGain(double db)
    {
        return static_cast<float>(std::pow(10.0, db / 20.0));
    }

    // The USB row under Show details: Apple's USB service, and what it and Windows see. The
    // cable hint only where it can help: when neither sees an iPhone.
    std::wstring UsbDetail(phomic::UsbReceiver::Status const& usb)
    {
        const auto iPhones = [](uint32_t count) { return std::format(L"{} iPhone{}", count, count == 1 ? L"" : L"s"); };
        if (!usb.enabled)
        {
            return L"Off";
        }
        if (!usb.checked)
        {
            return L"—";
        }
        if (!usb.serviceRunning)
        {
            return usb.windowsDevices > 0
                ? std::format(L"Apple's USB service isn't running (Windows sees {})", iPhones(usb.windowsDevices))
                : std::wstring(L"Apple's USB service isn't running");
        }
        if (usb.devices != usb.windowsDevices)
        {
            return std::format(L"Apple's USB service sees {}, Windows sees {}", iPhones(usb.devices), usb.windowsDevices);
        }
        if (usb.devices == 0)
        {
            return L"Apple's USB service is running, but no iPhone is connected. Some cables only charge.";
        }
        return std::format(L"Apple's USB service is running and sees {}", iPhones(usb.devices));
    }

    std::wstring GainHeader(int db)
    {
        return db == 0 ? std::wstring(L"Gain (0 dB)") : std::format(L"Gain ({:+d} dB)", db);
    }

    // A closed InfoBar keeps a zero-height slot that still gets the StackPanel's spacing,
    // so hidden banners are also collapsed.
    void ShowBar(InfoBar const& bar, bool open)
    {
        bar.IsOpen(open);
        bar.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
    }

    // "v1.2.0" (a release tag) or "1.2.0" as numbers; nothing if it isn't a version.
    std::optional<std::array<int, 3>> ParseVersion(std::wstring_view text)
    {
        if (!text.empty() && (text.front() == L'v' || text.front() == L'V'))
        {
            text.remove_prefix(1);
        }
        std::array<int, 3> parts{};
        size_t index = 0;
        int value = -1;
        for (const wchar_t c : text)
        {
            if (c >= L'0' && c <= L'9' && value < 10000)
            {
                value = std::max(value, 0) * 10 + (c - L'0');
            }
            else if (c == L'.' && value >= 0 && index < parts.size() - 1)
            {
                parts[index++] = value;
                value = -1;
            }
            else
            {
                return std::nullopt;
            }
        }
        if (value < 0)
        {
            return std::nullopt;
        }
        parts[index] = value;
        return parts;
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return s;
    }

    /// "Windows 11 24H2 (build 26100)", for the feedback form.
    std::wstring WindowsVersion()
    {
        auto read = [](wchar_t const* name)
        {
            wchar_t value[64]{};
            DWORD size = sizeof(value);
            RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                         RRF_RT_REG_SZ, nullptr, value, &size);
            return std::wstring(value);
        };
        const std::wstring display = read(L"DisplayVersion");
        const std::wstring build = read(L"CurrentBuild");
        // Windows 11 still says "10" in most places; its builds start at 22000.
        std::wstring out = _wtoi(build.c_str()) >= 22000 ? L"Windows 11" : L"Windows 10";
        if (!display.empty()) out += L" " + display;
        if (!build.empty()) out += L" (build " + build + L")";
        return out;
    }

    /// A new issue on GitHub with Phomic's feedback form, this app's version and Windows' filled in.
    winrt::Windows::Foundation::Uri FeedbackUri()
    {
        using winrt::Windows::Foundation::Uri;
        std::wstring url = L"https://github.com/Nordohm/Phomic/issues/new?template=feedback.yml";
        url += L"&app=" + std::wstring(Uri::EscapeComponent(L"Windows " PHOMIC_VERSION_STRING));
        url += L"&device=" + std::wstring(Uri::EscapeComponent(WindowsVersion()));
        return Uri(url);
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
        // The title bar is drawn in the window (AppTitleBar); Windows keeps the caption buttons.
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());
        Activated([weak = get_weak()](auto&&, WindowActivatedEventArgs const& args)
        {
            if (auto self = weak.get())
            {
                // Like Windows' own title bar: dimmed while another window is active.
                const bool inactive = args.WindowActivationState() == WindowActivationState::Deactivated;
                self->AppTitleText().Opacity(inactive ? 0.5 : 1.0);
                if (!inactive)
                {
                    self->KeepShape(); // also the first time it's shown
                }
                // Windows gives the first focus to the first button (the App Store badge), with its
                // focus frame when the app was started from the keyboard. Park it on the window
                // itself instead, after Windows has placed it; Tab then starts at the first button.
                if (!inactive && !self->m_firstFocusDone)
                {
                    self->m_firstFocusDone = true;
                    self->DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak]
                    {
                        if (auto self = weak.get())
                        {
                            self->RootGrid().Focus(FocusState::Programmatic);
                        }
                    });
                }
            }
        });
        // Once the first focus has moved on, the window itself is no Tab stop.
        RootGrid().LostFocus([weak = get_weak()](auto&&, RoutedEventArgs const& args)
        {
            if (auto self = weak.get(); self && args.OriginalSource() == self->RootGrid())
            {
                self->RootGrid().IsTabStop(false);
            }
        });
        ResizeForDpi();
        // Follow Windows' light/dark mode in the title bar too.
        AppWindow().TitleBar().PreferredTheme(winrt::Microsoft::UI::Windowing::TitleBarTheme::UseDefaultAppMode);
        SetUpTray();

        namespace settings = phomic::settings;
        m_bufferPreset = std::clamp(settings::GetInt(settings::kBufferMode, kDefaultBufferPreset), 0, kBufferChoices - 1);
        const int gainDb = std::clamp(settings::GetInt(settings::kManualGainDb, 0), kMinGainDb, kMaxGainDb);
        m_autoGainOn = settings::GetInt(settings::kAutoGain, 1) != 0;
        const int autoStartDb = std::clamp(settings::GetInt(settings::kAutoGainDb, kAutoGainStartDb),
                                           static_cast<int>(phomic::AutoGain::kMinDb), static_cast<int>(phomic::AutoGain::kMaxDb));
        m_autoGain = std::make_unique<phomic::AutoGain>(static_cast<float>(autoStartDb));
        m_appliedGainDb = static_cast<float>(autoStartDb);
        m_noiseSuppressionOn = settings::GetInt(settings::kNoiseSuppression, 0) != 0;
        m_denoiser = std::make_unique<phomic::Denoiser>();
        const bool bluetoothEnabled = settings::GetInt(settings::kBluetooth, 1) != 0;
        m_wifiEnabled = settings::GetInt(settings::kWiFi, 1) != 0;
        const bool usbEnabled = settings::GetInt(settings::kUsb, 1) != 0;

        m_jitter = std::make_unique<phomic::JitterBuffer>();
        m_gain = DbToGain(gainDb);
        ApplyBufferTarget();

        // On the output's audio thread. Both outputs share it, but only one of them runs at a time.
        auto source = [this](float* out, uint32_t frames)
        {
            m_jitter->Read(out, frames);
            // Noise suppression first, so automatic gain measures what's left of the background.
            // Switched on, it starts afresh (and adds its 10 ms from then on).
            const bool denoise = m_noiseSuppressionOn.load(std::memory_order_relaxed);
            if (denoise && !m_denoising)
            {
                m_denoiser->Reset();
            }
            m_denoising = denoise;
            if (denoise)
            {
                m_denoiser->Process(out, frames);
            }
            if (m_autoGainOn.load(std::memory_order_relaxed))
            {
                m_autoGain->Process(out, frames);
                m_appliedGainDb.store(m_autoGain->GainDb(), std::memory_order_relaxed);
                m_limiter.Process(out, frames, 1.f);
            }
            else
            {
                m_limiter.Process(out, frames, m_gain.load(std::memory_order_relaxed));
            }
        };
        m_output = std::make_unique<phomic::AudioOutput>(source);
        m_cableOutput = std::make_unique<phomic::CableOutput>(source);
        m_server = std::make_unique<phomic::StreamServer>(*m_jitter, m_arbiter);
        m_bluetooth = std::make_shared<phomic::BluetoothReceiver>(*m_jitter, m_arbiter, m_server->ServiceName());
        m_bluetooth->SetEnabled(bluetoothEnabled);
        m_usb = std::make_unique<phomic::UsbReceiver>(*m_jitter, m_arbiter, m_server->ServiceName());
        m_usb->SetEnabled(usbEnabled);
        m_watcher = std::make_unique<phomic::DeviceWatcher>();

        BufferCombo().SelectedIndex(m_bufferPreset);
        GainSlider().Minimum(kMinGainDb);
        GainSlider().Maximum(kMaxGainDb);
        GainSlider().Value(gainDb);
        GainSlider().Header(box_value(hstring(GainHeader(gainDb))));
        GainMinText().Text(std::format(L"{:+d} dB", kMinGainDb));
        GainMaxText().Text(std::format(L"{:+d} dB", kMaxGainDb));
        AutoGainToggle().IsOn(m_autoGainOn);
        NoiseSuppressionToggle().IsOn(m_noiseSuppressionOn);
        UpdateGain(false);
        BluetoothToggle().IsOn(bluetoothEnabled);
        WiFiToggle().IsOn(m_wifiEnabled);
        UsbToggle().IsOn(usbEnabled);
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
        // Closed: not again for this version, only for the next one.
        UpdateBar().Closed([weak = get_weak()](InfoBar const& sender, auto&&)
        {
            sender.Visibility(Visibility::Collapsed);
            if (auto self = weak.get())
            {
                phomic::settings::SetString(phomic::settings::kSkippedUpdate, self->m_updateVersion);
            }
        });

        FooterText().Text(L" " PHOMIC_VERSION_STRING L" is free and open source. "); // after the "Phomic" link
        FeedbackLink().NavigateUri(FeedbackUri());
        ShowAppStoreBadge();
        RootPanel().ActualThemeChanged([weak = get_weak()](auto&&, auto&&)
        {
            if (auto self = weak.get())
            {
                self->ShowAppStoreBadge();
            }
        });

        m_initialized = true;
        const auto wifi = m_server->GetStatus();
        const auto bluetooth = m_bluetooth->GetStatus();
        const auto usb = m_usb->GetStatus();
        UpdateConnection(wifi, bluetooth, usb);
        UpdateTransports(wifi, bluetooth, usb);
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
        if (m_usb)
        {
            m_usb->Shutdown();
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
        if (m_autoGain)
        {
            // Automatic gain starts from here next time.
            phomic::settings::SetInt(phomic::settings::kAutoGainDb, static_cast<int>(std::lround(m_appliedGainDb.load())));
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
        // The height is a first guess: FitToContent sets it once the content is laid out.
        AppWindow().ResizeClient({ static_cast<int32_t>(kWindowWidth * scale), static_cast<int32_t>(800 * scale) });
        // The content fills the width between the margins. (Stretch would center it by its
        // desired width, which pushes it sideways when nothing in it is that wide.)
        RootScroller().SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& args)
        {
            if (auto self = weak.get())
            {
                const double margins = 24 + 24;
                self->RootPanel().Width(std::max(args.NewSize().Width - margins, 0.0));
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
                if (scale != self->m_badgeScale && self->m_badgeScale != 0)
                {
                    self->ShowAppStoreBadge(); // draw it sharp for the new display
                }
                if (height != self->m_contentHeight || scale != self->m_neededScale)
                {
                    self->m_contentHeight = height;
                    self->FitToContent(height);
                }
            }
        });
        // The width is fixed, but Windows can still change it (it once came back wider, unseen,
        // and stayed so until the content changed): whenever the size changes, put it back.
        AppWindow().Changed([weak = get_weak()](auto&&, winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
        {
            if (auto self = weak.get(); self && (args.DidSizeChange() || args.DidVisibilityChange() || args.DidPresenterChange()))
            {
                self->KeepShape();
            }
        });
    }

    // Only on screen and not minimized: then the window's sizes are its real ones.
    bool MainWindow::SizesAreReal()
    {
        const HWND hwnd = WindowHandle();
        return hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd);
    }

    void MainWindow::KeepShape()
    {
        if (!SizesAreReal())
        {
            return;
        }
        if (m_fitPending && m_contentHeight > 0)
        {
            m_fitPending = false;
            FitToContent(m_contentHeight);
        }
        auto window = AppWindow();
        const int32_t width = static_cast<int32_t>(std::lround(kWindowWidth * GetDpiForWindow(WindowHandle()) / 96.0));
        if (window.ClientSize().Width != width)
        {
            // The size limits are what holds it to that width, so set them again first (from
            // sizes that are real now), then the width.
            if (m_contentHeight > 0)
            {
                FitToContent(m_contentHeight);
            }
            window.ResizeClient({ width, window.ClientSize().Height });
        }
    }

    void MainWindow::FitToContent(double contentHeight)
    {
        // The window is as tall as its content at most (nothing but empty space beyond that),
        // and follows the content as it changes, unless the user made the window shorter.
        if (contentHeight <= 0)
        {
            return;
        }
        if (!SizesAreReal())
        {
            m_fitPending = true; // see KeepShape: when it's back on screen
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

        // The title bar is part of the client area (see AppTitleBar), and Windows keeps the top
        // of it (a 1 px border) from the content: count both, or the content scrolls by that.
        double inset = 0;
        if (const auto root = Content().try_as<FrameworkElement>(); root && root.ActualHeight() > 0)
        {
            inset = std::clamp(client.Height / scale - root.ActualHeight(), 0.0, 8.0);
        }
        int32_t needed = static_cast<int32_t>(std::ceil((contentHeight + AppTitleBar().Height() + inset) * scale));
        namespace windowing = winrt::Microsoft::UI::Windowing;
        const auto area = windowing::DisplayArea::GetFromWindowId(window.Id(), windowing::DisplayAreaFallback::Nearest).WorkArea();
        const int32_t frameWidth = window.Size().Width - client.Width;
        const int32_t frameHeight = window.Size().Height - client.Height; // the border
        needed = std::min(needed, area.Height - frameHeight);

        // Size limits (outer window size, physical pixels): a fixed width, a height up to the
        // content's.
        if (auto presenter = window.Presenter().try_as<windowing::OverlappedPresenter>())
        {
            constexpr double kMinHeight = 360; // client area, in epx
            const int32_t width = static_cast<int32_t>(std::lround(kWindowWidth * scale)) + frameWidth;
            presenter.IsMaximizable(false);
            presenter.PreferredMinimumWidth(width);
            presenter.PreferredMaximumWidth(width);
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
        // The window's icons (taskbar, Alt+Tab) are the squircle; the tray and the title bar (drawn
        // in the window, see AppTitleBar) show the glyph alone. The taskbar uses the small icon,
        // the title bar's, so a Windows-drawn title bar can't differ from it.
        const HINSTANCE module = GetModuleHandleW(nullptr);
        m_appIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_PHOMIC), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        if (m_appIcon)
        {
            AppWindow().SetIcon(winrt::Microsoft::UI::GetIconIdFromIcon(m_appIcon));
        }
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(WindowHandle()));
        m_trayIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_PHOMIC_GLYPH), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
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
        CheckFirewall();
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

    namespace
    {
        // A stream over one of the exe's RCDATA resources.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IRandomAccessStream> ResourceStream(int id)
        {
            const HMODULE module = GetModuleHandleW(nullptr);
            const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
            const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
            const auto data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
            winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            if (data)
            {
                winrt::Windows::Storage::Streams::DataWriter writer(stream);
                writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + SizeofResource(module, resource)));
                co_await writer.StoreAsync();
                writer.DetachStream();
                stream.Seek(0);
            }
            co_return stream;
        }
    }

    // Apple's badge for the current theme: black in light mode, white in dark mode, drawn at
    // the display's pixel size so its small print stays sharp.
    winrt::fire_and_forget MainWindow::ShowAppStoreBadge()
    {
        auto weak = get_weak();
        const bool dark = RootPanel().ActualTheme() == ElementTheme::Dark;
        const HWND hwnd = WindowHandle();
        m_badgeScale = (hwnd ? GetDpiForWindow(hwnd) : 96) / 96.0;
        auto stream = co_await ResourceStream(dark ? IDR_BADGE_WHITE : IDR_BADGE_BLACK);
        if (stream.Size() == 0)
        {
            co_return;
        }
        if (auto self = weak.get())
        {
            Media::Imaging::SvgImageSource badge;
            badge.RasterizePixelHeight(self->AppStoreBadge().Height() * self->m_badgeScale);
            self->AppStoreBadge().Source(badge);
            co_await badge.SetSourceAsync(stream);
        }
    }

    // The app goes on the iPhone, so the useful thing on a PC is a code to scan with it.
    winrt::fire_and_forget MainWindow::ShowIPhoneAppCode()
    {
        if (m_showingAppCode)
        {
            co_return; // e.g. a double click
        }
        m_showingAppCode = true;
        auto weak = get_weak();
        Media::Imaging::BitmapImage code;
        Image image;
        image.Width(240);
        image.Height(240);
        image.Source(code);

        TextBlock text;
        text.Text(L"Scan the code with your iPhone's camera to open Phomic on the App Store.");
        text.TextWrapping(TextWrapping::Wrap);
        text.TextAlignment(TextAlignment::Center);
        StackPanel panel;
        panel.Spacing(16);
        panel.MaxWidth(320);
        panel.Children().Append(image);
        panel.Children().Append(text);

        ContentDialog dialog;
        dialog.XamlRoot(Content().XamlRoot());
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Style>());
        dialog.RequestedTheme(RootPanel().ActualTheme());
        dialog.Title(box_value(L"Phomic for iPhone"));
        dialog.Content(panel);
        dialog.PrimaryButtonText(L"Open in browser");
        dialog.CloseButtonText(L"Close");
        dialog.DefaultButton(ContentDialogButton::Close);

        auto stream = co_await ResourceStream(IDR_APPSTORE_QR);
        if (stream.Size() != 0)
        {
            co_await code.SetSourceAsync(stream);
        }
        if (!weak.get())
        {
            co_return;
        }
        ContentDialogResult result = ContentDialogResult::None;
        try
        {
            result = co_await dialog.ShowAsync();
        }
        catch (winrt::hresult_error const&)
        {
            // Another dialog (e.g. Windows' own) is open: nothing to show now.
        }
        if (auto self = weak.get())
        {
            self->m_showingAppCode = false;
        }
        if (result == ContentDialogResult::Primary)
        {
            ShellExecuteW(nullptr, L"open", L"https://apps.apple.com/app/phomic/id6835323092", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // ---------------------------------------------------------------- updates

    // Asks GitHub for the latest release of Phomic, and shows UpdateBar if it's newer than this
    // one. The request names only Phomic and its version. Offline, or GitHub says no: nothing
    // happens, and the next check tries again. PHOMIC_UPDATE_URL points it elsewhere (tests).
    winrt::fire_and_forget MainWindow::CheckForUpdate()
    {
        auto weak = get_weak();
        std::wstring source = L"https://api.github.com/repos/Nordohm/Phomic/releases/latest";
        wchar_t custom[512]{};
        const DWORD length = GetEnvironmentVariableW(L"PHOMIC_UPDATE_URL", custom, ARRAYSIZE(custom));
        if (length > 0 && length < ARRAYSIZE(custom))
        {
            source = custom;
        }

        std::wstring tag, page, installer, digest;
        try
        {
            namespace http = winrt::Windows::Web::Http;
            http::Filters::HttpBaseProtocolFilter filter; // always ask, never an old cached answer
            filter.CacheControl().ReadBehavior(http::Filters::HttpCacheReadBehavior::NoCache);
            http::HttpClient client(filter);
            client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Phomic/" PHOMIC_VERSION_STRING);
            client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
            const auto body = co_await client.GetStringAsync(winrt::Windows::Foundation::Uri(source));
            const auto release = winrt::Windows::Data::Json::JsonObject::Parse(body);
            tag = release.GetNamedString(L"tag_name", L"");
            page = release.GetNamedString(L"html_url", L"");
            const auto assets = release.GetNamedArray(L"assets", winrt::Windows::Data::Json::JsonArray());
            for (uint32_t i = 0; i < assets.Size(); ++i)
            {
                const auto asset = assets.GetObjectAt(i);
                const std::wstring name{ asset.GetNamedString(L"name", L"") };
                if (name.starts_with(L"Phomic-Setup-") && name.ends_with(L".exe"))
                {
                    installer = asset.GetNamedString(L"browser_download_url", L"");
                    digest = asset.GetNamedString(L"digest", L""); // "sha256:<hex>"
                }
            }
        }
        catch (...)
        {
            co_return;
        }

        auto self = weak.get();
        const auto latest = ParseVersion(tag);
        if (!self || !latest)
        {
            co_return;
        }
        const std::array<int, 3> current{ PHOMIC_VERSION_MAJOR, PHOMIC_VERSION_MINOR, PHOMIC_VERSION_PATCH };
        const std::wstring version = std::format(L"{}.{}.{}", (*latest)[0], (*latest)[1], (*latest)[2]);
        // Only ever open Phomic's own release files and pages, whatever the reply says.
        constexpr std::wstring_view kReleases = L"https://github.com/Nordohm/Phomic/releases/";
        const std::wstring url = installer.starts_with(kReleases) ? installer
                               : page.starts_with(kReleases) ? page
                               : std::wstring(kReleases);
        if (*latest <= current || version == phomic::settings::GetString(phomic::settings::kSkippedUpdate))
        {
            co_return;
        }
        self->m_updateVersion = version;
        self->m_updateUrl = url;
        // What Update downloads and runs: only Phomic's own installer, with GitHub's checksum.
        self->m_updateInstaller = installer.starts_with(kReleases) ? installer : std::wstring();
        self->m_updateDigest = digest.starts_with(L"sha256:") ? digest.substr(7) : std::wstring();
        std::transform(self->m_updateDigest.begin(), self->m_updateDigest.end(), self->m_updateDigest.begin(), towlower);
        self->UpdateBar().Title(std::format(L"Phomic {} is available", version));
        self->UpdateButton().Content(box_value(self->CanInstallUpdate() ? L"Update" : L"Download"));
        ShowBar(self->UpdateBar(), true);
    }

    void MainWindow::UpdateButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (CanInstallUpdate())
        {
            InstallUpdate();
        }
        else if (!m_updateUrl.empty())
        {
            ShellExecuteW(nullptr, L"open", m_updateUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // The installer Update downloaded (see InstallUpdate) stays in the temp folder: it's still
    // running when it starts the new Phomic. Remove it the next time, when it's long done.
    void MainWindow::RemoveDownloadedInstallers()
    {
        if (m_updating)
        {
            return;
        }
        wchar_t temp[MAX_PATH]{};
        if (GetTempPathW(ARRAYSIZE(temp), temp) == 0)
        {
            return;
        }
        const std::wstring folder = temp;
        WIN32_FIND_DATAW found{};
        const HANDLE search = FindFirstFileW((folder + L"Phomic-Setup-*.exe").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            DeleteFileW((folder + found.cFileName).c_str()); // one still in use stays until next time
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }

    // Set up by the installer (its uninstaller sits next to Phomic.exe), so the installer can
    // update it in place. A portable copy gets the download page instead.
    bool MainWindow::CanInstallUpdate() const
    {
        if (m_updateInstaller.empty() || m_updateDigest.size() != 64)
        {
            return false;
        }
        std::wstring folder = phomic::virtualmic::OwnExePath();
        folder.resize(folder.find_last_of(L'\\') + 1);
        return GetFileAttributesW((folder + L"unins000.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    // Downloads the new installer, checks it against GitHub's SHA-256 for it, and runs it
    // silently (a small progress window only); Phomic quits, and the installer starts it again
    // (/RESTARTPHOMIC=1). PHOMIC_UPDATE_DRY_RUN stops before running it (for testing).
    winrt::fire_and_forget MainWindow::InstallUpdate()
    {
        if (m_updating)
        {
            co_return;
        }
        m_updating = true;
        const std::wstring url = m_updateInstaller;
        const std::wstring digest = m_updateDigest;
        const std::wstring version = m_updateVersion;
        UpdateButton().IsEnabled(false);
        UpdateBar().IsClosable(false);
        UpdateBar().Title(std::format(L"Downloading Phomic {}…", version));

        auto weak = get_weak();
        winrt::apartment_context ui;
        std::wstring error;
        std::wstring path;
        try
        {
            namespace http = winrt::Windows::Web::Http;
            http::Filters::HttpBaseProtocolFilter filter;
            filter.CacheControl().ReadBehavior(http::Filters::HttpCacheReadBehavior::NoCache);
            http::HttpClient client(filter);
            client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Phomic/" PHOMIC_VERSION_STRING);
            auto download = client.GetBufferAsync(winrt::Windows::Foundation::Uri(url));
            auto shown = std::make_shared<std::atomic<int>>(-1);
            download.Progress([weak, shown, version, queue = DispatcherQueue()](auto&&, http::HttpProgress const& progress)
            {
                const auto total = progress.TotalBytesToReceive;
                if (!total || total.Value() == 0)
                {
                    return;
                }
                const int percent = static_cast<int>(std::min<uint64_t>(100, progress.BytesReceived * 100 / total.Value()));
                if (shown->exchange(percent) != percent)
                {
                    queue.TryEnqueue([weak, percent, version]
                    {
                        if (auto self = weak.get())
                        {
                            self->UpdateBar().Title(std::format(L"Downloading Phomic {}… {} %", version, percent));
                        }
                    });
                }
            });
            const auto buffer = co_await download;

            namespace crypto = winrt::Windows::Security::Cryptography;
            const auto sha256 = crypto::Core::HashAlgorithmProvider::OpenAlgorithm(crypto::Core::HashAlgorithmNames::Sha256());
            std::wstring hash{ crypto::CryptographicBuffer::EncodeToHexString(sha256.HashData(buffer)) };
            std::transform(hash.begin(), hash.end(), hash.begin(), towlower);
            if (hash != digest)
            {
                error = L"The download doesn't match the release.";
            }
            else
            {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(ARRAYSIZE(temp), temp);
                path = std::format(L"{}Phomic-Setup-{}.exe", temp, version);
                const winrt::handle file{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
                DWORD written = 0;
                if (file.get() == INVALID_HANDLE_VALUE || !WriteFile(file.get(), buffer.data(), buffer.Length(), &written, nullptr) ||
                    written != buffer.Length())
                {
                    error = L"Couldn't save the download.";
                }
            }
        }
        catch (winrt::hresult_error const& e)
        {
            error = std::wstring(e.message());
        }
        co_await ui;
        auto self = weak.get();
        if (!self)
        {
            co_return;
        }

        if (error.empty())
        {
            if (GetEnvironmentVariableW(L"PHOMIC_UPDATE_DRY_RUN", nullptr, 0) > 0)
            {
                self->UpdateBar().Title(std::format(L"Phomic {} downloaded and checked (dry run: not installed)", version));
                co_return;
            }
            SHELLEXECUTEINFOW run{ sizeof(run) };
            run.lpVerb = L"open";
            run.lpFile = path.c_str();
            run.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /RESTARTPHOMIC=1";
            run.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&run))
            {
                self->UpdateBar().Title(std::format(L"Installing Phomic {}…", version));
                self->Quit(); // the installer starts the new one
                co_return;
            }
            error = std::wstring(hresult_error(HRESULT_FROM_WIN32(GetLastError())).message());
        }

        // Didn't work: say why, and offer the download page instead.
        self->m_updating = false;
        self->m_updateInstaller.clear();
        self->UpdateBar().Title(std::format(L"Phomic {} is available", version));
        self->UpdateButton().Content(box_value(L"Download"));
        self->UpdateButton().IsEnabled(true);
        self->UpdateBar().IsClosable(true);
        self->ShowError(L"Couldn't update Phomic", error);
    }

    void MainWindow::GetIPhoneAppLink_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ShowIPhoneAppCode();
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

        // A newer version? Shortly after starting, then twice a day: Phomic can sit in the tray
        // for weeks.
        if (m_tick % (12 * 3600 * kTicksPerSecond) == 15 * kTicksPerSecond)
        {
            RemoveDownloadedInstallers();
            CheckForUpdate();
        }

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
        const auto usb = m_usb->GetStatus();
        const bool live = wifi.connected || bluetooth.connected || usb.connected;

        // Level meter: post-gain peak in dBFS (the limiter keeps it under its ceiling), fast
        // attack, slow release. "Clipping": the iPhone's own input is at full scale.
        float received = std::max({ m_server->TakePeak(), m_bluetooth->TakePeak(), m_usb->TakePeak() });
        if (!live)
        {
            received = 0.f;
        }
        const float gained = received * (m_autoGainOn ? DbToGain(m_appliedGainDb.load()) : m_gain.load());
        const float peak = std::min(gained, phomic::Limiter::kCeiling);
        const float db = peak > 1e-5f ? 20.f * std::log10(peak) : -100.f;
        const float target = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
        m_meter = target > m_meter ? target : std::max(target, m_meter - 0.03f);
        LevelScale().ScaleX(m_meter);
        if (m_tick % 4 == 0)
        {
            LevelText().Text(!live ? std::wstring()
                : db <= -60.f ? std::wstring(L"silent")
                : std::format(L"{:.0f} dB{}", db, received >= 0.99f ? L" · clipping"
                                                : gained > phomic::Limiter::kCeiling ? L" · limiting" : L""));
            UpdateGain(live);
        }

        UpdateConnection(wifi, bluetooth, usb);
        // Windows Firewall, every 10 s while it can matter and someone can see the answer: it
        // also notices a fix made in Windows' own settings.
        if (m_tick % (10 * kTicksPerSecond) == 2 * kTicksPerSecond && m_wifiEnabled && wifi.listening &&
            !wifi.connected && AppWindow().IsVisible())
        {
            CheckFirewall();
        }
        if (m_tick % 5 == 0)
        {
            UpdateTransports(wifi, bluetooth, usb);
        }
        if (m_tick % kTicksPerSecond == 0)
        {
            MeasureStream(wifi, bluetooth, usb);
            // Building the rows (and looking up this PC's addresses) only while they're on screen.
            if (DetailsPanel().Visibility() == Visibility::Visible && AppWindow().IsVisible())
            {
                UpdateDetails(wifi, bluetooth, usb);
            }
        }
    }

    // Packets and loss over the last second, for Show details.
    void MainWindow::MeasureStream(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
        phomic::UsbReceiver::Status const& usb)
    {
        const uint64_t totalPackets = wifi.connected ? wifi.packets : usb.connected ? usb.packets : bluetooth.packets;
        const auto stats = m_jitter->GetStats();
        const uint64_t received = stats.received >= m_lastReceived ? stats.received - m_lastReceived : stats.received;
        const uint64_t concealed = stats.concealed >= m_lastConcealed ? stats.concealed - m_lastConcealed : stats.concealed;
        m_packetsPerSecond = totalPackets >= m_lastPackets ? totalPackets - m_lastPackets : totalPackets;
        m_lossPercent = (received + concealed) > 0 ? 100.0 * concealed / static_cast<double>(received + concealed) : 0.0;
        m_lastPackets = totalPackets;
        m_lastReceived = stats.received;
        m_lastConcealed = stats.concealed;
    }

    // Each transport keeps the reason its own last session ended; the status card shows the
    // newest one, whichever transport it came from.
    void MainWindow::TrackLastDisconnect(phomic::StreamServer::Status const& wifi,
        phomic::BluetoothReceiver::Status const& bluetooth, phomic::UsbReceiver::Status const& usb)
    {
        std::wstring const* reasons[] = { &wifi.lastDisconnect, &bluetooth.lastDisconnect, &usb.lastDisconnect };
        for (size_t i = 0; i < std::size(reasons); ++i)
        {
            if (*reasons[i] != m_seenDisconnect[i])
            {
                m_seenDisconnect[i] = *reasons[i];
                if (!reasons[i]->empty())
                {
                    m_lastDisconnect = *reasons[i];
                }
            }
        }
    }

    void MainWindow::UpdateConnection(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
        phomic::UsbReceiver::Status const& usb)
    {
        TrackLastDisconnect(wifi, bluetooth, usb);
        const bool live = wifi.connected || bluetooth.connected || usb.connected;
        const bool muted = wifi.connected ? wifi.muted : usb.connected ? usb.muted : bluetooth.muted;
        IdleBadge().Visibility(live ? Visibility::Collapsed : Visibility::Visible);
        LiveBadge().Visibility(live ? Visibility::Visible : Visibility::Collapsed);
        LiveBadge().Opacity(muted ? 0.4 : 1.0);
        // Over USB the cable decides: there's nothing to disconnect.
        DisconnectButton().Visibility(live && !usb.connected ? Visibility::Visible : Visibility::Collapsed);
        GetIPhoneAppLink().Visibility(live ? Visibility::Collapsed : Visibility::Visible);

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
        else if (usb.connected)
        {
            if (usb.muted)
            {
                title = std::format(L"{} is muted", usb.deviceName);
                detail = L"Unmute on the iPhone to send audio again.";
            }
            else if (usb.msSinceAudio > 1500)
            {
                title = std::format(L"Connected to {}", usb.deviceName);
                detail = L"Connected over USB, waiting for audio…";
            }
            else
            {
                title = std::format(L"Streaming from {}", usb.deviceName);
                detail = L"USB cable";
                if (usb.rttMs >= 0)
                {
                    detail += std::format(L" · {:.0f} ms round trip", usb.rttMs);
                }
            }
        }
        else if (bluetooth.connecting)
        {
            title = L"Connecting over Bluetooth…";
            detail = L"An iPhone is connecting to this PC.";
        }
        else if (!(m_wifiEnabled && wifi.listening) && !(bluetooth.supported && bluetooth.enabled) &&
                 !(usb.enabled && usb.serviceRunning))
        {
            title = L"Not accepting connections";
            detail = m_wifiEnabled && !wifi.error.empty() ? wifi.error : std::wstring(L"Turn on Wi-Fi, Bluetooth or USB under Connection.");
        }
        else
        {
            title = L"Waiting for your iPhone";
            // Why the last session ended, if one did; else what to do.
            detail = m_lastDisconnect.empty() ? std::wstring(L"Open Phomic on your iPhone to connect.") : m_lastDisconnect;
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

    void MainWindow::UpdateTransports(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
        phomic::UsbReceiver::Status const& usb)
    {
        // Wi-Fi row
        std::wstring wifiText;
        const bool firewallBlocks = m_wifiEnabled && wifi.listening && !wifi.connected &&
                                    m_firewall == phomic::firewall::Verdict::Blocked;
        if (!m_wifiEnabled)
        {
            wifiText = L"Off";
        }
        else if (!wifi.listening)
        {
            wifiText = m_wifiRetrying && !m_wifiErrorShown ? std::wstring(L"Starting…")
                : wifi.error.empty() ? std::wstring(L"Not available") : wifi.error;
        }
        else if (firewallBlocks)
        {
            wifiText = L"Windows Firewall blocks iPhones from connecting";
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

        // USB row: its state, or what's wrong with Apple's USB service and a button that fixes it.
        std::wstring usbText;
        UsbAction usbAction = UsbAction::None;
        std::wstring usbCommand;
        using phomic::UsbIssue;
        if (!usb.enabled)
        {
            usbText = L"Off";
        }
        else if (usb.connected)
        {
            usbText = std::format(L"In use · {}", usb.deviceName);
        }
        else if (!usb.checked || (!usb.serviceRunning && usb.issue == UsbIssue::None))
        {
            usbText = L"Starting…";
        }
        else if (usb.issue == UsbIssue::NoAppleDevices)
        {
            usbText = L"Needs Apple Devices, Apple's free app that lets Windows talk to an iPhone over a cable";
            usbAction = UsbAction::GetAppleDevices;
        }
        else if (usb.issue == UsbIssue::StartingService)
        {
            usbText = L"Waiting for Apple's USB service…";
        }
        else if (usb.issue == UsbIssue::ServiceNotRunning)
        {
            usbText = L"Apple's USB service didn't start. Opening Apple Devices starts it; you can close the window again.";
            usbAction = UsbAction::OpenAppleDevices;
        }
        else if (usb.issue == UsbIssue::StartupDisabled)
        {
            usbText = L"Apple's USB service isn't running: Apple Devices is off under Settings › Apps › Startup. Start it "
                      L"here, and turn it on there so it starts with Windows.";
            // If starting it quietly didn't work, the window does.
            usbAction = m_usbStartTick != 0 && m_tick - m_usbStartTick > 12 * kTicksPerSecond
                ? UsbAction::OpenAppleDevices : UsbAction::StartService;
        }
        else if (usb.issue == UsbIssue::PortForward)
        {
            usbText = std::format(L"A Windows port forward on {}:{} blocks Apple's USB service. Remove it from a terminal run as "
                L"administrator, then open Apple Devices.", usb.forwardAddress, phomic::kUsbmuxPort);
            usbAction = UsbAction::CopyCommand;
            usbCommand = std::format(L"netsh interface portproxy delete v4tov4 listenaddress={} listenport={}",
                usb.forwardAddress, phomic::kUsbmuxPort);
        }
        else if (usb.issue == UsbIssue::PortTaken)
        {
            usbText = std::format(L"{} uses port {}, which Apple's USB service needs. Close it, then open Apple Devices.",
                usb.portHolder.empty() ? std::wstring(L"Another program") : usb.portHolder, phomic::kUsbmuxPort);
        }
        else if (usb.issue == UsbIssue::ClassicServiceStopped)
        {
            usbText = L"Apple Mobile Device Service is stopped. Start it under Services, or install Apple Devices instead.";
            usbAction = UsbAction::GetAppleDevices;
        }
        else if (usb.issue == UsbIssue::DeviceUnreachable)
        {
            usbText = L"Apple's USB service can't reach the iPhone. Unplug it and plug it back in, unlocked. "
                      L"If that doesn't help, restart the iPhone.";
        }
        else if (usb.issue == UsbIssue::NotSeenByApple)
        {
            usbText = L"Apple's USB service has lost track of the iPhone. Unplug the cable, wait 5 seconds, and plug it back in with the iPhone unlocked.";
        }
        else if (usb.devices > 0)
        {
            usbText = L"iPhone plugged in · pick USB in Phomic on it";
        }
        else
        {
            usbText = L"Ready · plug your iPhone into this PC with a cable";
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
        if (usbText != m_shownUsb)
        {
            m_shownUsb = usbText;
            UsbText().Text(usbText);
        }
        BluetoothToggle().IsEnabled(bluetooth.supported);
        if (usb.issue != UsbIssue::StartupDisabled && usb.issue != UsbIssue::StartingService)
        {
            m_usbStartTick = 0; // a later episode starts with the quiet start again
        }
        m_usbAction = usbAction;
        m_usbCommand = usbCommand;
        const bool copied = m_usbCopiedTick != 0 && m_tick - m_usbCopiedTick < 3 * kTicksPerSecond;
        const std::wstring buttonText = usbAction == UsbAction::GetAppleDevices ? L"Get Apple Devices"
            : usbAction == UsbAction::StartService ? L"Start it"
            : usbAction == UsbAction::OpenAppleDevices ? L"Open Apple Devices"
            : copied ? L"Copied" : L"Copy command";
        const std::wstring buttonTip = usbAction == UsbAction::GetAppleDevices ? std::wstring(L"Free, from Apple, in the Microsoft Store")
            : usbAction == UsbAction::CopyCommand ? usbCommand : std::wstring(L"Starts Apple's USB service");
        if (buttonText != m_shownUsbButton || buttonTip != m_shownUsbTip)
        {
            m_shownUsbButton = buttonText;
            m_shownUsbTip = buttonTip;
            UsbActionButton().Content(box_value(buttonText));
            ToolTipService::SetToolTip(UsbActionButton(), box_value(buttonTip));
        }
        UsbActionButton().Visibility(usbAction != UsbAction::None ? Visibility::Visible : Visibility::Collapsed);

        // "Switch" on the transports that aren't in use, if they can take over.
        const bool switching = m_switchRequestedTick != 0 && m_tick - m_switchRequestedTick < 4 * kTicksPerSecond;
        const bool canSwitchToBluetooth = (wifi.connected || usb.connected) && bluetooth.supported && bluetooth.enabled;
        const bool canSwitchToWiFi = (bluetooth.connected || usb.connected) && m_wifiEnabled && wifi.listening && !firewallBlocks;
        const bool canSwitchToUsb = (wifi.connected || bluetooth.connected) && usb.enabled && usb.serviceRunning && usb.devices > 0;
        SwitchToBluetoothButton().Visibility(canSwitchToBluetooth ? Visibility::Visible : Visibility::Collapsed);
        SwitchToWiFiButton().Visibility(canSwitchToWiFi ? Visibility::Visible : Visibility::Collapsed);
        SwitchToUsbButton().Visibility(canSwitchToUsb ? Visibility::Visible : Visibility::Collapsed);
        WiFiAllowButton().Visibility(firewallBlocks ? Visibility::Visible : Visibility::Collapsed);
        WiFiAllowButton().IsEnabled(!m_firewallFixing);

        // Mark the row in use (the same rows that say "In use").
        const std::array<bool, 3> inUse{ wifi.connected, bluetooth.connected, usb.connected };
        if (inUse != m_shownInUse)
        {
            m_shownInUse = inUse;
            const auto show = [](bool on) { return on ? Visibility::Visible : Visibility::Collapsed; };
            WiFiHighlight().Visibility(show(inUse[0]));
            WiFiMark().Visibility(show(inUse[0]));
            WiFiActiveIcon().Visibility(show(inUse[0]));
            WiFiIcon().Visibility(show(!inUse[0]));
            BluetoothHighlight().Visibility(show(inUse[1]));
            BluetoothMark().Visibility(show(inUse[1]));
            BluetoothActiveIcon().Visibility(show(inUse[1]));
            BluetoothIcon().Visibility(show(!inUse[1]));
            UsbHighlight().Visibility(show(inUse[2]));
            UsbMark().Visibility(show(inUse[2]));
            UsbActiveIcon().Visibility(show(inUse[2]));
            UsbIcon().Visibility(show(!inUse[2]));
        }
        SwitchToBluetoothButton().IsEnabled(!switching);
        SwitchToWiFiButton().IsEnabled(!switching);
        SwitchToUsbButton().IsEnabled(!switching);
    }

    void MainWindow::UpdateDetails(phomic::StreamServer::Status const& wifi, phomic::BluetoothReceiver::Status const& bluetooth,
        phomic::UsbReceiver::Status const& usb)
    {
        const auto stats = m_jitter->GetStats();

        // Stream: only while an iPhone is connected.
        const bool live = wifi.connected || bluetooth.connected || usb.connected;
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
            else if (usb.connected)
            {
                link = L"USB cable";
                if (usb.rttMs >= 0)
                {
                    link += std::format(L" · {:.0f} ms round trip", usb.rttMs);
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
            std::wstring interruptions = L"None";
            if (stats.underruns || stats.skips)
            {
                interruptions = std::format(L"{} rebuffer{} · {} skip{}",
                    stats.underruns, stats.underruns == 1 ? L"" : L"s", stats.skips, stats.skips == 1 ? L"" : L"s");
            }
            SetDetailRows(StreamGrid(), {
                { L"Connection", link },
                { L"Audio", audio },
                { L"Packets", std::format(L"{} per second · {:.1f} % lost", m_packetsPerSecond, m_lossPercent) },
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
            { L"USB", UsbDetail(usb) },
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
        m_usb->Disconnect();
    }

    void MainWindow::GainSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        const int db = static_cast<int>(std::lround(args.NewValue()));
        m_gain = DbToGain(db);
        if (auto slider = sender.try_as<Slider>())
        {
            slider.Header(box_value(hstring(GainHeader(db))));
        }
        if (m_initialized)
        {
            phomic::settings::SetInt(phomic::settings::kManualGainDb, db);
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

    void MainWindow::AutoGainToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        m_autoGainOn = AutoGainToggle().IsOn();
        phomic::settings::SetInt(phomic::settings::kAutoGain, m_autoGainOn ? 1 : 0);
        UpdateGain(false);
    }

    void MainWindow::NoiseSuppressionToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        m_noiseSuppressionOn = NoiseSuppressionToggle().IsOn();
        phomic::settings::SetInt(phomic::settings::kNoiseSuppression, m_noiseSuppressionOn ? 1 : 0);
    }

    // Automatic gain on: where it is now (while live). Off: the slider to set it by hand.
    void MainWindow::UpdateGain(bool live)
    {
        const bool automatic = m_autoGainOn.load();
        ManualGainPanel().Visibility(automatic ? Visibility::Collapsed : Visibility::Visible);
        std::wstring text;
        if (automatic && live)
        {
            const int db = static_cast<int>(std::lround(m_appliedGainDb.load()));
            text = db == 0 ? std::wstring(L"Now 0 dB") : std::format(L"Now {:+d} dB", db);
        }
        if (GainText().Text() != text)
        {
            GainText().Text(text);
        }
        GainText().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
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
        RefreshTransports();
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
        RefreshTransports();
    }

    void MainWindow::UpdatePairing()
    {
        const bool required = phomic::pairing::Required();
        PairingText().Text(required
            ? std::wstring(L"Each iPhone asks for it once. Not needed over USB: the cable is enough.")
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
        if (show)
        {
            UpdateDetails(m_server->GetStatus(), m_bluetooth->GetStatus(), m_usb->GetStatus()); // filled before it shows
        }
        DetailsPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        DetailsButton().Content(box_value(show ? L"Hide details" : L"Show details"));
    }

    void MainWindow::NewCodeButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // Every iPhone that knew the old code has to enter the new one next time it connects.
        phomic::pairing::NewCode();
        UpdatePairing();
    }

    void MainWindow::RefreshTransports()
    {
        UpdateTransports(m_server->GetStatus(), m_bluetooth->GetStatus(), m_usb->GetStatus());
    }

    // Asks the connected iPhone, over whichever transport it uses, to move to `to`.
    void MainWindow::RequestSwitch(phomic::Source to)
    {
        m_switchRequestedTick = m_tick;
        m_server->RequestSwitch(to);
        m_bluetooth->RequestSwitch(to);
        m_usb->RequestSwitch(to);
        RefreshTransports();
    }

    void MainWindow::SwitchToBluetoothButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // The iPhone hangs up and starts advertising; the Bluetooth scanner then connects.
        RequestSwitch(phomic::Source::Bluetooth);
    }

    void MainWindow::SwitchToWiFiButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // The iPhone hangs up and connects to this PC over Wi-Fi.
        RequestSwitch(phomic::Source::WiFi);
    }

    void MainWindow::SwitchToUsbButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // The iPhone hangs up and listens on USB; the USB receiver then connects through the cable.
        RequestSwitch(phomic::Source::Usb);
    }

    void MainWindow::UsbToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_initialized)
        {
            return;
        }
        const bool on = UsbToggle().IsOn();
        m_usb->SetEnabled(on);
        phomic::settings::SetInt(phomic::settings::kUsb, on ? 1 : 0);
        RefreshTransports();
    }

    void MainWindow::UsbActionButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        switch (m_usbAction)
        {
        case UsbAction::GetAppleDevices:
            phomic::appleusb::OpenStorePage();
            break;
        case UsbAction::StartService:
            m_usb->StartAppleService();
            m_usbStartTick = m_tick;
            RefreshTransports();
            break;
        case UsbAction::OpenAppleDevices:
            // Starting the app starts its USB service; the receiver finds it within a few seconds.
            phomic::appleusb::OpenAppleDevices();
            break;
        case UsbAction::CopyCommand:
        {
            winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(m_usbCommand);
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            m_usbCopiedTick = m_tick;
            RefreshTransports();
            break;
        }
        default:
            break;
        }
    }

    // Reads Windows Firewall's rules for this program off the UI thread. PHOMIC_FIREWALL_EXE
    // checks another path instead (for testing against an installed copy's rules).
    winrt::fire_and_forget MainWindow::CheckFirewall()
    {
        if (m_firewallChecking)
        {
            co_return;
        }
        m_firewallChecking = true;
        std::wstring exe = phomic::virtualmic::OwnExePath();
        wchar_t custom[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"PHOMIC_FIREWALL_EXE", custom, ARRAYSIZE(custom));
        if (length > 0 && length < ARRAYSIZE(custom))
        {
            exe = custom;
        }
        auto weak = get_weak();
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        const auto verdict = phomic::firewall::Check(exe);
        co_await ui;
        if (auto self = weak.get())
        {
            self->m_firewallChecking = false;
            if (verdict != phomic::firewall::Verdict::Unknown && verdict != self->m_firewall)
            {
                self->m_firewall = verdict;
                self->RefreshTransports();
            }
        }
    }

    void MainWindow::WiFiAllowButton_Click(winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_firewallFixing)
        {
            return;
        }
        m_firewallFixing = true;
        RefreshTransports();
        // Changing firewall rules needs admin rights: Phomic again, elevated (App's --allow-firewall).
        // The UAC prompt blocks, so wait for it off the UI thread.
        auto dispatcher = DispatcherQueue();
        std::thread([dispatcher, weak = get_weak()]
        {
            const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            DWORD exitCode = 0;
            HRESULT hr = phomic::virtualmic::RunElevatedAndWait(phomic::virtualmic::OwnExePath(), L"--allow-firewall", &exitCode);
            if (SUCCEEDED(hr))
            {
                hr = static_cast<HRESULT>(exitCode);
            }
            if (SUCCEEDED(coInit))
            {
                CoUninitialize();
            }
            dispatcher.TryEnqueue([weak, hr]
            {
                if (auto self = weak.get())
                {
                    self->m_firewallFixing = false;
                    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                    {
                        self->ShowError(L"Couldn't change Windows Firewall", std::wstring(hresult_error(hr).message()));
                    }
                    self->RefreshTransports();
                    self->CheckFirewall();
                }
            });
        }).detach();
    }
}
