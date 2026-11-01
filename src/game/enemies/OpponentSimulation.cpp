#include "game/enemies/OpponentSimulation.h"

namespace gdl::game {
std::vector<MissileTarget> OpponentSimulation::targets(const Enemies& enemies,
                                                       std::span<const MissileTarget> critters,
                                                       std::span<const MissileTarget> bosses) {
    auto result = enemies.targets();
    for (auto target : critters) {
        target.id += kCritterIds;
        result.push_back(target);
    }
    for (auto target : bosses) {
        target.id += kBossIds;
        result.push_back(target);
    }
    return result;
}

std::vector<MissileStop> OpponentSimulation::advance(const Frame& frame,
                                                     const EnemyMissiles::SceneryBlast& scenery,
                                                     const std::function<void()>& afterEnemies) {
    if (m_placements != nullptr) {
        // This is the native watch phase's ordinary dispatcher. It intentionally
        // consumes each selected record even when the spawn fails or replaces a
        // visible pool slot; the shared owner preserves the original row order.
        m_placements->activate(
            [&](const auto& row) { m_enemies.spawn(row.spawn, {}, m_generators.obstacles()); });
    }
    if (frame.tuning.wholePool) {
        m_enemies.widenPool(); // the mechanics let the swarm fill the whole pool
    } else {
        m_enemies.narrowPool(); // and it thins back to the level's cap once they no longer do
    }
    m_generators.update(frame.ticks, m_enemies, frame.players, frame.fixtures, frame.timeStopped);
    auto boxes = m_generators.enemyObstacles();
    boxes.insert(boxes.end(), frame.fixtures.begin(), frame.fixtures.end());
    m_enemies.update(frame.ticks, frame.seconds, frame.players, boxes, &m_missiles,
                     frame.missileSpeed, frame.timeStopped);
    // Birth effects precede effects produced by missile collisions.
    if (afterEnemies) {
        afterEnemies();
    }
    std::vector<MissileStop> stops;
    for (const auto& box : m_generators.obstacles()) {
        stops.push_back(MissileStop::of(box));
    }
    if (frame.stops.empty()) {
        for (const auto& box : frame.fixtures) {
            stops.push_back(MissileStop::of(box));
        }
    } else {
        stops.insert(stops.end(), frame.stops.begin(), frame.stops.end());
    }
    m_missiles.update(frame.seconds, frame.collision, frame.players,
                      targets(m_enemies, frame.critters, frame.bosses), stops, scenery);
    return stops;
}

} // namespace gdl::game
