#include "game/console/PartyCountRequest.h"

#include <array>
#include <string_view>
#include <utility>

#include "game/console/ConsoleArguments.h"
#include "game/console/ConsoleUsage.h"

namespace gdl::game {

namespace {
constexpr s32 kMostPartyCount = 4;
} // namespace

std::string partyCountUsage() {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kRows{
        {{"party_count <1-4>", "sets the level up for a party that size"},
         {"party_count off", "follows the players present again"},
         {"party_count", "says which it is"}}};
    return consoleUsage(kRows);
}

gdl::Expected<PartyCountRequest, std::string> parsePartyCount(ConsoleCommand::Arguments arguments) {
    if (arguments.size() != 1) {
        return gdl::Unexpected(partyCountUsage());
    }
    const std::string word = lowerArgument(arguments[0]);
    if (word == "off" || word == "auto") {
        return PartyCountRequest{.mode = PartyCountRequest::Mode::Players};
    }
    const auto count = integerArgument(word);
    if (!count.has_value() || *count < 1 || *count > kMostPartyCount) {
        return gdl::Unexpected(partyCountUsage());
    }
    return PartyCountRequest{.mode = PartyCountRequest::Mode::Fixed, .players = *count};
}

} // namespace gdl::game
