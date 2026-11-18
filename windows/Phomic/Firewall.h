#pragma once

#include <string>

// Windows Firewall and this program: whether iPhones on the PC's current network can reach it
// over Wi-Fi, and the fix when they can't.
//
// Windows asks the first time a program accepts connections. Answering no (or closing the
// question) makes inbound block rules for that program's path on the current kind of network,
// and Windows doesn't ask again. Reading the rules needs no admin rights; changing them does.
namespace phomic::firewall
{
    enum class Verdict
    {
        Open,    // allowed here, the firewall is off here, or Windows hasn't asked yet
        Blocked, // a block rule, or no allow rule where Windows won't ask (again)
        Unknown, // the firewall couldn't be read
    };

    // For the networks the PC is on now. Takes a moment (it goes through every rule): call
    // it off the UI thread. Initializes COM on the thread itself.
    Verdict Check(std::wstring const& exePath);

    // Turns this program's inbound block rules into allow rules, and adds an allow rule for
    // the networks the PC is on now if none covers them. Needs admin rights (see App's
    // --allow-firewall).
    HRESULT Allow(std::wstring const& exePath);
}
