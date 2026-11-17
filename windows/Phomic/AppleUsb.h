#pragma once

// What Windows says about Apple's USB service (usbmux), for telling the user what's wrong
// when USB doesn't work. None of it needs admin rights.
//
// The service comes with the Apple Devices app from the Microsoft Store, which starts it
// when the user signs in (a startup task), or with the older Apple Mobile Device Support
// (from iTunes), which runs it as a Windows service. Either listens on 127.0.0.1:27015.
namespace phomic::appleusb
{
    // The Apple Devices app is installed for this user.
    bool AppleDevicesInstalled();

    // Whether Apple Devices may start its USB service at sign-in (Settings > Apps > Startup).
    enum class Startup { Unknown, Enabled, Disabled };
    Startup AppleDevicesStartup();

    // Apple Mobile Device Service, from Apple Mobile Device Support.
    enum class ClassicService { Missing, Running, Stopped, Disabled };
    ClassicService ClassicServiceState();

    // Who listens on a local TCP port, where that blocks 127.0.0.1.
    struct PortHolder
    {
        bool held = false;
        std::wstring process;        // e.g. "svchost.exe"; empty if Windows won't say
        bool apple = false;          // one of Apple's own device services
        std::wstring forwardAddress; // a "netsh interface portproxy" rule listens there
    };
    PortHolder WhoHoldsPort(uint16_t port);

    // iPhones (and iPads) Windows sees on USB right now.
    uint32_t AppleDevicesPluggedIn();

    // Starts Apple Devices' USB service without opening its window: activates the background
    // agent in its package (a COM server), which starts the service the way the Apple Devices
    // window does. Undocumented, so OpenAppleDevices() is the fallback. Needs COM on the thread.
    bool StartAppleService();

    // Opens the Apple Devices app (which starts its USB service), or its Microsoft Store page.
    void OpenAppleDevices();
    void OpenStorePage();
}
