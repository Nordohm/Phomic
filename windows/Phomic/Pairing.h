#pragma once

#include "Settings.h"

// Who this PC is, and whether iPhones need a pairing code to connect (Wi-Fi and
// Bluetooth alike). The iPhone asks for the code once per PC and remembers it;
// "New code" makes every iPhone ask again.
namespace phomic::pairing
{
    inline constexpr wchar_t kPcIdSetting[] = L"PcId";
    inline constexpr wchar_t kRequiredSetting[] = L"RequirePairingCode";
    inline constexpr wchar_t kCodeSetting[] = L"PairingCode";

    namespace detail
    {
        inline uint32_t Random()
        {
            static std::mutex lock;
            static std::mt19937 rng{ std::random_device{}() };
            std::lock_guard guard(lock);
            return rng();
        }
    }

    // 8 hex digits, created on first use. Identifies this PC over Bluetooth, where
    // there's no network address, and lets the iPhone find the same PC on either transport.
    inline std::wstring PcId()
    {
        std::wstring id = settings::GetString(kPcIdSetting);
        if (id.size() != 8)
        {
            id = std::format(L"{:08X}", detail::Random());
            settings::SetString(kPcIdSetting, id);
        }
        return id;
    }

    inline uint32_t PcIdValue()
    {
        return static_cast<uint32_t>(std::wcstoul(PcId().c_str(), nullptr, 16));
    }

    inline bool Required()
    {
        return settings::GetInt(kRequiredSetting, 0) != 0;
    }

    inline void SetRequired(bool required)
    {
        settings::SetInt(kRequiredSetting, required ? 1 : 0);
    }

    inline std::wstring NewCode()
    {
        const std::wstring code = std::format(L"{:04}", detail::Random() % 10000);
        settings::SetString(kCodeSetting, code);
        return code;
    }

    inline std::wstring Code()
    {
        const std::wstring code = settings::GetString(kCodeSetting);
        return code.size() == 4 ? code : NewCode();
    }

    // True if a phone presenting `code` may connect.
    inline bool Accepts(std::wstring_view code)
    {
        return !Required() || code == Code();
    }
}
