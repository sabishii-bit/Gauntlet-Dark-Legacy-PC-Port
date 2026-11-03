#include "game/console/ConsoleListings.h"

#include <algorithm>

#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleUsage.h"
#include "game/console/DevConsole.h"

namespace gdl::game {

namespace {
/** `list`: the first word names what, the rest is the listing's own. */
class ListCommand final : public ConsoleCommand {
public:
    explicit ListCommand(const ConsoleListings& listings) : m_listings(listings) {}

    std::string_view name() const override { return "list"; }
    std::string summary() const override { return "maps, items or inventory: what there is"; }
    ConsoleReach reach() const override { return ConsoleReach::Local; }
    void run(DevConsole& console, Arguments arguments) override {
        for (const std::string& line : m_listings.lines(arguments)) {
            console.print(line);
        }
    }
    std::vector<std::string> completions(Arguments arguments) const override {
        return arguments.size() <= 1 ? m_listings.words() : std::vector<std::string>{};
    }

private:
    const ConsoleListings& m_listings;
};
} // namespace

void ConsoleListings::add(std::unique_ptr<ConsoleListing> listing) {
    if (listing != nullptr) {
        m_listings.push_back(std::move(listing));
    }
}

std::vector<std::string> ConsoleListings::words() const {
    std::vector<std::string> words;
    for (const auto& listing : m_listings) {
        if (std::ranges::none_of(words, [&](const std::string& word) {
                return sameNameIgnoringCase(word, listing->word());
            })) {
            words.emplace_back(listing->word());
        }
    }
    return words;
}

std::string ConsoleListings::usage() const {
    std::vector<std::string_view> worded;
    std::vector<ConsoleListing::Row> rows;
    for (const auto& listing : m_listings) {
        std::optional<ConsoleListing::Row> row = listing->usageRow();
        if (!row.has_value() || std::ranges::any_of(worded, [&](std::string_view word) {
                return sameNameIgnoringCase(word, listing->word());
            })) {
            continue;
        }
        worded.push_back(listing->word());
        rows.push_back(std::move(*row));
    }
    const std::vector<std::pair<std::string_view, std::string_view>> views(rows.begin(),
                                                                           rows.end());
    return consoleUsage(views);
}

std::vector<std::string> ConsoleListings::lines(Arguments arguments) const {
    std::vector<std::string> lines;
    bool answered = false;
    if (!arguments.empty()) {
        for (const auto& listing : m_listings) {
            if (!sameNameIgnoringCase(listing->word(), arguments.front())) {
                continue;
            }
            answered = true;
            const std::vector<std::string> said = listing->lines(arguments.subspan(1));
            lines.insert(lines.end(), said.begin(), said.end());
        }
    }
    if (!answered) {
        lines.push_back(usage());
    }
    return lines;
}

std::unique_ptr<ConsoleCommand> listCommand(const ConsoleListings& listings) {
    return std::make_unique<ListCommand>(listings);
}

} // namespace gdl::game
