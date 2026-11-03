#pragma once
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"

#include "game/characters/CharacterSave.h"
#include "game/critters/Critters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/level/LevelCatalog.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/**
 * The scene as the shared console verbs may change it: the party's cheats, records and places,
 * its travel, and what a spawn stands before it. Only the scene makes one, for one console
 * action at the start of a tick, in that action's game.
 */
class ConsoleStage {
public:
    /** The most console spawns that stand in one stage of the shared game. */
    static constexpr u32 kMaximumConsoleSpawns = 256;

    PlayScene::ConsolePlay play() const { return m_play; }

    PlayScene::ConsoleCheats cheats(s32 seat) const;
    void setCheats(s32 seat, const PlayScene::ConsoleCheats& cheats);
    /** The live record of `seat`, whose player the scene has found in the party. */
    CharacterSave& save(s32 seat);
    /** The character `seat` drives, or null when that seat is not in. */
    const PlayerActor* actor(s32 seat) const;
    /** The experience `amount` comes to for `save` here, when its counter has room for it. */
    std::optional<s32> fittingAward(const CharacterSave& save, s32 amount) const;
    /** The record of the partner `seat`'s character shares its experience with in a combo, or
     * null. */
    const CharacterSave* comboPartner(s32 seat) const;
    /** Gives `seat`'s character experience won otherwise than by a kill. */
    void awardExperience(s32 seat, s32 amount);
    void restat(s32 seat);
    /** The records of everyone still in the party, in its order. */
    std::vector<CharacterSave*> presentSaves();
    /** Fells everyone standing; how many fell. */
    s32 killEveryone();
    bool relocate(const Vec3& position);
    /** Sends the party to `request`'s destination once the tick is confirmed; into a challenge,
     * the way back is where `seat`'s character stands. */
    void requestTravel(const PlayScene::ConsoleTravelRequest& request, s32 seat);

    bool isTower() const;
    /** The level in play. */
    const LevelRef& level() const;
    /** The level a console word names ("TOWER" the tower), or none. */
    std::optional<LevelRef> levelNamed(std::string_view name) const;
    const WorldCollision& collision() const;
    RenderDevice& device();
    /** Drops a pickup of the level in play by its record's name; false for none of that name. */
    bool placeItem(std::string_view name, const Vec3& position);
    bool spawnObject(const LevelRef& level, std::string_view name, const Vec3& position, f32 yaw);
    ItemArchive* borrowItems(const LevelRef& level);
    /** Lends the swarm `from`'s voices; false when that level lends none. */
    bool borrowVoices(const LevelRef& from);
    /** Loads `from`'s layout; false when it is not unpacked. */
    bool loadLayout(const LevelRef& from, WorldLayout& layout) const;

    /** Whether another console spawn may stand: always offline, under kMaximumConsoleSpawns in
     * the shared game. */
    bool spawnRoom() const;
    /** Counts a console spawn that stood; counted in both games. */
    void countSpawn();
    Enemies& enemies();
    Critters& critters();
    Generators& generators();
    /** The party as the swarm sees it, for what is spawned to face. */
    std::vector<EnemyView> opponentViews() const;

private:
    friend class PlayScene;
    ConsoleStage(PlayScene& scene, PlayScene::ConsolePlay play) : m_scene(scene), m_play(play) {}

    PlayScene& m_scene;
    PlayScene::ConsolePlay m_play;
};

} // namespace gdl::game
