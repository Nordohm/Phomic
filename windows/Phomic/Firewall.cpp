#include "pch.h"
#include "Firewall.h"

#include <netfw.h>
#include <oleauto.h>

namespace phomic::firewall
{
    namespace
    {
        constexpr long kProfiles[] = { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC };
        constexpr long kAllProfiles = NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC;
        constexpr wchar_t kRuleName[] = L"Phomic";

        // COM for this call. On a thread that already has another apartment (the UI thread
        // in the elevated helper) that one is used as it is.
        struct ComScope
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ~ComScope()
            {
                if (SUCCEEDED(hr))
                {
                    CoUninitialize();
                }
            }
        };

        struct Bstr
        {
            BSTR value = nullptr;
            Bstr() = default;
            explicit Bstr(wchar_t const* text) : value(SysAllocString(text)) {}
            Bstr(Bstr const&) = delete;
            Bstr& operator=(Bstr const&) = delete;
            ~Bstr() { SysFreeString(value); }
        };

        // Rules may hold environment variables (%ProgramFiles%\…) and any case.
        std::wstring Normalized(std::wstring_view path)
        {
            std::wstring text(path);
            if (text.find(L'%') != std::wstring::npos)
            {
                wchar_t expanded[MAX_PATH * 2];
                const DWORD length = ExpandEnvironmentStringsW(text.c_str(), expanded, ARRAYSIZE(expanded));
                if (length > 0 && length <= ARRAYSIZE(expanded))
                {
                    text = expanded;
                }
            }
            CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
            return text;
        }

