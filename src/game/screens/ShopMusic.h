#pragma once

#include <string_view>

namespace gdl::game {

/** The tally and shop use the realm just visited, not accumulated completion flags. */
constexpr char shopMusicRealm(std::string_view levelName) {
    const char realm = levelName.empty() ? 'A' : levelName.front();
    // The test realm borrows the province bank. Tower and secret realms use castle.
    if (realm == 'T') {
        return 'G';
    }
    return realm >= 'A' && realm <= 'K' ? realm : 'A';
}

} // namespace gdl::game
