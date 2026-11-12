#include <algorithm>
#include <cmath>

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/level/ImportedTargets.h"
#include "game/level/MissileStop.h"

namespace gdl::game {
namespace {
/** The segments a rock stands with once an arena's eruption raises it: DC 8c0a9e80 and GC
 * 0x80063D40 (whose eruption the N64 Yeti follows) set three times its definition's health,
 * which AddItemWobj (8c0a9d80, 0x80063DB0) stands as three segments. */
constexpr s32 kRaisedTier = 3;

Obstacle arenaBody(const ImportedBoss::ArenaRock& definition) {
    Obstacle body;
    body.centre = definition.position;
    body.yaw = definition.yaw;
    body.cylinderRadius = definition.radius;
    body.height = definition.height;
    body.enemyItem = Obstacle::ItemQuery{true, definition.radius};
    return body;
}
} // namespace

void LegendsSceneAdapter::selectArenaRock(usize index, bool burst) {
    auto& rock = m_arenaRocks[index];
    if (rock.activeFigure) {
        m_hiddenFixtures[*rock.activeFigure] = true;
    }
    rock.activeFigure.reset();
    if (rock.dormant) {
        return;
    }
    rock.activeFigure =
        burst && rock.burstFigure ? rock.burstFigure : rock.figures[static_cast<usize>(rock.tier)];
    if (rock.activeFigure) {
        auto& figure = m_fixtures[*rock.activeFigure];
        const auto* sequence = figure.sequenceInfo(0);
        figure.play(0, !burst && sequence != nullptr && sequence->repeats
                           ? ItemFigure::Playback::Loop
                           : ItemFigure::Playback::Once);
        figure.capturePresentation();
        m_hiddenFixtures[*rock.activeFigure] = false;
    }
}

bool LegendsSceneAdapter::strikeArenaRock(usize index, f32 power) {
    if (index >= m_arenaRocks.size() || !std::isfinite(power) || power <= 0) {
        return false;
    }
    auto& rock = m_arenaRocks[index];
    if (rock.dormant || rock.health <= 0 || rock.armor < 0) {
        return false;
    }
    // Shared GDL attacks supply their already-calculated damage. Retain native
    // total/tier health, with the same bounded integer damage as GDL SafeRocks.
    const f32 amount =
        std::clamp(power - static_cast<f32>(rock.armor), 1.0f, static_cast<f32>(rock.health));
    // DC HitItem truncates after armor; N64 retains its existing shared combat rounding.
    rock.health -= m_platform == LegendsPlatform::Dreamcast ? static_cast<s32>(amount)
                                                            : static_cast<s32>(std::lround(amount));
    const s32 tier = rock.health == 0 ? 0 : (rock.health - 1) / rock.definition.tierHealth + 1;
    if (tier != rock.tier) {
        rock.tier = tier;
        // e0888f50 removes the body immediately, plays EXP when authored, and
        // then returns to L0. Some native arenas have no rubble/EXP sequence.
        selectArenaRock(index, tier == 0);
    }
    return true;
}

void LegendsSceneAdapter::scheduleArenaActivation(usize index, f32 delay) {
    if (index < m_arenaRocks.size() && std::isfinite(delay) && delay > 0) {
        m_arenaRocks[index].activationDelay = delay;
    }
}

void LegendsSceneAdapter::updateArena(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_bossEruption.update(seconds);
    for (usize index = 0; index < m_arenaRocks.size(); ++index) {
        auto& rock = m_arenaRocks[index];
        if (rock.activationDelay > 0) {
            rock.activationDelay -= seconds;
            if (rock.activationDelay <= 0) {
                rock.activationDelay = 0;
                rock.health = rock.definition.tierHealth * kRaisedTier;
                rock.tier = kRaisedTier;
                rock.dormant = false;
                selectArenaRock(index);
            }
        }
        if (!rock.dormant && rock.tier == 0 && rock.burstFigure &&
            rock.activeFigure == rock.burstFigure && m_fixtures[*rock.burstFigure].finished()) {
            selectArenaRock(index);
        }
    }
}

std::vector<CombatArenaTarget> LegendsSceneAdapter::arenaTargets() const {
    std::vector<CombatArenaTarget> result;
    for (usize index = 0; index < m_arenaRocks.size(); ++index) {
        const auto& rock = m_arenaRocks[index];
        const Vec3 rotation{0, rock.definition.yaw, 0};
        result.push_back({index,
                          m_platform == LegendsPlatform::Dreamcast
                              ? legendsDreamcastItemPlacement(rock.definition.position, rotation)
                              : itemPlacement(rock.definition.position, rotation),
                          !rock.dormant && rock.health > 0});
    }
    return result;
}

std::vector<ImportedTarget> LegendsSceneAdapter::arenaDamageTargets() const {
    std::vector<ImportedTarget> result;
    for (usize i = 0; i < m_arenaRocks.size(); ++i) {
        const auto& rock = m_arenaRocks[i];
        if (rock.dormant || rock.health <= 0) {
            continue;
        }
        const auto& definition = rock.definition;
        // Attacks reach a rock by its cylinder; it has no breakable faces of its own.
        result.push_back({kImportedArenaBase + static_cast<s32>(i),
                          definition.position,
                          definition.radius,
                          definition.height,
                          true,
                          true,
                          {}});
    }
    return result;
}

std::vector<Mat4> LegendsSceneAdapter::arenaAnchors() const {
    std::vector<Mat4> result;
    result.reserve(m_arenaRocks.size());
    for (const auto& rock : m_arenaRocks) {
        const Vec3 rotation{0, rock.definition.yaw, 0};
        result.push_back(m_platform == LegendsPlatform::Dreamcast
                             ? legendsDreamcastItemPlacement(rock.definition.position, rotation)
                             : itemPlacement(rock.definition.position, rotation));
    }
    return result;
}

std::vector<Obstacle> LegendsSceneAdapter::arenaObstacles() const {
    std::vector<Obstacle> result;
    for (const auto& rock : m_arenaRocks) {
        if (!rock.dormant && rock.health > 0) {
            result.push_back(arenaBody(rock.definition));
        }
    }
    return result;
}

std::vector<MissileStop> LegendsSceneAdapter::arenaMissileStops() const {
    std::vector<MissileStop> result;
    for (usize index = 0; index < m_arenaRocks.size(); ++index) {
        const auto& rock = m_arenaRocks[index];
        if (!rock.dormant && rock.health > 0) {
            result.push_back(MissileStop{.box = arenaBody(rock.definition),
                                         .rock = kImportedArenaBase + static_cast<s32>(index),
                                         .rockHealth = rock.health,
                                         .rockArmor = rock.armor});
        }
    }
    return result;
}

bool LegendsSceneAdapter::arenaBlocksSegment(const Vec3& from, const Vec3& to, f32 radius) const {
    return std::ranges::any_of(m_arenaRocks, [&](const auto& rock) {
        return !rock.dormant && rock.health > 0 &&
               arenaBody(rock.definition).blocksSegment(from, to, radius);
    });
}

} // namespace gdl::game
