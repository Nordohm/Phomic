#pragma once

namespace phomic
{
    // The Phomic icon in the notification area ("tray"), with Open / Quit on right-click.
    //
    // It owns a small hidden window that receives the icon's clicks, re-adds the icon
    // when Explorer restarts, and lets a second copy of Phomic bring this one forward
    // (see ShowExisting). Create and use it on the UI thread.
    class TrayIcon
    {
    public:
        enum class Command { Open, Quit };

        TrayIcon(HICON icon, std::function<void(Command)> onCommand);
        ~TrayIcon();
        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;

        void SetTooltip(std::wstring const& text);

        // From a second copy of Phomic: ask the running one to show its window.
        // False if no running copy has a tray icon.
        static bool ShowExisting();

        // For the uninstaller: ask a running Phomic started from `exePath` to quit (like Quit in
        // the tray menu), and end it if it hasn't within 5 s. Copies elsewhere are left alone.
        // False if one is still running.
        static bool QuitExisting(std::wstring const& exePath);

    private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
        void Add();
        void ShowMenu(POINT at);

        HWND m_window = nullptr;
        HICON m_icon = nullptr;
        std::function<void(Command)> m_onCommand;
        std::wstring m_tooltip = L"Phomic";
        UINT m_taskbarCreated = 0;
    };
}
