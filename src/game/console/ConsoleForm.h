#pragma once
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/console/ConsoleReach.h"

namespace gdl::game {

class DevConsole;

/**
 * Words of a command that a module other than the command's owner answers. The console asks
 * a command's forms before the command, and the first that claims the words runs them where
 * its own reach says.
 */
class ConsoleForm {
public:
    using Arguments = std::span<const std::string>;

    ConsoleForm() = default;
    ConsoleForm(const ConsoleForm&) = delete;
    ConsoleForm& operator=(const ConsoleForm&) = delete;
    ConsoleForm(ConsoleForm&&) = delete;
    ConsoleForm& operator=(ConsoleForm&&) = delete;
    virtual ~ConsoleForm() = default;

    /** Whether these words, the command's name left out, are this form's to answer. */
    virtual bool claims(Arguments arguments) const = 0;
    virtual ConsoleReach reach() const = 0;
    virtual void run(DevConsole& console, Arguments arguments) = 0;
    /** What the command's summary says of this form, such as "<legends map> [preview]". */
    virtual std::string_view summaryForm() const { return {}; }
    /** The form's lines in the command's usage: the words, and what they do. */
    virtual std::vector<std::pair<std::string, std::string>> usageRows() const { return {}; }
    /** A note that stands in for the command's own under its usage. */
    virtual std::string_view usageNote() const { return {}; }
    /** What the console says when the form is refused for running only offline. */
    virtual std::string_view refusal() const { return {}; }
};

} // namespace gdl::game
