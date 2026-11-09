#include <algorithm>
#include <cmath>
#include <format>

#include "engine/assets/AssetScope.h"
#include "engine/core/Error.h"

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/level/BossKinds.h"
#include "game/level/ImportedTargets.h"
#include "game/level/MissileStop.h"

namespace gdl::game {
namespace {
Obstacle arenaBody(const ImportedBoss::ArenaRock& definition, Vec2 boxHalfExtents) {
    Obstacle body;
    body.centre = definition.position;
    body.yaw = definition.yaw;
    const bool box = boxHalfExtents.x > 0 && boxHalfExtents.y > 0;
    body.cylinderRadius = box ? 0 : definition.radius;
    body.halfAcross = boxHalfExtents.x;
    body.halfAlong = boxHalfExtents.y;
    body.height = definition.height;
    body.enemyItem = Obstacle::ItemQuery{!box, definition.radius};
    return body;
}
} // namespace

void LegendsSceneAdapter::bindN64Arena(RenderDevice& device, const ImportedBoss& boss) {
    if (boss.arenaRocks.size() > 16 || !m_arenaRocks.empty()) {
        throw FormatError("N64 Legends arena: invalid cover roster");
    }
    m_hasBossEntrance = boss.kind == boss_type::kSkorne1;
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    for (const auto& definition : boss.arenaRocks) {
        if (!std::isfinite(definition.position.x) || !std::isfinite(definition.position.y) ||
            !std::isfinite(definition.position.z) || !std::isfinite(definition.yaw) ||
            !std::isfinite(definition.radius) || !std::isfinite(definition.height) ||
            definition.radius <= 0 || definition.height <= 0 || definition.tierHealth < 1 ||
            definition.tierHealth > 100000) {
            throw FormatError("N64 Legends arena: invalid native cover body");
        }
        ArenaRockPresentation binding;
        binding.definition = definition;
        binding.health = definition.tierHealth * 3;
        // Native Boss.bin e0888cd4 starts every rock at L3. The imported Yeti
        // intentionally uses GDL's dormant/eruption mechanic, retaining the native
        // authored anchor roster instead of the counterpart stage's positions.
        binding.dormant = boss.kind == boss_type::kYeti;
        ItemInstance placement;
        placement.position = definition.position;
        placement.rotation.y = definition.yaw;
        const auto bind = [&](std::string_view sequence, bool required) -> std::optional<usize> {
            const auto name = formats::legendsN64TreeName("SAFEROCK", sequence);
            if (!scope.findTree(name)) {
                if (required) {
                    throw FormatError("N64 Legends arena: missing " + name);
                }
                return std::nullopt;
            }
            const usize index = m_fixtures.size();
            place(device, name, placement);
            if (m_fixtures.size() == index) {
                throw FormatError("N64 Legends arena: cannot bind " + name);
            }
            m_hiddenFixtures[index] = true;
            return index;
        };
        for (usize tier = 0; tier < binding.figures.size(); ++tier) {
            binding.figures[tier] = bind(std::format("L{}", tier), tier != 0);
        }
        binding.burstFigure = bind("EXP", false);
        m_arenaRocks.push_back(binding);
        selectArenaRock(m_arenaRocks.size() - 1);
    }
}

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
                rock.health = rock.definition.tierHealth * rock.initialTier;
                rock.tier = rock.initialTier;
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

void LegendsSceneAdapter::beginBossEntrance() {
    m_bossEruption.begin();
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
        if (rock.dormant || rock.health <= 0)
            continue;
        const auto& definition = rock.definition;
        result.push_back({kImportedArenaBase + static_cast<s32>(i), definition.position,
                          definition.radius, definition.height, true, true, rock.surface});
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
            result.push_back(arenaBody(rock.definition, rock.boxHalfExtents));
        }
    }
    return result;
}

std::vector<MissileStop> LegendsSceneAdapter::arenaMissileStops() const {
    std::vector<MissileStop> result;
    for (usize index = 0; index < m_arenaRocks.size(); ++index) {
        const auto& rock = m_arenaRocks[index];
        if (!rock.dormant && rock.health > 0) {
            result.push_back(MissileStop{.box = arenaBody(rock.definition, rock.boxHalfExtents),
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
               arenaBody(rock.definition, rock.boxHalfExtents).blocksSegment(from, to, radius);
    });
}

} // namespace gdl::game
