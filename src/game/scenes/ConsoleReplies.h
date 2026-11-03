#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/scenes/PlayScene.h"

namespace gdl::game {

// What the shared console verbs answer where offline play and the shared game word a reply
// apart: each mode keeps its own words.

/** What god, notarget or noclip, named `verb`, says once it is turned `on` or off. */
std::string cheatReply(PlayScene::ConsolePlay play, std::string_view verb, bool on);
/** What suicide says once `fallen` characters have fallen, away from the tower. */
std::string fallenReply(PlayScene::ConsolePlay play, s32 fallen);
/** What unlock_tower says once the tower stands open to the `opened` characters still in. */
std::string towerOpenedReply(PlayScene::ConsolePlay play, s32 opened);
/** What goto says of the party moved to `at`, or of nobody standing to move without it. */
std::string movedReply(PlayScene::ConsolePlay play, const std::optional<Vec3>& at);
/** What goto or spawn says of a word, `name`, that names no level. */
std::string unknownMapReply(PlayScene::ConsolePlay play, std::string_view name);
/** What spawn says once `count` of the swarm's kind `name` stood at `tier`. */
std::string spawnedEnemiesReply(PlayScene::ConsolePlay play, s32 count, std::string_view name,
                                s32 tier);
/** What spawn says of a great one `name`, in a gargoyle's `form` when it has one, that `stood`
 * or could not. */
std::string critterReply(PlayScene::ConsolePlay play, bool stood, std::string_view name,
                         std::string_view form);
/** What spawn says of a mixed brood asked of a realm that breeds none. */
std::string_view broodRealmRefusal(PlayScene::ConsolePlay play);
/** What spawn says of a pickup asked of another level. */
std::string_view pickupWithMapRefusal(PlayScene::ConsolePlay play);
/** What spawn says when the level in play has no pickup record `name`. */
std::string pickupMissingReply(PlayScene::ConsolePlay play, std::string_view name);
/** What spawn says when `level` has no object `name`. */
std::string objectMissingReply(PlayScene::ConsolePlay play, std::string_view name,
                               std::string_view level);
/** What spawn says when `name`, no map named, is neither a pickup nor an object of `level`, the
 * level in play. */
std::string thingMissingReply(PlayScene::ConsolePlay play, std::string_view name,
                              std::string_view level);
/** The line offline play puts before a spawn's reply when `level`, the map named, lent the swarm
 * no voices; the shared game says nothing of it. */
std::optional<std::string> voicesNote(PlayScene::ConsolePlay play, std::string_view level);
/** The line offline play puts before a spawn's reply when `level`, the map named, is not
 * unpacked; the shared game says nothing of it. */
std::optional<std::string> notUnpackedNote(PlayScene::ConsolePlay play, std::string_view level);
/** A spawn's reply: `result` after the notes there are, a line each in their order. */
std::string afterNotes(std::span<const std::optional<std::string>> notes, std::string_view result);

} // namespace gdl::game
