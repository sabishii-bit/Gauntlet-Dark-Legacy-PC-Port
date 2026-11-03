#include "game/scenes/ConsoleStage.h"

#include "engine/assets/WorldData.h"
#include "engine/core/Assert.h"

#include "game/party/PartyRecords.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {

PlayScene::ConsoleCheats ConsoleStage::cheats(s32 seat) const {
    return m_scene.cheats(seat);
}

void ConsoleStage::setCheats(s32 seat, const PlayScene::ConsoleCheats& cheats) {
    m_scene.setCheats(seat, cheats);
}

CharacterSave& ConsoleStage::save(s32 seat) {
    CharacterSave* live = m_scene.liveSave(seat);
    GDL_VERIFY(live != nullptr, "A console action's seat is found in the party before it runs");
    return *live;
}

const PlayerActor* ConsoleStage::actor(s32 seat) const {
    return m_scene.actor(seat);
}

std::optional<s32> ConsoleStage::fittingAward(const CharacterSave& save, s32 amount) const {
    return PartyRecords::fittingAward(m_scene.m_world->level(), save, amount);
}

const CharacterSave* ConsoleStage::comboPartner(s32 seat) const {
    for (const PlayerRuntime& runtime : m_scene.m_players) {
        if (runtime.actor.player() != seat || !runtime.combo.active()) {
            continue;
        }
        const s32 partner = runtime.combo.partner;
        if (partner >= 0 && static_cast<usize>(partner) < m_scene.m_players.size()) {
            return &m_scene.m_players[static_cast<usize>(partner)].actor.save();
        }
    }
    return nullptr;
}

void ConsoleStage::awardExperience(s32 seat, s32 amount) {
    m_scene.awardExperience(seat, amount, false);
}

void ConsoleStage::restat(s32 seat) {
    m_scene.restat(seat);
}

std::vector<CharacterSave*> ConsoleStage::presentSaves() {
    std::vector<CharacterSave*> saves;
    for (PlayerRuntime& runtime : m_scene.m_players) {
        if (!runtime.departed) {
            saves.push_back(&runtime.actor.save());
        }
    }
    return saves;
}

s32 ConsoleStage::killEveryone() {
    s32 fallen = 0;
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        fallen += m_scene.kill(seat) ? 1 : 0;
    }
    return fallen;
}

bool ConsoleStage::relocate(const Vec3& position) {
    return m_scene.relocate(position);
}

void ConsoleStage::requestTravel(const PlayScene::ConsoleTravelRequest& request, s32 seat) {
    // A native tick records intent only. The all-seat-confirmed terminal
    // frontier owns loading and replacement of the current scene.
    auto& sim = m_scene.m_sim;
    sim.consoleTravel = request;
    sim.destination = request.destination;
    sim.secretTravel = request.destination.isSecret();
    if (sim.secretTravel) {
        if (const PlayerActor* owner = m_scene.actor(seat)) {
            sim.secretReturnPosition = owner->position();
        }
    }
}

bool ConsoleStage::isTower() const {
    return m_scene.m_world->isTower();
}

const LevelRef& ConsoleStage::level() const {
    return m_scene.m_world->ref();
}

std::optional<LevelRef> ConsoleStage::levelNamed(std::string_view name) const {
    if (name == "TOWER") {
        return LevelRef::tower();
    }
    const LevelCatalog* levels = m_scene.m_context.levels;
    return levels != nullptr ? levels->byName(name) : std::nullopt;
}

const WorldCollision& ConsoleStage::collision() const {
    return m_scene.m_world->collision();
}

RenderDevice& ConsoleStage::device() {
    return *m_scene.m_device;
}

bool ConsoleStage::placeItem(std::string_view name, const Vec3& position) {
    return m_scene.m_world->placeItem(*m_scene.m_device, name, position);
}

bool ConsoleStage::spawnObject(const LevelRef& level, std::string_view name, const Vec3& position,
                               f32 yaw) {
    return m_scene.spawnObject(level, name, position, yaw);
}

ItemArchive* ConsoleStage::borrowItems(const LevelRef& level) {
    return m_scene.borrowItems(level);
}

bool ConsoleStage::borrowVoices(const LevelRef& from) {
    WorldData data;
    if (!data.load(m_scene.m_context.unpackedRoot / from.worldDataFile())) {
        return false;
    }
    const LevelInfo* level = data.level(from.name);
    return level != nullptr && m_scene.borrowOpponentSounds(*level, data.audio(level->audioIndex));
}

bool ConsoleStage::loadLayout(const LevelRef& from, WorldLayout& layout) const {
    return layout.load(m_scene.m_context.unpackedRoot / from.directory);
}

bool ConsoleStage::spawnRoom() const {
    return m_play == PlayScene::ConsolePlay::Offline ||
           m_scene.m_sim.consoleSpawns < kMaximumConsoleSpawns;
}

void ConsoleStage::countSpawn() {
    ++m_scene.m_sim.consoleSpawns;
}

Enemies& ConsoleStage::enemies() {
    return m_scene.enemies();
}

Critters& ConsoleStage::critters() {
    return m_scene.critters();
}

Generators& ConsoleStage::generators() {
    return m_scene.generators();
}

std::vector<EnemyView> ConsoleStage::opponentViews() const {
    return m_scene.m_opponents.views(m_scene.m_players);
}

} // namespace gdl::game
