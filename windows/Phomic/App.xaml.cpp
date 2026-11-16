#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "VirtualMic.h"
#include "Settings.h"
#include "Startup.h"
#include "TrayIcon.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Phomic::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    App::~App()
    {
        if (m_instanceMutex)
        {
            CloseHandle(m_instanceMutex);
        }
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Helper mode: the window relaunches Phomic elevated with one of these when Windows
        // wants admin rights to rename/disable the VB-CABLE endpoints. The exit code is the HRESULT.
        const std::wstring_view commandLine = GetCommandLineW();
        if (commandLine.find(L"--setup-microphone") != std::wstring_view::npos)
        {
            ExitProcess(static_cast<UINT>(phomic::virtualmic::Apply()));
        }
        if (commandLine.find(L"--restore-microphone") != std::wstring_view::npos)
        {
            ExitProcess(static_cast<UINT>(phomic::virtualmic::Restore()));
        }

        // Only one copy can own the network port; bring the existing window forward instead.
        m_instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\PhomicHostSingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            // Its window may be hidden in the tray: ask it to show itself.
            if (!phomic::TrayIcon::ShowExisting())
            {
                if (HWND existing = FindWindowW(nullptr, L"Phomic"))
                {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                }
            }
            Exit();
            return;
        }

        // Phomic used to be called iMic: take over its settings and sign-in entry.
        phomic::settings::MigrateFromIMic();
        phomic::startup::MigrateFromIMic();

        window = make<MainWindow>();
        // Started with Windows: stay in the tray until the user opens the window.
        if (commandLine.find(phomic::startup::kTrayArgument) == std::wstring_view::npos)
        {
            window.Activate();
        }
    }
}
