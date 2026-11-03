#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleReach.h"

namespace gdl::game {

class DevConsole;

/**
 * One command the developer console runs, as an object: its name, what `help` says of it,
 * where it runs, what it does with the console and its arguments, and the words it can
 * complete. A command is registered once with the console's registry and found there by
 * name, ignoring case; the application registers its own beside the console's built-in ones.
 */
class ConsoleCommand {
public:
    using Arguments = std::span<const std::string>;

    ConsoleCommand() = default;
    ConsoleCommand(const ConsoleCommand&) = delete;
    ConsoleCommand& operator=(const ConsoleCommand&) = delete;
    ConsoleCommand(ConsoleCommand&&) = delete;
    ConsoleCommand& operator=(ConsoleCommand&&) = delete;
    virtual ~ConsoleCommand() = default;

    virtual std::string_view name() const = 0;
    /** What `help` says of the command after its name. */
    virtual std::string summary() const = 0;
    /** Where the command runs, which decides the sessions that admit it. */
    virtual ConsoleReach reach() const = 0;
    virtual void run(DevConsole& console, Arguments arguments) = 0;
    /** The words the console may complete the last of `arguments` to (the word being typed,
     * which may be empty); none for a command that takes free text. */
    virtual std::vector<std::string> completions(Arguments arguments) const;
};

/** A command made of a name, a summary, its reach and a function: the quick way to register
 * one. Its reach is never Shared. */
class LambdaCommand final : public ConsoleCommand {
public:
    using Handler = std::function<void(DevConsole& console, Arguments arguments)>;
    using Completer = std::function<std::vector<std::string>(Arguments arguments)>;

    LambdaCommand(std::string name, std::string summary, ConsoleReach reach, Handler handler,
                  Completer completer = {});
    std::string_view name() const override { return m_name; }
    std::string summary() const override { return m_summary; }
    ConsoleReach reach() const override { return m_reach; }
    void run(DevConsole& console, Arguments arguments) override;
    std::vector<std::string> completions(Arguments arguments) const override;

private:
    std::string m_name;
    std::string m_summary;
    ConsoleReach m_reach;
    Handler m_handler;
    Completer m_completer;
};

/** A completer that offers a fixed set of words for the first argument. */
LambdaCommand::Completer wordsCompleter(std::vector<std::string> words);

/**
 * The console's commands: kept in the order they were registered (the order `help` lists
 * them), found by name ignoring case, and the first whose name begins with a prefix, for
 * completing the word being typed. Beside them it keeps the forms other modules answer
 * of a command's words, in the order they were registered. It is the host modules register
 * their words through.
 */
class ConsoleRegistry final : public ConsoleHost {
public:
    void add(std::unique_ptr<ConsoleCommand> command) override;
    /** Registers a command made of a name, a summary, its reach and a function. */
    void add(std::string name, std::string summary, ConsoleReach reach,
             LambdaCommand::Handler handler, LambdaCommand::Completer completer = {});
    void addForm(std::string_view command, std::unique_ptr<ConsoleForm> form) override;
    std::span<const std::unique_ptr<ConsoleCommand>> commands() const { return m_commands; }
    usize size() const { return m_commands.size(); }
    /** The command named `name` in any case, or null. */
    ConsoleCommand* find(std::string_view name) const;
    /** The first form registered on `command` that claims `arguments`, or null. */
    ConsoleForm* claimant(std::string_view command, ConsoleCommand::Arguments arguments) const;
    /** The forms registered on `command`, in their order; none for a name without forms. */
    std::span<const std::unique_ptr<ConsoleForm>> forms(std::string_view command) const;
    /** The first registered name (among those `allowed`, when given) that begins with
     * `prefix` in any case and is longer than it: what the console completes the word to. */
    std::optional<std::string>
    completeName(std::string_view prefix,
                 const std::function<bool(std::string_view)>& allowed = {}) const;

private:
    /** The forms of one command, under the name they were registered on. */
    struct CommandForms {
        std::string command;
        std::vector<std::unique_ptr<ConsoleForm>> forms;
    };

    std::vector<std::unique_ptr<ConsoleCommand>> m_commands;
    std::vector<CommandForms> m_forms;
};

/** Whether `word` begins with `prefix`, ignoring case. */
bool startsWithIgnoringCase(std::string_view word, std::string_view prefix);
/** Whether two names are the same, ignoring case. */
bool sameNameIgnoringCase(std::string_view a, std::string_view b);

} // namespace gdl::game
