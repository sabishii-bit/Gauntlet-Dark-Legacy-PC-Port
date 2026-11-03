#pragma once
#include <memory>
#include <string_view>

namespace gdl::game {

class ConsoleCommand;
class ConsoleForm;

/**
 * What a module registers its console words through: the commands it owns, and the forms it
 * answers of commands other modules own. It offers nothing else of the console.
 */
class ConsoleHost {
public:
    /** Registers `command`; one of the same name replaces the one held, where it stood. A
     * command that runs in the shared game must be one of the wire's shared words. */
    virtual void add(std::unique_ptr<ConsoleCommand> command) = 0;
    /** Registers `form` on the command named `command` in any case, after its other forms. */
    virtual void addForm(std::string_view command, std::unique_ptr<ConsoleForm> form) = 0;

protected:
    ~ConsoleHost() = default;
};

} // namespace gdl::game
