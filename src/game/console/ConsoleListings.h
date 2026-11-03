#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/console/ConsoleCommand.h"

namespace gdl::game {

/** One thing `list` names: the word that asks for it, its line in list's usage and what it says. */
class ConsoleListing {
public:
    using Arguments = ConsoleCommand::Arguments;
    /** A form of `list` and what it names, as the usage sets them side by side. */
    using Row = std::pair<std::string, std::string>;

    ConsoleListing() = default;
    ConsoleListing(const ConsoleListing&) = delete;
    ConsoleListing& operator=(const ConsoleListing&) = delete;
    ConsoleListing(ConsoleListing&&) = delete;
    ConsoleListing& operator=(ConsoleListing&&) = delete;
    virtual ~ConsoleListing() = default;

    /** The word after `list` that asks for it, matched ignoring case. */
    virtual std::string_view word() const = 0;
    /** Its line in list's usage, such as {"list maps", "every level, by realm"}; none for a
     * listing that adds to another of its word. */
    virtual std::optional<Row> usageRow() const { return std::nullopt; }
    /** What it says, a line each, of the words after its own, such as a map's name. */
    virtual std::vector<std::string> lines(Arguments rest) const = 0;
};

/** A listing made of its word, its usage line and a function: the quick way to register one. */
class LambdaListing final : public ConsoleListing {
public:
    using Lines = std::function<std::vector<std::string>(Arguments rest)>;

    LambdaListing(std::string word, std::optional<Row> row, Lines lines)
        : m_word(std::move(word)), m_usageRow(std::move(row)), m_lines(std::move(lines)) {}
    std::string_view word() const override { return m_word; }
    std::optional<Row> usageRow() const override { return m_usageRow; }
    std::vector<std::string> lines(Arguments rest) const override {
        return m_lines ? m_lines(rest) : std::vector<std::string>{};
    }

private:
    std::string m_word;
    std::optional<Row> m_usageRow;
    Lines m_lines;
};

/**
 * What `list` names, kept in the order registered. Listings may share a word: the lines of
 * each follow those of the one registered before it, and the first to give a usage line
 * stands for the word in the usage.
 */
class ConsoleListings {
public:
    using Arguments = ConsoleListing::Arguments;

    void add(std::unique_ptr<ConsoleListing> listing);
    /** Every word a listing answers, once each, in the order first registered. */
    std::vector<std::string> words() const;
    /** list's usage: each word's usage line, in the order registered. */
    std::string usage() const;
    /** What `list` says of `arguments`: the lines of every listing of the first word in turn,
     * or the usage when no listing answers it. */
    std::vector<std::string> lines(Arguments arguments) const;

private:
    std::vector<std::unique_ptr<ConsoleListing>> m_listings;
};

/** The `list` command over `listings`, which it reads each time it runs or completes, so
 * listings registered after it count too; `listings` must outlive it. */
std::unique_ptr<ConsoleCommand> listCommand(const ConsoleListings& listings);

} // namespace gdl::game
