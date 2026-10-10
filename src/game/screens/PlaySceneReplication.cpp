#include "game/screens/PlayScene.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/StageResources.h"

namespace gdl::game {
bool PlayScene::bindPlayerProjectiles(ProjectileResources& resources) {
    if (!m_open || m_device == nullptr) {
        return false;
    }
    std::array<PlayerFigure*, InputCommand::kSeats> figures{};
    for (const auto& player : m_players) {
        const auto seat = player.actor.player();
        if (seat < 0 || static_cast<usize>(seat) >= figures.size() || player.figure == nullptr ||
            figures[static_cast<usize>(seat)] != nullptr) {
            return false;
        }
        figures[static_cast<usize>(seat)] = player.figure.get();
    }
    return resources.addPlayers(*m_device, m_weapons, figures, m_effects.textureLenders());
}
bool PlayScene::bindSceneProjectiles(ProjectileResources& resources) {
    if (!m_open || m_device == nullptr || m_world == nullptr) {
        return false;
    }
    ProjectileResources next = resources;
    auto stocks = m_opponents.critters().resources();
    for (auto* stock : m_opponents.bosses().resources()) {
        stocks.push_back(stock);
    }
    const std::array archives{&m_world->items(), &m_world->realmItems(), &m_world->powerups()};
    if (!bindPlayerProjectiles(next) ||
        !next.addEnemies(*m_device, m_opponents.enemies(), m_effects.textureLenders()) ||
        !next.addStage(*m_device, archives, stocks, m_effects.textureLenders())) {
        return false;
    }
    resources = std::move(next);
    return true;
}
bool PlayScene::bindReplicationResources(ProjectileResources& projectiles, PickupResources& pickups,
                                         FixtureResources& fixtures) {
    if (!m_open || m_device == nullptr || m_world == nullptr ||
        !StageResources::preload(*m_world, m_opponents.enemies(), m_opponents.critters(),
                                 m_opponents.bosses())) {
        return false;
    }
    ProjectileResources nextProjectiles;
    PickupResources nextPickups;
    FixtureResources nextFixtures;
    const auto archives = StageResources::fixtureArchives(
        *m_world, m_opponents.enemies(), m_opponents.critters(), m_opponents.bosses());
    if (!bindSceneProjectiles(nextProjectiles) ||
        !nextPickups.bind(*m_device, m_world->placedItems().archives()) ||
        !nextFixtures.bind(*m_device, archives, m_opponents.generators(), m_fixtures.safeRocks())) {
        return false;
    }
    projectiles = std::move(nextProjectiles);
    pickups = std::move(nextPickups);
    fixtures = std::move(nextFixtures);
    return true;
}
} // namespace gdl::game
