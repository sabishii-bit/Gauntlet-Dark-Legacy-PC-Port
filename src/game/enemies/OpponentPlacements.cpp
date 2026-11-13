#include "game/enemies/OpponentPlacements.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/math/PortableMath.h"

#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"

namespace gdl::game {
namespace {
constexpr f32 kCarryReach = 2;
constexpr f32 kCarryRise = 3;

f32 sightParamOf(const ItemInstance& instance) {
    constexpr usize kSightOffset = 4;
    f32 sight = 0;
    if (instance.params.size() >= kSightOffset + sizeof(sight)) {
        std::memcpy(&sight, &instance.params[kSightOffset], sizeof(sight));
    }
    return sight;
}
} // namespace

std::vector<PlacedEnemies::Placement>
OpponentPlacements::load(RenderDevice& device, const WorldLayout& layout,
                         std::span<const LevelEnemy> roster, s32 party, s32 present,
                         Enemies& enemies, Critters& critters, CritterStatues& statues,
                         const WorldCollision* collision, const ClaimItem& claim) {
    std::vector<PlacedEnemies::Placement> placements;
    const auto authored = itemSupportWorld(layout, collision);
    const auto& infos = layout.itemInfos();
    for (const auto& instance : layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kPlacedEnemy || !shownToParty(instance.minPlayers, party)) {
            continue;
        }
        const auto named = enemyKindOf(info.name);
        const s32 strength = Generators::paramOf(instance, 0);
        if (!named) {
            continue;
        }
        const s32 kind = levelKindOf(roster, *named, strength);
        const Mat4 stood = itemPlacement(instance.position, instance.rotation);
        PlacedEnemies::Placement placement;
        placement.kind = kind;
        placement.facing = portable::atan2(stood[2][0], stood[2][2]);
        placement.viewRadius = 2 * std::max(info.radius, info.height);
        placement.sight = sightParamOf(instance);
        placement.spawn.position = instance.position;
        // AddItemSub sets the item on the floor about it (FloorPos, four up and ten down) and
        // parents it to a moving one, so a placement on a flying platform (the Grounds'
        // general) is seen and born where the platform is, not where it was authored.
        if (collision != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
            // Animated stumps may already start lowered by the time opponents
            // load. Bind to the floor at the authored pose, as other item riders
            // do, then let PlacedEnemies use its current transform for admission.
            const auto* support =
                itemSupportAt(instance.position, collision, authored ? &*authored : nullptr);
            if (const auto floor = support != nullptr
                                       ? support->restingFloorAt(instance.position, 4, 10, 1)
                                       : std::nullopt) {
                placement.spawn.position.y = floor->y + ItemFigure::kFloorLift;
                if (const auto transform = support->objectTransform(floor->object)) {
                    Mat4 placed = stood;
                    placed[3] = Vec4{placement.spawn.position, 1};
                    placement.support = floor->object;
                    placement.supportLocal = glm::inverse(*transform) * placed;
                }
            }
        }
        const bool great =
            kind == kGolemEnemyKind || kind == kGeneralEnemyKind || kind == kGargoyleEnemyKind;
        if (great) {
            // Generals also preload offscreen; the presentation catalog is fixed
            // before online snapshots. Their unused spawn.kind keeps its native default.
            if (kind == kGeneralEnemyKind) {
                critters.archiveFor(CombatantKind::General);
            }
            placement.carried = claim(instance.position, kCarryReach, kCarryRise);
            CombatantKind family = CombatantKind::Unknown;
            if (kind == kGolemEnemyKind) {
                family = CombatantKind::Golem;
            } else if (kind == kGargoyleEnemyKind) {
                family = CombatantKind::Gargoyle;
            }
            if (family != CombatantKind::Unknown) {
                CritterStatues::Placement statue;
                statue.kind = family;
                statue.instance = instance;
                statue.onFloor = (info.collisionFlags & ItemInfo::kKeepsHeight) == 0;
                statue.radius = info.radius;
                statue.height = info.height;
                statue.viewRadius = placement.viewRadius;
                statue.sight = placement.sight;
                statue.activeOn = info.activeOn;
                statue.carried = placement.carried;
                statue.support = placement.support;
                statue.supportLocal = placement.supportLocal;
                ItemArchive* archive = critters.archiveFor(family);
                if (archive != nullptr && statues.add(device, *archive, statue, collision)) {
                    continue;
                }
            }
            placements.push_back(placement);
            continue;
        }
        const auto row = ordinary(instance, info, roster, party, present,
                                  [&](s32 family) { return enemies.loadKind(family); });
        if (!row) {
            continue;
        }
        // Keep the floor placement/attachment resolved above. Replacing the
        // whole row here discarded it for ordinary archers and bombers only.
        const Vec3 settled = placement.spawn.position;
        placement.spawn = row->spawn;
        placement.spawn.position = settled;
        if (kind == kDeathKind) {
            // SetItem: 0/1 are red and 2/3 black; odd values start awakened.
            placement.spawn.tier = strength >= 2 ? 2 : 1;
            CritterStatues::Placement statue;
            statue.enemy = placement.spawn;
            statue.instance = instance;
            statue.onFloor = (info.collisionFlags & ItemInfo::kKeepsHeight) == 0;
            statue.radius = info.radius;
            statue.height = info.height;
            statue.viewRadius = placement.viewRadius;
            statue.sight = placement.sight;
            statue.activeOn = info.activeOn;
            statue.support = placement.support;
            statue.supportLocal = placement.supportLocal;
            ItemArchive* archive = enemies.archive(kDeathKind);
            if (archive != nullptr && statues.add(device, *archive, statue, collision)) {
                if ((strength & 1) != 0) {
                    statues.wake(statues.count() - 1);
                }
                continue;
            }
        }
        placements.push_back(placement);
    }
    return placements;
}

