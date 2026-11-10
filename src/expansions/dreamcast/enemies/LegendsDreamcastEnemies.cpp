#include "expansions/dreamcast/enemies/LegendsDreamcastEnemies.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/math/PortableMath.h"

#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
/** A placement parameter byte as the two's-complement number the native record stores. */
s32 signedByte(u8 byte) {
    return std::bit_cast<s8>(byte);
}
std::optional<s32> species(std::string_view name, s32 tier,
                           const formats::LegendsDreamcastFixtureScope& scope) {
    const auto named = enemyKindOf(name);
    if (!named || *named < 0 || *named > 19) {
        return {};
    }
    s32 kind = *named;
    // 1ST_READ.BIN 8c08ff40: RAT takes class 1; GRU/KNI prefer class 4
    // for ranged/suicide tiers, then class 2, then class 3. Other names are exact.
    if (kind == 3) {
        kind = scope.enemyClasses[1];
    } else if (kind == 4 || kind == 5) {
        if (tier >= 4 && scope.enemyClasses[4] >= 0) {
            kind = scope.enemyClasses[4];
        } else if (scope.enemyClasses[2] >= 0) {
            kind = scope.enemyClasses[2];
        } else if (scope.enemyClasses[3] >= 0) {
            kind = scope.enemyClasses[3];
        }
    }
    // DC's 0..19 are the corresponding GDL species. DC 20 is another knight,
    // not GDL's ghost. Keep unsupported extended native IDs out of common pools.
    if (kind == 20) {
        kind = 5;
    }
    return kind >= 0 && kind <= 19 ? std::optional<s32>{kind} : std::nullopt;
}
bool placementValid(const formats::ItemInstanceRecord& item) {
    return item.minPlayers >= 0 && item.minPlayers <= 4 && finite(item.position) &&
           finite(item.rotation) && std::abs(item.rotation.x) < .00001f &&
           std::abs(item.rotation.z) < .00001f;
}
} // namespace

std::unique_ptr<ItemArchive>
loadLegendsDreamcastGeneratorAssets(const std::filesystem::path& nativeRoot, s32 kind,
                                    const formats::LegendsDreamcastFixtureScope& scope,
                                    std::vector<std::string>& warnings) {
    if (kind < 0 || kind > 19) {
        return {};
    }
    const std::string prefix = toLowerAscii(enemyKind(kind).prefix);
    // LoadMonster 8c090000 selects "aux" for category 4 and numeric suffixes
    // above 10. The shipped GEN_* objects live only in base species banks;
    // never import an unselected base bank for an auxiliary-only roster.
    const bool selected = std::ranges::any_of(scope.enemySounds, [kind](const auto& enemy) {
        return (enemy.kind == kind || (enemy.kind == 20 && kind == 5)) && enemy.subtype != 4 &&
               enemy.subtype <= 10;
    });
    if (!selected) {
        warnings.push_back("Dreamcast generator library is not selected by this level: " + prefix +
                           "; using fallback artwork");
        return {};
    }
    try {
        if (auto archive = loadLegendsDreamcastArchive(nativeRoot / "monsters" / prefix)) {
            return archive;
        }
    } catch (const FormatError& error) {
        warnings.push_back(std::format("Dreamcast generator library {} is invalid: {}; "
                                       "using fallback artwork",
                                       prefix, error.what()));
        return {};
    } catch (const FileError& error) {
        warnings.push_back(std::format("Dreamcast generator library {} could not be read: {}; "
                                       "using fallback artwork",
                                       prefix, error.what()));
        return {};
    }
    warnings.push_back("Dreamcast generator library is unavailable: " + prefix +
                       "; using fallback artwork");
    return {};
}

