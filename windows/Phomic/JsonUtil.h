#pragma once

namespace phomic
{
    // Escapes a UTF-8 string for use inside a JSON string literal.
    inline std::string JsonEscape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    inline std::string JsonEscape(std::wstring_view s)
    {
        return JsonEscape(std::string_view(winrt::to_string(s)));
    }
}
