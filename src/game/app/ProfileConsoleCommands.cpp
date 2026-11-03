#include "game/app/ProfileConsoleCommands.h"

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleReach.h"
#include "game/console/DevConsole.h"
#include "game/console/FixtureRequest.h"
#include "game/console/LevelListingLines.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

namespace {
using Arguments = ConsoleForm::Arguments;

/** Whether `arguments` name a character by its code for `verb`. */
bool namesCharacter(Arguments arguments, LockVerb verb) {
    const auto request = parseLockRequest(arguments, verb);
    return request.has_value() && request->character;
}

/** unlock's or lock's character form, as `verb` says. */
void changeCharacter(const ProfileConsoleAccess& access, DevConsole& console, Arguments arguments,
                     LockVerb verb) {
    const bool unlock = verb == LockVerb::Unlock;
    const auto request = parseLockRequest(arguments, verb);
    GDL_VERIFY(request.has_value() && request->character,
               "the character form claims a character's code alone");
    const auto index = classIndexOf(request->what);
    if (!index.has_value()) {
        console.print(
            std::format("No character is coded {}; list characters names them", request->what));
        return;
    }
    if (*index < kStartingClassCount) {
        console.print(std::format("{} is always there to choose", classCode(*index)));
        return;
    }
    if (!access.saves.opened() &&
        !access.saves.open(access.config.saveDirectory(), access.config.save.slots)) {
        console.print("The saves cannot be opened.");
        return;
    }
    const auto mask = static_cast<u16>(1U << (*index - kStartingClassCount));
    const bool written = unlock ? access.saves.unlockClasses(mask) : access.saves.lockClasses(mask);
    PlayScene* scene = access.scene();
    if (!unlock && scene != nullptr) {
        // The party in play carries the old reward bits; they would bring it back.
        for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
            if (CharacterSave* save = scene->liveSave(player)) {
                save->classUnlock &= static_cast<u16>(~mask);
            }
        }
    }
    console.print(std::format("{} {}{}", classCode(*index),
                              unlock ? "unlocked: the select screen offers it"
                                     : "locked: the select screen hides it again",
                              written ? "" : " (for this session; the profile was not written)"));
}

void addCharacterForm(ConsoleHost& host, const ProfileConsoleAccess& access, LockVerb verb) {
    host.addForm(verb == LockVerb::Unlock ? "unlock" : "lock",
                 std::make_unique<LambdaForm>(
                     ConsoleReach::OfflineOnly, std::string{},
                     [verb](Arguments arguments) { return namesCharacter(arguments, verb); },
                     [access, verb](DevConsole& console, Arguments arguments) {
                         changeCharacter(access, console, arguments, verb);
                     }));
}
} // namespace

void registerProfileConsoleCommands(ConsoleHost& host, ConsoleListings& listings,
                                    const ProfileConsoleAccess& access) {
    addCharacterForm(host, access, LockVerb::Unlock);
    addCharacterForm(host, access, LockVerb::Lock);
    listings.add(std::make_unique<LambdaListing>(
        "characters",
        ConsoleListing::Row{"list characters", "every playable character, locked or hidden too"},
        [access](ConsoleListing::Arguments /*rest*/) {
            if (access.saves.opened() ||
                access.saves.open(access.config.saveDirectory(), access.config.save.slots)) {
                return characterLines(access.saves.classUnlocks());
            }
            return std::vector<std::string>{
                "The saves cannot be opened, so what is unlocked is unknown."};
        }));
}

} // namespace gdl::game
