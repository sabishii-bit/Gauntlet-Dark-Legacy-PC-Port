#pragma once
#include <functional>
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

/** A form made of its reach, its refusal and two functions: the quick way to register one. */
class LambdaForm final : public ConsoleForm {
public:
    using Claims = std::function<bool(Arguments arguments)>;
    using Handler = std::function<void(DevConsole& console, Arguments arguments)>;

    LambdaForm(ConsoleReach reach, std::string refusal, Claims claims, Handler handler)
        : m_reach(reach), m_refusal(std::move(refusal)), m_claims(std::move(claims)),
          m_handler(std::move(handler)) {}
    bool claims(Arguments arguments) const override { return m_claims && m_claims(arguments); }
    ConsoleReach reach() const override { return m_reach; }
    void run(DevConsole& console, Arguments arguments) override {
        if (m_handler) {
            m_handler(console, arguments);
        }
    }
    std::string_view refusal() const override { return m_refusal; }

private:
    ConsoleReach m_reach;
    std::string m_refusal;
    Claims m_claims;
    Handler m_handler;
};

} // namespace gdl::game
