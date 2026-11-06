#include <utility>

#include "engine/core/Error.h"

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

std::optional<usize>
LegendsSceneAdapter::bindN64Death(const formats::LegendsN64Fixture& fixture, usize figure,
                                  std::span<const formats::LegendsMapCollisionTriangle> collision) {
    auto state = LegendsDeath::decode(fixture, m_playerCount);
    if (!state) {
        return {};
    }
    if (m_deaths.size() >= LegendsDeath::kMost) {
        throw FormatError("Too many native Death statues");
    }
    const auto placement = formats::legendsN64FixturePlacement(fixture, collision);
    state->placeAt(placement.position);
    m_deaths.push_back({*state, figure, m_fixtures[figure].transform(), placement.floorObject});
    return m_deaths.size() - 1;
}

void LegendsSceneAdapter::updateDeaths() {
    for (auto& death : m_deaths) {
        if (death.state.released() || !death.active) {
            continue;
        }
        const Mat4 pose =
            death.floorOwner >= 0
                ? glm::translate(Mat4{1}, m_worldMovement.supportingOffset(death.floorOwner)) *
                      death.initialTransform
                : m_fixtures[death.figure].transform();
        death.state.placeAt(Vec3{pose[3]});
        m_fixtures[death.figure].placeAt(pose);
    }
}

std::vector<ImportedTarget> LegendsSceneAdapter::deathTargets() const {
    std::vector<ImportedTarget> result;
    for (usize i = 0; i < m_deaths.size(); ++i) {
        if (m_deaths[i].active && !m_deaths[i].state.released() &&
            !m_hiddenFixtures[m_deaths[i].figure]) {
            result.push_back(m_deaths[i].state.target(static_cast<s32>(i)));
        }
    }
    return result;
}

std::vector<Obstacle> LegendsSceneAdapter::deathObstacles() const {
    std::vector<Obstacle> result;
    for (const auto& target : deathTargets()) {
        Obstacle body;
        body.centre = target.base;
        body.height = target.height;
        body.cylinderRadius = target.radius;
        body.enemyItem = Obstacle::ItemQuery{true, target.radius};
        result.push_back(body);
    }
    return result;
}

bool LegendsSceneAdapter::strikeDeath(s32 localId, f32 power) {
    const s32 index = localId - LegendsDeath::kTargetBase;
    if (index < 0 || static_cast<usize>(index) >= m_deaths.size()) {
        return false;
    }
    auto& death = m_deaths[static_cast<usize>(index)];
    if (!death.active || m_hiddenFixtures[death.figure] || !death.state.strike(power)) {
        return false;
    }
    for (const auto& selector : m_floorSelectors) {
        if (selector.activeFigure == death.figure) {
            m_lootSelectors.freeze(selector.slot);
        }
    }
    return true;
}

void LegendsSceneAdapter::touchDeaths(Vec3 feet, f32 radius, f32 height) {
    for (auto& death : m_deaths) {
        if (death.active && !m_hiddenFixtures[death.figure] &&
            death.state.touch(feet, radius, height)) {
            for (const auto& selector : m_floorSelectors) {
                if (selector.activeFigure == death.figure) {
                    m_lootSelectors.freeze(selector.slot);
                }
            }
        }
    }
}

void LegendsSceneAdapter::releaseDeaths(const std::function<bool(const EnemySpawn&)>& release) {
    if (!release) {
        return;
    }
    for (usize index = 0; index < m_deaths.size(); ++index) {
        auto& death = m_deaths[index];
        if (!death.active || !death.state.pending() || m_hiddenFixtures[death.figure]) {
            continue;
        }
        for (const auto& selector : m_floorSelectors) {
            if (selector.activeFigure == death.figure) {
                m_lootSelectors.freeze(selector.slot);
            }
        }
        if (!release(death.state.spawn())) {
            continue;
        }
        // e0065560 replaces POWERUP:DETH with the live class7 actor. Unlike the
        // original fixed pool, admission can fail here: retain the statue until
        // the shared enemy pool accepts it, and emit its sound only once.
        death.state.release();
        m_hiddenFixtures[death.figure] = true;
        for (auto& container : m_containers) {
            if (container.death == index) {
                container.state.collected();
                if (container.state.definition().kind == LegendsContainerDefinition::Kind::Chest) {
                    m_hiddenFixtures[container.openingFigure] = true;
                    m_hiddenFixtures[container.brokenFigure] = true;
                }
            }
        }
        m_soundEvents.push_back({0x6e, death.state.position()});
        m_deathAdvicePending = true;
    }
}

bool LegendsSceneAdapter::takeDeathAdvice() {
    return std::exchange(m_deathAdvicePending, false);
}

} // namespace gdl::game
