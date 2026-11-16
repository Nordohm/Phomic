#pragma once

// Turns VB-CABLE (https://vb-cable.com, donationware by VB-Audio) into "the Phomic
// microphone": Phomic plays into the cable's playback end, and other apps record from
// its other end. The driver package itself is shipped unmodified; we only use
// Windows' own per-device settings (the same ones as Settings › Sound › Rename / Don't allow):
//
//   CABLE Output   (recording) -> "Phomic"                         what Discord/Teams/OBS pick
//   CABLE Input    (playback)  -> "Phomic (internal)", disabled    Phomic writes into the driver directly (CableOutput)
//   CABLE In 16 Ch (playback)  -> disabled                       unused second input
// so the only VB-CABLE device anyone sees is the "Phomic" microphone.
namespace phomic::virtualmic
{
    inline constexpr wchar_t kMicName[] = L"Phomic";
    inline constexpr wchar_t kFeedName[] = L"Phomic (internal)";
    inline constexpr wchar_t kDriverLabel[] = L"VB-Audio Virtual Cable";
    inline constexpr wchar_t kWebsite[] = L"https://vb-audio.com/Cable/";

    enum class State
    {
        NotInstalled, // no VB-CABLE endpoints
        NeedsSetup,   // VB-CABLE present but not named/tidied yet
        Ready,
    };

    struct Endpoints
    {
        std::wstring micId;     // recording end
        std::wstring micName;
        std::wstring feedId;    // playback end ("CABLE Input")
        std::wstring feedName;
        bool feedActive = false;
        std::wstring extraId;   // "CABLE In 16ch"
        bool extraActive = false;
    };

    // Finds VB-CABLE's endpoints by driver label, so it works before and after renaming.
    std::optional<Endpoints> Find();
    State GetState(std::optional<Endpoints> const& endpoints);

    // Renames the cable to "Phomic" and disables its playback endpoints. May need admin rights:
    // returns E_ACCESSDENIED in that case (run it again elevated).
    HRESULT Apply();
    // Puts VB-CABLE's original names back and re-enables its playback endpoints.
    HRESULT Restore();

    // This program's own path (to relaunch it elevated).
    std::wstring OwnExePath();

    // Starts `exe` with a UAC prompt and waits for it. Blocking: call off the UI thread.
    // Returns HRESULT_FROM_WIN32(ERROR_CANCELLED) if the user declined the prompt.
    HRESULT RunElevatedAndWait(std::wstring const& exe, std::wstring const& arguments, DWORD* exitCode);

    inline bool IsAccessDenied(HRESULT hr)
    {
        return hr == E_ACCESSDENIED;
    }
}