std::unique_ptr<ItemArchive>
loadLegendsDreamcastEnemyAssets(const std::filesystem::path& nativeRoot, s32 kind,
                                std::vector<std::string>& warnings,
                                std::span<const LevelEnemy> roster) {
    if (kind < 0 || kind > 19) {
        return {};
    }
    // DC archives already use the common READY/WALK/START/ATTACK/HIT names
    // and A/B/S variant trees. No N64 four-character action translation applies.
    const std::string prefix = toLowerAscii(enemyKind(kind).prefix);
    std::string bank = prefix;
    s32 bodyTier = 1;
    // LoadMonster 8c090000 selects a single-tier bank for ENMY categories>10.
    // E1/F1 only contain GRU2/TRO2/... and TRE3/DEM3/... respectively; loading
    // the base species silently substitutes a different model and texture set.
    for (const auto& enemy : roster) {
        if (enemy.kind == kind && enemy.subtype > 10) {
            bodyTier = enemy.subtype - 10;
            if (bodyTier < 1 || bodyTier > 3) {
                warnings.push_back("Dreamcast monster library has an unsupported tier: " + prefix);
                return {};
            }
            bank += std::to_string(bodyTier);
            break;
        }
    }
    auto archive = loadLegendsDreamcastArchive(nativeRoot / "monsters" / bank);
    if (!archive) {
        warnings.push_back("Dreamcast monster library is unavailable: " + bank);
        return {};
    }
    const auto first =
        archive->trees.find(std::string(enemyKind(kind).prefix) + std::to_string(bodyTier));
    if (!first || !archive->trees.tree(*first).findSequence("READY")) {
        warnings.push_back("Dreamcast monster library has no compatible body: " + bank);
        return {};
    }
    return archive;
}

std::optional<LegendsDreamcastGenerator> legendsDreamcastGenerator(
    const formats::ItemInfoRecord& info, const formats::ItemInstanceRecord& instance,
    u32 nativeIndex, const formats::LegendsDreamcastFixtureScope& scope, std::string* diagnostic) {
    if (diagnostic) {
        diagnostic->clear();
    }
    if (info.type != 3) {
        return {};
    }
    const auto reject = [&](std::string_view text) -> std::optional<LegendsDreamcastGenerator> {
        if (diagnostic) {
            *diagnostic = text;
        }
        return {};
    };
    const bool mixed = scope.realmType == 5 || scope.realmType == 6;
    // SetItem 8c0a8d2a..8c0a8d62 overwrites the authored generator tier in
    // Temple/Hell before multiplying HP and choosing GEN_SPECIAL2/3.
    const s32 tier = mixed ? scope.realmType - 3 : std::max<s32>(signedByte(instance.params[0]), 1);
    const auto kind =
        mixed ? std::optional<s32>{scope.realmType == 5 ? 4 : 5} : species(info.name, 0, scope);
    if (!kind) {
        return reject("Dreamcast generator needs an unsupported species");
    }
    if (!placementValid(instance) || tier > 3 || (instance.flags & ~1U) != 0 ||
        (info.collisionType != 1 && info.collisionType != 3) || !std::isfinite(info.radius) ||
        info.radius <= 0 || info.radius > 32 || !std::isfinite(info.height) || info.height <= 0 ||
        info.height > 64 || !std::isfinite(info.xSize) || info.xSize < 0 || info.xSize > 32 ||
        !std::isfinite(info.zSize) || info.zSize < 0 || info.zSize > 32 ||
        !finite(info.collisionOffset) || info.hitPoints <= 0 || info.armor < 0) {
        return reject("Dreamcast generator has unsupported placement or contact parameters");
    }
    LegendsDreamcastGenerator out;
    auto& gameplay = out.gameplay;
    gameplay.nativeIndex = nativeIndex;
    gameplay.kind = *kind;
    gameplay.tier = tier;
    gameplay.position = instance.position;
    // The common generator pool builds itemPlacement, whose editor yaw sign
    // is opposite DC CreateYPR 8c0ece60. Pass its equivalent editor angle.
    gameplay.yaw = -instance.rotation.y;
    gameplay.minimumPlayers = static_cast<u8>(instance.minPlayers);
    gameplay.algorithm = signedByte(instance.params[2]);
    // GetGenMax 8c0aa5e0 uses current damage state, not GDL's authored cap.
    // SetGenSpeed 8c0aa540 / GenWait 8c0aa740 use a shared 30..90-tick
    // range, independent of tier. SetItem overwrites params[4] and never
    // reads params[6]; treating these as GDL cap/rate produced F1's cap 2.
    // Keep the port's GDL difficulty/mechanics rather than import DC's
    // separate adaptive player-performance scalar (8c287bf4).
    gameplay.breeding = ImportedGenerator::Breeding{.broodByState = {6, 4, 2},
                                                    .difficulty = scope.generatorDifficulty,
                                                    .broodDifficulty = {.25f, 3},
                                                    .broodBonus = scope.realmType == 5   ? .5f
                                                                  : scope.realmType == 6 ? 1.f
                                                                                         : 0.f,
                                                    .maximumBrood = 10,
                                                    .waitTicks = {30, 90},
                                                    .waitDifficulty = {.5f, 2},
                                                    .activeDistance = 50};
    gameplay.alwaysActive = (instance.flags & 1U) != 0 || (info.activeType & 0x40) != 0;
    if (mixed) {
        // generate_enemy 8c0914da..8c091518: one random(4), tables at
        // 8c14afb8/afc8 and 8c14afd8/afe8. Brood tier stays fixed after
        // the portal loses a visual/health stage, unlike ordinary generators.
        gameplay.broodKinds =
            scope.realmType == 5 ? std::vector<s32>{4, 1, 7, 10} : std::vector<s32>{5, 2, 8, 11};
        gameplay.broodAlgorithms =
            scope.realmType == 5 ? std::vector<s32>{7, 7, 0, 7} : std::vector<s32>{7, 30, 0, 0};
        gameplay.broodTier = tier;
    }
    auto& body = gameplay.body;
    body.type = ItemInfo::kGenerator;
    body.name = enemyKind(*kind).name;
    body.collisionType = info.collisionType;
    body.collisionFlags = static_cast<u32>(info.collisionFlags);
    body.collisionOffset = info.collisionOffset;
    body.radius = info.radius;
    body.height = info.height;
    body.xSize = info.xSize;
    body.zSize = info.zSize;
    // SetItem 8c0a8340 initializes HP=templateHP*tier. The common pool applies
    // the same per-state HP rule and the selected Dark Legacy difficulty scale.
    body.hitPoints = info.hitPoints;
    body.armor = info.armor;
    for (s32 state = 0; state <= tier; ++state) {
        out.models[static_cast<usize>(state)] =
            std::format("GEN_{}{}", mixed ? "SPECIAL" : enemyKind(*kind).prefix, state);
    }
    return out;
}

