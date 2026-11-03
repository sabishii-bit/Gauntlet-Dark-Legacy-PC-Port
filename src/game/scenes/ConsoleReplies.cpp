#include "game/scenes/ConsoleReplies.h"

#include <format>

#include "game/console/NoclipMotion.h"

namespace gdl::game {

namespace {
using ConsolePlay = PlayScene::ConsolePlay;

bool offline(ConsolePlay play) {
    return play == ConsolePlay::Offline;
}
} // namespace

std::string cheatReply(ConsolePlay play, std::string_view verb, bool on) {
    if (offline(play)) {
        if (verb == "god") {
            return on ? "God mode on" : "God mode off";
        }
        if (verb == "notarget") {
            return on ? "The enemies no longer see you" : "The enemies see you again";
        }
        if (verb == "noclip") {
            return on ? std::format("Noclip on: {}", NoclipMotion::kHelp) : "Noclip off";
        }
    }
    return std::format("{} {}", verb, on ? "on" : "off");
}

std::string fallenReply(ConsolePlay play, s32 fallen) {
    if (offline(play) && fallen == 0) {
        return "Nobody was standing.";
    }
    return std::format("{} fallen", fallen);
}

std::string towerOpenedReply(ConsolePlay play, s32 opened) {
    if (!offline(play)) {
        return "The tower stands open to the party.";
    }
    if (opened == 0) {
        return "Nobody is in to open it for.";
    }
    return std::format("The tower stands open to {} character{}.", opened, opened == 1 ? "" : "s");
}

std::string movedReply(ConsolePlay play, const std::optional<Vec3>& at) {
    if (!offline(play)) {
        return at ? "Moved the party." : "Nobody is standing.";
    }
    if (!at) {
        return "Nobody is standing to move.";
    }
    return std::format("Moved to {:.1f} {:.1f} {:.1f}", at->x, at->y, at->z);
}

std::string unknownMapReply(ConsolePlay play, std::string_view name) {
    return offline(play) ? std::format("No map named {}; list maps names them", name)
                         : std::format("No map named {}", name);
}

std::string spawnedEnemiesReply(ConsolePlay play, s32 count, std::string_view name, s32 tier) {
    if (offline(play) && count == 0) {
        return std::format("No room for {} here, or its kind is missing", name);
    }
    return std::format("Spawned {} {} at tier {}", count, name, tier);
}

std::string critterReply(ConsolePlay play, bool stood, std::string_view name,
                         std::string_view form) {
    if (!offline(play)) {
        return stood ? std::format("Spawned a {}", name)
                     : std::format("No room or artwork for {}", name);
    }
    const std::string_view what = form.empty() ? name : form;
    return stood ? std::format("Spawned a {}", what)
                 : std::format("No room for a {} here, or its kind is missing", what);
}

std::string_view broodRealmRefusal(ConsolePlay play) {
    return offline(play)
               ? "A mixed brood is the temple's (E) or the underworld's (F); name a map of either"
               : "A mixed brood needs a Temple (E) or Underworld (F) map.";
}

std::string_view pickupWithMapRefusal(ConsolePlay play) {
    return offline(play) ? "Pickups come from the level in play alone; name one without a map"
                         : "Pickups come from the current level; omit the map.";
}

std::string pickupMissingReply(ConsolePlay play, std::string_view name) {
    return offline(play)
               ? std::format("No pickup record named {} here; list items names them", name)
               : std::format("No pickup named {}", name);
}

std::string objectMissingReply(ConsolePlay play, std::string_view name, std::string_view level) {
    return offline(play)
               ? std::format("Nothing named {} in {}; list objects {} names what there is", name,
                             level, level)
               : std::format("No object named {} in {}", name, level);
}

std::string thingMissingReply(ConsolePlay play, std::string_view name, std::string_view level) {
    return offline(play) ? std::format("Nothing named {} here; list items, objects, enemies or "
                                       "critters name what there is",
                                       name)
                         : std::format("No object named {} in {}", name, level);
}

std::optional<std::string> voicesNote(ConsolePlay play, std::string_view level) {
    if (!offline(play)) {
        return std::nullopt;
    }
    return std::format("({} lends no voices)", level);
}

std::optional<std::string> notUnpackedNote(ConsolePlay play, std::string_view level) {
    if (!offline(play)) {
        return std::nullopt;
    }
    return std::format("{} is not unpacked", level);
}

} // namespace gdl::game
