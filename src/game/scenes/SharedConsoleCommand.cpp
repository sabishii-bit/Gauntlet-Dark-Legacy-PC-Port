#include "game/scenes/SharedConsoleCommand.h"

#include <array>
#include <format>
#include <memory>
#include <optional>
#include <utility>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleUsage.h"

namespace gdl::game {

SharedConsoleCommand::SharedConsoleCommand(const SharedConsoleVerb& verb, ConsoleActionSink& sink,
                                           const LevelCatalog& levels,
                                           const ConsoleRegistry& registry,
                                           LambdaCommand::Completer completer)
    : m_verb(verb), m_sink(sink), m_levels(levels), m_registry(registry),
      m_completer(std::move(completer)) {}

std::string SharedConsoleCommand::summary() const {
    std::vector<std::string_view> forms = m_verb.summaryForms();
    for (const auto& form : m_registry.forms(m_verb.name())) {
        if (!form->summaryForm().empty()) {
            forms.push_back(form->summaryForm());
        }
    }
    if (forms.empty()) {
        return std::string(m_verb.summaryTail());
    }
    std::string text;
    for (usize i = 0; i < forms.size(); ++i) {
        text += i == 0 ? "" : (i + 1 == forms.size() ? " or " : ", ");
        text += forms[i];
    }
    return std::format("{}: {}", text, m_verb.summaryTail());
}

std::string SharedConsoleCommand::usage() const {
    std::vector<std::pair<std::string, std::string>> rows = m_verb.usageRows();
    std::string_view note = m_verb.usageNote();
    for (const auto& form : m_registry.forms(m_verb.name())) {
        const std::vector<std::pair<std::string, std::string>> formRows = form->usageRows();
        rows.insert(rows.end(), formRows.begin(), formRows.end());
        if (!form->usageNote().empty()) {
            note = form->usageNote();
        }
    }
    const std::vector<std::pair<std::string_view, std::string_view>> views(rows.begin(),
                                                                           rows.end());
    if (note.empty()) {
        return consoleUsage(views);
    }
    const std::array<std::string_view, 1> notes{note};
    return consoleUsage(views, notes);
}

std::vector<std::string> SharedConsoleCommand::completions(Arguments arguments) const {
    return m_completer ? m_completer(arguments) : m_verb.completions(arguments, m_levels);
}

void SharedConsoleCommand::run(DevConsole& console, Arguments arguments) {
    const auto reach = m_verb.check(arguments, consolePlayOf(console.session()));
    if (!reach) {
        const ConsoleRefusal& refusal = reach.error();
        console.print(refusal.kind == ConsoleRefusal::Kind::Usage ? usage() : refusal.text);
        return;
    }
    if (*reach == ConsoleReach::OfflineOnly) {
        console.print(m_verb.offlineOnlyRefusal());
        return;
    }
    GDL_VERIFY(*reach == ConsoleReach::Shared,
               "a shared verb's local words are claimed by their form before the verb");
    // A parked scene steps nothing, so it takes none of the shared game's words.
    if (const auto refused = ConsolePolicy::sharedRefusal(console.session())) {
        console.print(*refused);
        return;
    }
    std::string line(m_verb.name());
    for (const std::string& argument : arguments) {
        line += ' ';
        line += argument;
    }
    if (const std::optional<std::string> said = m_sink.submit(std::move(line))) {
        console.print(*said);
    }
}

void registerSharedConsoleCommand(DevConsole& console, std::string_view name,
                                  ConsoleActionSink& sink, const LevelCatalog& levels,
                                  LambdaCommand::Completer completer) {
    const SharedConsoleVerb* verb = sharedConsoleVerb(name);
    GDL_VERIFY(verb != nullptr, "only a shared verb registers as a shared command");
    console.addCommand(std::make_unique<SharedConsoleCommand>(
        *verb, sink, levels, console.commands(), std::move(completer)));
}

PlayScene::ConsolePlay consolePlayOf(const ConsoleSession& session) {
    return session.seat == ConsoleSeat::Offline ? PlayScene::ConsolePlay::Offline
                                                : PlayScene::ConsolePlay::Shared;
}

} // namespace gdl::game