std::optional<ImportedActor>
legendsDreamcastActor(const formats::ItemInfoRecord& info,
                      const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                      const formats::LegendsDreamcastFixtureScope& scope, std::string* diagnostic) {
    if (diagnostic) {
        diagnostic->clear();
    }
    if (info.type != 4) {
        return {};
    }
    const auto reject = [&](std::string_view text) -> std::optional<ImportedActor> {
        if (diagnostic) {
            *diagnostic = text;
        }
        return {};
    };
    const s32 tier = signedByte(instance.params[0]);
    const auto kind = species(info.name, tier, scope);
    if (!kind) {
        return reject("Dreamcast placed actor needs an unsupported native species");
    }
    if (!placementValid(instance) || tier < 1 || tier > 6 || (instance.flags & ~1U) != 0) {
        return reject("Dreamcast placed actor has unsupported placement parameters");
    }
    ImportedActor actor;
    actor.nativeIndex = nativeIndex;
    actor.minimumPlayers = static_cast<u8>(instance.minPlayers);
    actor.spawn.kind = *kind;
    actor.spawn.tier = tier;
    actor.spawn.algorithm = signedByte(instance.params[2]);
    actor.spawn.position = instance.position;
    actor.idleYaw = actor.yaw = instance.rotation.y;
    actor.spawn.direction = {portable::sin(actor.yaw), 0, portable::cos(actor.yaw)};
    actor.spawn.placed = true;
    actor.spawn.priority = EnemySpawn::Priority::Visible;
    actor.spawn.sight = std::bit_cast<f32>(readU32LE(instance.params, 4));
    if (!std::isfinite(actor.spawn.sight) || actor.spawn.sight < 0 || actor.spawn.sight > 1024) {
        return reject("Dreamcast placed actor has an invalid sight radius");
    }
    // Native placed-actor update 8c0ac220 measures the complete 3D separation
    // against 50 (literal 8c0ac340), independently of the authored AI sight radius.
    actor.activationRadius = 50;
    actor.activationHeight = 0;
    return actor;
}
} // namespace gdl::game
