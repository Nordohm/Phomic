#include "pch.h"
#include "TrayIcon.h"

#include <shellapi.h>
#include <windowsx.h>

namespace phomic
{
    namespace
    {
        constexpr wchar_t kWindowClass[] = L"PhomicTrayWindow";
        constexpr UINT kIconId = 1;
        constexpr UINT kCallbackMessage = WM_APP + 1;
        constexpr UINT kShowMessage = WM_APP + 2;   // sent by a second copy of Phomic
        constexpr UINT kQuitMessage = WM_APP + 3;   // sent by Phomic --quit (the uninstaller)
        constexpr UINT_PTR kOpenCommand = 1;
        constexpr UINT_PTR kQuitCommand = 2;

        void RegisterWindowClass(WNDPROC proc)
        {
            static bool registered = false;
            if (registered)
            {
                return;
            }
            WNDCLASSEXW windowClass{ sizeof(windowClass) };
            windowClass.lpfnWndProc = proc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kWindowClass;
            registered = RegisterClassExW(&windowClass) != 0;
        }
    }

    TrayIcon::TrayIcon(HICON icon, std::function<void(Command)> onCommand)
        : m_icon(icon), m_onCommand(std::move(onCommand))
    {
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        RegisterWindowClass(&TrayIcon::WindowProc);
        // A hidden top-level window (not message-only: those miss the TaskbarCreated broadcast).
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Phomic tray", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        Add();
    }

    TrayIcon::~TrayIcon()
    {
        if (m_window)
        {
            NOTIFYICONDATAW data{ sizeof(data) };
            data.hWnd = m_window;
            data.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &data);
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            DestroyWindow(m_window);
        }
    }

    void TrayIcon::Add()
    {
        if (!m_window)
        {
            return;
        }
        NOTIFYICONDATAW data{ sizeof(data) };
        data.hWnd = m_window;
        data.uID = kIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kCallbackMessage;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, m_tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_DELETE, &data); // in case Explorer kept a stale one
        if (Shell_NotifyIconW(NIM_ADD, &data))
        {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    }

    void TrayIcon::SetTooltip(std::wstring const& text)
    {
        if (text == m_tooltip || !m_window)
        {
            return;
        }
        m_tooltip = text;
        NOTIFYICONDATAW data{ sizeof(data) };
        data.hWnd = m_window;
        data.uID = kIconId;
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcsncpy_s(data.szTip, m_tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    bool TrayIcon::ShowExisting()
    {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (!existing)
        {
            return false;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(existing, &processId);
        AllowSetForegroundWindow(processId); // we were just started by the user, so we may pass this on
        PostMessageW(existing, kShowMessage, 0, 0);
        return true;
    }

    bool TrayIcon::QuitExisting(std::wstring const& exePath)
    {
        // Collect first: a window that closes would end FindWindowExW's walk.
        std::vector<HWND> windows;
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, kWindowClass, nullptr)) != nullptr;)
        {
            windows.push_back(window);
        }
        bool allGone = true;
        for (const HWND window : windows)
        {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == 0 || processId == GetCurrentProcessId())
            {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processId);
            if (!process)
            {
                process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
            }
            if (!process)
            {
                continue;
            }
            wchar_t path[MAX_PATH * 2];
            DWORD length = ARRAYSIZE(path);
            const bool ours = QueryFullProcessImageNameW(process, 0, path, &length) &&
                CompareStringOrdinal(path, static_cast<int>(length), exePath.c_str(), static_cast<int>(exePath.size()), TRUE) == CSTR_EQUAL;
            if (ours)
            {
                PostMessageW(window, kQuitMessage, 0, 0);
                if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
                {
                    TerminateProcess(process, 1);
                    allGone = WaitForSingleObject(process, 2000) == WAIT_OBJECT_0 && allGone;
                }
            }
            CloseHandle(process);
        }
        return allGone;
    }

    LRESULT CALLBACK TrayIcon::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto self = static_cast<TrayIcon*>(create->lpCreateParams);
            self->m_window = window; // Handle needs it before CreateWindowExW returns
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (auto self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA)))
        {
            return self->Handle(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT TrayIcon::Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kCallbackMessage)
        {
            switch (LOWORD(lParam))
            {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                m_onCommand(Command::Open);
                break;
            case WM_CONTEXTMENU:
                ShowMenu({ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
                break;
            default:
                break;
            }
            return 0;
        }
        if (message == kShowMessage)
        {
            m_onCommand(Command::Open);
            return 0;
        }
        if (message == kQuitMessage)
        {
            m_onCommand(Command::Quit);
            return 0;
        }
        // Windows is ending the session, or an installer asks Phomic to close (Restart
        // Manager): quit for real. The window alone would only close (or go to the tray), the
        // process would stay, and an installer would wait until it gave up and ended it.
        if (message == WM_QUERYENDSESSION)
        {
            return TRUE;
        }
        if (message == WM_ENDSESSION)
        {
            if (wParam)
            {
                m_onCommand(Command::Quit);
            }
            return 0;
        }
        if (message == m_taskbarCreated && m_taskbarCreated != 0)
        {
            Add(); // Explorer restarted
            return 0;
        }
        return DefWindowProcW(m_window, message, wParam, lParam);
    }

    void TrayIcon::ShowMenu(POINT at)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }
        AppendMenuW(menu, MF_STRING, kOpenCommand, L"Open");
        SetMenuDefaultItem(menu, static_cast<UINT>(kOpenCommand), FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit");

        // Needed so the menu closes when clicking elsewhere.
        SetForegroundWindow(m_window);
        UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
        flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, at.x, at.y, m_window, nullptr));
        PostMessageW(m_window, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (command == kOpenCommand)
        {
            m_onCommand(Command::Open);
        }
        else if (command == kQuitCommand)
        {
            m_onCommand(Command::Quit);
        }
    }
}
