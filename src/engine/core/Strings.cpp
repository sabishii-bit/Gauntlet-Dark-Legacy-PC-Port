#include "engine/core/Strings.h"

#include <cctype>

namespace gdl {

std::string normalizeAssetName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) != 0) {
            if (!out.empty()) {
                break;
            }
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(uc)));
    }
    return out;
}

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace gdl
