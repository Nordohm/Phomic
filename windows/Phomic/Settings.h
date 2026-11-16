#pragma once

// Small persisted preferences (HKCU\Software\Phomic). The app is unpackaged, so
// Windows.Storage.ApplicationData is not available.
namespace phomic::settings
{
    std::wstring GetString(wchar_t const* name, std::wstring const& fallback = {});
    void SetString(wchar_t const* name, std::wstring const& value);

    int GetInt(wchar_t const* name, int fallback);
    void SetInt(wchar_t const* name, int value);

    // Phomic used to be called iMic: moves HKCU\Software\iMic here once, if Phomic has no
    // settings yet (pairing code, PC id and preferences stay the same).
    void MigrateFromIMic();

    inline constexpr wchar_t kOutputDevice[] = L"OutputDevice";
    inline constexpr wchar_t kGainDb[] = L"GainDb";
    inline constexpr wchar_t kBufferMode[] = L"BufferMode"; // 0 = Auto, 1-3 = 30/60/120 ms
    inline constexpr wchar_t kBluetooth[] = L"Bluetooth";
    inline constexpr wchar_t kWiFi[] = L"WiFi";
    inline constexpr wchar_t kCloseToTray[] = L"CloseToTray";
}