std::optional<PlacedEnemies::Placement>
OpponentPlacements::ordinary(const ItemInstance& instance, const ItemInfo& info,
                             std::span<const LevelEnemy> roster, s32 party, s32 present,
                             const LoadKind& load) {
    if (info.type != ItemInfo::kPlacedEnemy || !shownToParty(instance.minPlayers, party)) {
        return std::nullopt;
    }
    const auto named = enemyKindOf(info.name);
    if (!named) {
        return std::nullopt;
    }
    const s32 strength = Generators::paramOf(instance, 0);
    const s32 kind = levelKindOf(roster, *named, strength);
    // IT waits for company among the players present, whatever party the level is set up for
    // (gNumPlayers at 0x800601B0); great creatures use the separate statue/carried-item
    // loader, not this ordinary swarm-row path.
    const EnemyTraits* traits = enemyTraits(kind);
    const bool swarmPool = traits != nullptr && (traits->home == EnemyHome::Swarm ||
                                                 traits->home == EnemyHome::PlacedOnly);
    if ((kind == kItKind && present <= 1) || !swarmPool || !load || !load(kind)) {
        return std::nullopt;
    }
    const Mat4 stood = itemPlacement(instance.position, instance.rotation);
    PlacedEnemies::Placement placement;
    placement.kind = kind;
    placement.facing = portable::atan2(stood[2][0], stood[2][2]);
    placement.viewRadius = 2 * std::max(info.radius, info.height);
    placement.sight = sightParamOf(instance);
    auto& spawn = placement.spawn;
    spawn.kind = kind;
    spawn.tier = std::max(strength, 1);
    spawn.algorithm = Generators::paramOf(instance, 1);
    // SetItem stores the interval at byte eight, in tenths of a second. One
    // removes the wait, whereas zero preserves EnemySpawn's default.
    if (const s32 interval = Generators::paramOf(instance, 4); interval > 0) {
        spawn.throwInterval = interval == 1 ? 0.0f : static_cast<f32>(interval) * 0.1f;
    }
    spawn.position = instance.position;
    spawn.direction = Vec3{stood[2][0], 0, stood[2][2]};
    spawn.sight = placement.sight;
    spawn.placed = true;
    spawn.priority = EnemySpawn::Priority::Visible;
    spawn.asleep = strength == 0 && kind != kDeathKind;
    return placement;
}

} // namespace gdl::game
