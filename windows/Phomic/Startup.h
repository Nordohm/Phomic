#pragma once

#include <cwctype>

// "Start with Windows": a per-user Run entry that starts Phomic in the tray. No admin
// rights needed. Task Manager's Startup apps page can turn it off too; that choice is
// stored under StartupApproved, which IsEnabled respects.
namespace phomic::startup
{
    inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    inline constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    inline constexpr wchar_t kValueName[] = L"Phomic";
    // Command-line switch: start with the window hidden in the tray.
    inline constexpr wchar_t kTrayArgument[] = L"--tray";

    inline std::wstring ExePath()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    inline std::wstring RunCommand()
    {
        return L"\"" + ExePath() + L"\" " + kTrayArgument;
    }

    inline std::wstring CurrentEntry()
    {
        wchar_t value[2048]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        {
            return {};
        }
        return value;
    }

    inline void WriteEntry()
    {
        const std::wstring command = RunCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    inline bool IsEnabled()
    {
        if (CurrentEntry().empty())
        {
            return false;
        }
        BYTE approved[12]{};
        DWORD size = sizeof(approved);
        if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, approved, &size) == ERROR_SUCCESS &&
            size > 0 && (approved[0] & 1) != 0)
        {
            return false; // turned off in Task Manager › Startup apps
        }
        return true;
    }

    inline void SetEnabled(bool enabled)
    {
        if (enabled)
        {
            WriteEntry();
        }
        else
        {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        }
        // Forget an earlier on/off choice from Task Manager, so this one counts.
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName);
    }

    // Phomic used to be called iMic: replaces iMic's sign-in entry with Phomic's, keeping it
    // on or off (also when it was turned off in Task Manager).
    inline void MigrateFromIMic()
    {
        constexpr wchar_t kLegacyName[] = L"iMic";
        wchar_t value[2048]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kLegacyName, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        {
            return;
        }
        std::wstring command = value;
        for (auto& c : command)
        {
            c = static_cast<wchar_t>(towlower(c));
        }
        if (command.find(L"imic.exe") == std::wstring::npos || command.find(kTrayArgument) == std::wstring::npos)
        {
            return; // not the entry iMic made
        }
        BYTE approved[12]{};
        DWORD approvedSize = sizeof(approved);
        const bool turnedOff =
            RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kLegacyName, RRF_RT_REG_BINARY, nullptr, approved, &approvedSize) == ERROR_SUCCESS &&
            approvedSize > 0 && (approved[0] & 1) != 0;
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kLegacyName);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kLegacyName);
        if (!turnedOff && CurrentEntry().empty())
        {
            WriteEntry();
        }
    }

    // If Phomic's folder was moved, point the entry at this copy.
    inline void Repair()
    {
        const std::wstring entry = CurrentEntry();
        if (entry.size() < 2 || entry[0] != L'"')
        {
            return;
        }
        const size_t end = entry.find(L'"', 1);
        if (end == std::wstring::npos)
        {
            return;
        }
        const std::wstring path = entry.substr(1, end - 1);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            WriteEntry();
        }
    }
}
