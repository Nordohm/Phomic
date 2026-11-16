#include "pch.h"
#include "Settings.h"

namespace phomic::settings
{
    namespace
    {
        constexpr wchar_t kKey[] = L"Software\\Phomic";
    }

    std::wstring GetString(wchar_t const* name, std::wstring const& fallback)
    {
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        {
            return fallback;
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        {
            return fallback;
        }
        value.resize(wcsnlen(value.c_str(), value.size()));
        return value;
    }

    void SetString(wchar_t const* name, std::wstring const& value)
    {
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_SZ, value.c_str(),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    int GetInt(wchar_t const* name, int fallback)
    {
        DWORD value = 0;
        DWORD bytes = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        {
            return fallback;
        }
        return static_cast<int>(value);
    }

    void SetInt(wchar_t const* name, int value)
    {
        const DWORD raw = static_cast<DWORD>(value);
        RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &raw, sizeof(raw));
    }

    void MigrateFromIMic()
    {
        constexpr wchar_t kLegacyKey[] = L"Software\\iMic";

        HKEY existing{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &existing) == ERROR_SUCCESS)
        {
            RegCloseKey(existing); // already has its own settings
            return;
        }
        HKEY legacy{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyKey, 0, KEY_READ, &legacy) != ERROR_SUCCESS)
        {
            return; // nothing to move
        }
        HKEY target{};
        LSTATUS copied = RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &target, nullptr);
        if (copied == ERROR_SUCCESS)
        {
            copied = RegCopyTreeW(legacy, nullptr, target);
            RegCloseKey(target);
        }
        RegCloseKey(legacy);
        if (copied != ERROR_SUCCESS)
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, kKey); // start clean rather than half-copied
            RegDeleteKeyW(HKEY_CURRENT_USER, kKey);
            return;
        }
        // The saved output target had the old name in it.
        if (GetString(kOutputDevice) == L"imic:cable")
        {
            SetString(kOutputDevice, L"phomic:cable");
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyKey);
        RegDeleteKeyW(HKEY_CURRENT_USER, kLegacyKey);
    }
}
