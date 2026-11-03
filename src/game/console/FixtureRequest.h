#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Expected.h"
#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"

namespace gdl::game {

/** A thing of a level the console names: its token (switch#3, gate#0, chest#12), the name it
 * goes by and, for a switch, the object it drives. */
struct NamedEntry {
    std::string token;
    std::string name;
    // NOLINTNEXTLINE(readability-redundant-member-init) -- default for partial aggregates
    std::string alternate{}; ///< another name it answers to: a switch's target
    bool operator==(const NamedEntry&) const = default;
};

/** One switch of a level, for `list switches` and `activate`. */
struct SwitchEntry {
    s32 index = -1; ///< its token is switch#index
    s32 id = 0;
    std::string name;
    std::string target; ///< the object it drives, or empty
    bool shootable = false;
    bool chained = false; ///< fired through another switch, never stepped on
    bool fired = false;
    bool enabled = true; ///< for the party present
    bool operator==(const SwitchEntry&) const = default;
};

/** Which of the two commands a lock request is read for. */
enum class LockVerb : u8 { Unlock, Lock };

/** What `lock` or `unlock` is asked for: a character by its code, or a door or chest by its
 * token or name. */
struct LockRequest {
    bool character = false;
    std::string what;
    bool operator==(const LockRequest&) const = default;
};

/** Every entry `word` names: by its token, its name or its alternate, in any case. */
std::vector<usize> entriesNamed(std::span<const NamedEntry> entries, std::string_view word);
/** The switches a layout's trigger items stand for, in the order the level binds them. */
std::vector<SwitchEntry> switchesOf(const WorldLayout& layout);
std::vector<NamedEntry> switchEntries(std::span<const SwitchEntry> switches);
std::string activateUsage();
/** Reads `lock`'s or `unlock`'s arguments; that command's usage when they make no sense. */
gdl::Expected<LockRequest, std::string> parseLockRequest(ConsoleCommand::Arguments arguments,
                                                         LockVerb verb);
std::string lockUsage(LockVerb verb);

} // namespace gdl::game
