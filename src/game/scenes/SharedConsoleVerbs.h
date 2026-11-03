#pragma once
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/console/ConsoleReach.h"
#include "game/level/LevelCatalog.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

class ConsoleStage;

/** Why a shared console line is refused before it is sent: words its command cannot read. */
struct ConsoleRefusal {
    enum class Kind : u8 {
        Usage,  ///< `text` is the command's usage
        Message ///< `text` says what is wrong
    };
    Kind kind = Kind::Message;
    std::string text;
    bool operator==(const ConsoleRefusal&) const = default;
};

/**
 * One command of the shared console as the scene runs it, the same on every peer: what help
 * says of it, how its words are judged before they are sent, and what it does to the scene at
 * the start of a tick. A verb keeps nothing between lines.
 */
class SharedConsoleVerb {
public:
    using Arguments = std::span<const std::string>;

    SharedConsoleVerb() = default;
    SharedConsoleVerb(const SharedConsoleVerb&) = delete;
    SharedConsoleVerb& operator=(const SharedConsoleVerb&) = delete;
    SharedConsoleVerb(SharedConsoleVerb&&) = delete;
    SharedConsoleVerb& operator=(SharedConsoleVerb&&) = delete;
    virtual ~SharedConsoleVerb() = default;

    virtual std::string_view name() const = 0;
    /** The forms help names before the summary's tail, joined by ", " and a last " or ". */
    virtual std::vector<std::string_view> summaryForms() const { return {}; }
    /** What help says the command does after its forms; the whole summary without them. */
    virtual std::string_view summaryTail() const = 0;
    /** The command's forms, each with what it does, as its usage lists them. */
    virtual std::vector<std::pair<std::string, std::string>> usageRows() const { return {}; }
    /** What the usage says under the forms. */
    virtual std::string_view usageNote() const { return {}; }
    /** The words the last of `arguments`, the one being typed, may complete to. */
    virtual std::vector<std::string> completions(Arguments /*arguments*/,
                                                 const LevelCatalog& /*levels*/) const {
        return {};
    }
    /** Whether the command reads any words after its name. */
    virtual bool takesArguments() const = 0;
    /** Context-free: where these words run in `play`'s game, or why every peer refuses them. */
    virtual gdl::Expected<ConsoleReach, ConsoleRefusal>
    check(Arguments arguments, PlayScene::ConsolePlay play) const = 0;
    /** Runs the words for `seat` at the start of a tick and says what came of it; what the
     * shared game cannot do, it refuses in its own words. */
    virtual std::string apply(ConsoleStage& stage, s32 seat, Arguments arguments) const = 0;
    /** What the console says of this command's words that run only offline. */
    virtual std::string_view offlineOnlyRefusal() const { return {}; }
};

/** The shared verb named `lowered`, a name already in lower case; null for any other. */
const SharedConsoleVerb* sharedConsoleVerb(std::string_view lowered);
/** Every shared verb, in the order of the wire's kSharedConsoleWords. */
std::span<const SharedConsoleVerb* const> sharedConsoleVerbs();
/** Whether every axis of `point` is finite and within a million of the origin: the places
 * console words name and the ways console flights wish to go. */
bool consolePointInRange(const Vec3& point);

} // namespace gdl::game