        winrt::com_ptr<INetFwPolicy2> OpenPolicy(HRESULT& hr)
        {
            winrt::com_ptr<INetFwPolicy2> policy;
            hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(policy.put()));
            return SUCCEEDED(hr) ? policy : nullptr;
        }

        // Calls visit(rule) for each inbound rule for this program, on or off.
        template <typename Visit>
        HRESULT ForEachRule(INetFwPolicy2* policy, std::wstring const& exePath, Visit&& visit)
        {
            winrt::com_ptr<INetFwRules> rules;
            HRESULT hr = policy->get_Rules(rules.put());
            if (FAILED(hr))
            {
                return hr;
            }
            winrt::com_ptr<IUnknown> enumerator;
            hr = rules->get__NewEnum(enumerator.put());
            if (FAILED(hr))
            {
                return hr;
            }
            const auto items = enumerator.try_as<IEnumVARIANT>();
            if (!items)
            {
                return E_NOINTERFACE;
            }
            const std::wstring wanted = Normalized(exePath);
            VARIANT item;
            VariantInit(&item);
            ULONG fetched = 0;
            while (items->Next(1, &item, &fetched) == S_OK && fetched == 1)
            {
                winrt::com_ptr<INetFwRule> rule;
                if (item.vt == VT_DISPATCH && item.pdispVal)
                {
                    item.pdispVal->QueryInterface(IID_PPV_ARGS(rule.put()));
                }
                VariantClear(&item);
                NET_FW_RULE_DIRECTION direction{};
                Bstr application;
                if (!rule || FAILED(rule->get_Direction(&direction)) || direction != NET_FW_RULE_DIR_IN ||
                    FAILED(rule->get_ApplicationName(&application.value)) || !application.value ||
                    Normalized(application.value) != wanted)
                {
                    continue;
                }
                visit(rule.get());
            }
            return S_OK;
        }

        // Which networks this program's rules allow or block it on, per protocol.
        struct Coverage
        {
            long allowTcp = 0;
            long allowUdp = 0;
            long block = 0;
            bool asked = false; // there's a rule at all, so Windows has asked about this program

            void Add(INetFwRule* rule)
            {
                asked = true;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                NET_FW_ACTION action{};
                long profiles = 0;
                long protocol = 0;
                if (FAILED(rule->get_Enabled(&enabled)) || !enabled || FAILED(rule->get_Action(&action)) ||
                    FAILED(rule->get_Profiles(&profiles)) || FAILED(rule->get_Protocol(&protocol)))
                {
                    return;
                }
                const bool tcp = protocol == NET_FW_IP_PROTOCOL_TCP || protocol == NET_FW_IP_PROTOCOL_ANY;
                const bool udp = protocol == NET_FW_IP_PROTOCOL_UDP || protocol == NET_FW_IP_PROTOCOL_ANY;
                if (action == NET_FW_ACTION_BLOCK && (tcp || udp))
                {
                    block |= profiles;
                }
                else if (action == NET_FW_ACTION_ALLOW)
                {
                    allowTcp |= tcp ? profiles : 0;
                    allowUdp |= udp ? profiles : 0;
                }
            }

            bool Allows(long profiles) const
            {
                return (allowTcp & profiles) == profiles && (allowUdp & profiles) == profiles;
            }
        };
    }

    Verdict Check(std::wstring const& exePath)
    {
        ComScope com;
        HRESULT hr = S_OK;
        const auto policy = OpenPolicy(hr);
        long current = 0;
        if (!policy || FAILED(policy->get_CurrentProfileTypes(&current)))
        {
            return Verdict::Unknown;
        }
        Coverage coverage;
        if (FAILED(ForEachRule(policy.get(), exePath, [&](INetFwRule* rule) { coverage.Add(rule); })))
        {
            return Verdict::Unknown;
        }

        for (const long profile : kProfiles)
        {
            const auto type = static_cast<NET_FW_PROFILE_TYPE2>(profile);
            VARIANT_BOOL on = VARIANT_FALSE;
            if (!(current & profile) || FAILED(policy->get_FirewallEnabled(type, &on)) || !on)
            {
                continue;
            }
            VARIANT_BOOL blockAll = VARIANT_FALSE;
            VARIANT_BOOL quiet = VARIANT_FALSE;
            NET_FW_ACTION inbound = NET_FW_ACTION_BLOCK;
            policy->get_BlockAllInboundTraffic(type, &blockAll);
            policy->get_NotificationsDisabled(type, &quiet);
            policy->get_DefaultInboundAction(type, &inbound);
            if (blockAll || (coverage.block & profile))
            {
                return Verdict::Blocked;
            }
            // Not allowed here: Windows asks the first time, unless it asked before (about
            // another kind of network) or doesn't ask at all.
            if (!coverage.Allows(profile) && inbound == NET_FW_ACTION_BLOCK && (coverage.asked || quiet))
            {
                return Verdict::Blocked;
            }
        }
        return Verdict::Open;
    }

    HRESULT Allow(std::wstring const& exePath)
    {
        ComScope com;
        HRESULT hr = S_OK;
        const auto policy = OpenPolicy(hr);
        if (!policy)
        {
            return hr;
        }
        long current = 0;
        hr = policy->get_CurrentProfileTypes(&current);
        if (FAILED(hr))
        {
            return hr;
        }
        current &= kAllProfiles;
        if (current == 0)
        {
            current = NET_FW_PROFILE2_PRIVATE;
        }

        // Block rules become allow rules (Windows' own "Allow an app" does the same). An
        // earlier rule of ours is reused rather than adding another.
        Coverage coverage;
        winrt::com_ptr<INetFwRule> ours;
        HRESULT changed = S_OK;
        hr = ForEachRule(policy.get(), exePath, [&](INetFwRule* rule)
        {
            NET_FW_ACTION action{};
            if (SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_BLOCK)
            {
                if (const HRESULT set = rule->put_Action(NET_FW_ACTION_ALLOW); FAILED(set))
                {
                    changed = set;
                }
            }
            Bstr name;
            long protocol = 0;
            if (SUCCEEDED(rule->get_Name(&name.value)) && name.value && wcscmp(name.value, kRuleName) == 0 &&
                SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_ANY)
            {
                ours.copy_from(rule);
            }
            coverage.Add(rule);
        });
        if (FAILED(hr))
        {
            return hr;
        }
        if (FAILED(changed))
        {
            return changed; // E_ACCESSDENIED without admin rights
        }
        if (coverage.Allows(current))
        {
            return S_OK;
        }

        if (ours)
        {
            long profiles = 0;
            ours->get_Profiles(&profiles);
            hr = ours->put_Action(NET_FW_ACTION_ALLOW);
            if (SUCCEEDED(hr))
            {
                hr = ours->put_Profiles((profiles & kAllProfiles) | current);
            }
            if (SUCCEEDED(hr))
            {
                hr = ours->put_Enabled(VARIANT_TRUE);
            }
            return hr;
        }

        winrt::com_ptr<INetFwRule> rule;
        hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(rule.put()));
        if (FAILED(hr))
        {
            return hr;
        }
        const Bstr name(kRuleName);
        const Bstr description(L"Lets iPhones connect to Phomic over Wi-Fi.");
        const Bstr application(exePath.c_str());
        for (const HRESULT step : {
                 rule->put_Name(name.value),
                 rule->put_Description(description.value),
                 rule->put_ApplicationName(application.value),
                 rule->put_Protocol(NET_FW_IP_PROTOCOL_ANY),
                 rule->put_Direction(NET_FW_RULE_DIR_IN),
                 rule->put_Action(NET_FW_ACTION_ALLOW),
                 rule->put_Profiles(current),
                 rule->put_Enabled(VARIANT_TRUE),
             })
        {
            if (FAILED(step))
            {
                return step;
            }
        }
        winrt::com_ptr<INetFwRules> rules;
        hr = policy->get_Rules(rules.put());
        return SUCCEEDED(hr) ? rules->Add(rule.get()) : hr;
    }
}
