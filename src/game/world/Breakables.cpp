#include "game/world/Breakables.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/Chests.h"
#include "game/world/TargetAssist.h"

namespace gdl::game {

namespace {

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

/** How near the segment from `from` to `to` comes to `point`, across the ground, and how
 * far along it (nought to one) that is. */
f32 nearestOnGround(const Vec3& from, const Vec3& to, const Vec3& point, f32& along) {
    const Vec2 a{from.x, from.z};
    const Vec2 ab{to.x - from.x, to.z - from.z};
    const Vec2 ap{point.x - from.x, point.z - from.z};
    const f32 length = glm::dot(ab, ab);
    along = length > 1e-8f ? std::clamp(glm::dot(ap, ab) / length, 0.0f, 1.0f) : 0.0f;
    return glm::length(Vec2{point.x, point.z} - (a + ab * along));
}

} // namespace

bool Breakables::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                      const WorldCollision* collision, ItemArchive* realmItems) {
    clear();
    m_collision = collision;
    const auto authored = itemSupportWorld(layout, collision);
    m_infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= m_infos.size()) {
            continue;
        }
        const ItemInfo& info = m_infos[static_cast<usize>(instance.info)];
        const bool holds = info.type == ItemInfo::kContainer && info.subtype == kBarrel;
        const bool breaks =
            info.type == kBreakable && info.subtype >= kBarrel && info.subtype <= kPoison;
        if (!holds && !breaks) {
            continue;
        }
        auto barrel = std::make_unique<Barrel>();
        barrel->instance = static_cast<s32>(index);
        barrel->minPlayers = instance.minPlayers;
        barrel->health = std::max<s32>(info.hitPoints, 1);
        barrel->armor = info.armor;
        barrel->radius = info.radius > 0.0f ? info.radius : 1.0f;
        barrel->height = info.height > 0.0f ? info.height : 3.0f;
        barrel->collisionOffset = info.collisionOffset;
        if (holds) {
            barrel->kind = BreakableStrike::Kind::Holding;
            barrel->contents = paramS16(instance, 0);
            barrel->count = paramS16(instance, 4);
        } else if (info.subtype == kExploding) {
            barrel->kind = BreakableStrike::Kind::Exploding;
        } else if (info.subtype == kPoison) {
            barrel->kind = BreakableStrike::Kind::Poison;
        }
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        ItemArchive& art = itemArchiveForTree(items, name, realmItems);
        if (!barrel->figure.place(device, art, name, instance, collision)) {
            log::warn("Barrels: no figure {} in the item archive", name);
        }
        barrel->box = barrel->figure.obstacle(info);
        barrel->support.bind(instance, info, authored ? &*authored : nullptr, barrel->figure,
                             barrel->box);
        barrel->support.sync(collision, barrel->figure, barrel->box);
        m_barrels.push_back(std::move(barrel));
    }
    return !m_barrels.empty();
}

void Breakables::clear() {
    m_collision = nullptr;
    m_barrels.clear();
    m_infos.clear();
    m_seed = kSeedStart;
}

MissileTarget Breakables::target(usize index, s32 id) const {
    const auto& barrel = *m_barrels.at(index);
    MissileTarget result{id, barrel.figure.position(), barrel.radius, barrel.height};
    const bool explosive = barrel.kind == BreakableStrike::Kind::Exploding ||
                           barrel.kind == BreakableStrike::Kind::Poison;
    result.acquisition = TargetAssist::itemAcquisition(
        barrel.figure.transform(), barrel.collisionOffset, barrel.radius, barrel.height,
        explosive ? TargetAssist::kExplosiveDistanceScale : TargetAssist::kItemDistanceScale);
    return result;
}

void Breakables::syncFloors() {
    for (const auto& barrel : m_barrels) {
        if (!barrel->gone) {
            barrel->support.sync(m_collision, barrel->figure, barrel->box);
        }
    }
}

void Breakables::setPlayerCount(s32 players) {
    for (const std::unique_ptr<Barrel>& barrel : m_barrels) {
        barrel->shown = shownToParty(barrel->minPlayers, players);
    }
}

bool Breakables::standing(usize index) const {
    if (index >= m_barrels.size()) {
        return false;
    }
    const Barrel& barrel = *m_barrels[index];
    return barrel.shown && !barrel.gone && barrel.state == kWhole;
}

std::optional<usize> Breakables::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    std::optional<usize> first;
    f32 firstAlong = 2.0f;
    for (usize index = 0; index < m_barrels.size(); ++index) {
        if (!standing(index)) {
            continue;
        }
        const Barrel& barrel = *m_barrels[index];
        const Vec3& base = barrel.figure.position();
        f32 along = 0.0f;
        if (nearestOnGround(from, to, base, along) > barrel.radius + radius) {
            continue;
        }
        const f32 y = from.y + (to.y - from.y) * along;
        if (y + radius < base.y || y - radius > base.y + barrel.height) {
            continue;
        }
        if (along < firstAlong) {
            firstAlong = along;
            first = index;
        }
    }
    return first;
}

std::vector<usize> Breakables::within(const Vec3& centre, f32 radius) const {
    std::vector<usize> found;
    for (usize index = 0; index < m_barrels.size(); ++index) {
        if (!standing(index)) {
            continue;
        }
        const Barrel& barrel = *m_barrels[index];
        const Vec3 offset = barrel.figure.position() - centre;
        if (std::hypot(offset.x, offset.z) <= radius + barrel.radius &&
            std::abs(offset.y) <= radius + barrel.height) {
            found.push_back(index);
        }
    }
    return found;
}

std::optional<BreakableStrike> Breakables::strike(usize index, f32 power) {
    if (!standing(index)) {
        return std::nullopt;
    }
    Barrel& barrel = *m_barrels[index];
    // Armour comes off the blow, which still counts for one.
    if (barrel.armor >= 0) {
        f32 felt = power - static_cast<f32>(barrel.armor);
        if (felt <= 0.0f) {
            felt = 1.0f;
        }
        barrel.health = std::max(barrel.health - static_cast<s32>(std::lround(felt)), 0);
    }
    BreakableStrike result;
    result.index = index;
    result.kind = barrel.kind;
    result.position = barrel.figure.position();
    result.broken = barrel.health == 0;
    if (!result.broken) {
        return result;
    }
    barrel.state = kBreaking;
    barrel.box.solid = false;
    barrel.figure.play(kBreaking, false);
    if (barrel.kind == BreakableStrike::Kind::Holding) {
        result.contents = Chests::resolveContents(m_infos, barrel.contents,
                                                  static_cast<usize>(barrel.instance), m_seed);
        result.count = barrel.count;
    }
    return result;
}

std::vector<usize> Breakables::update(f32 seconds) {
    syncFloors();
    std::vector<usize> retired;
    for (usize index = 0; index < m_barrels.size(); ++index) {
        Barrel& barrel = *m_barrels[index];
        if (!barrel.shown || barrel.gone) {
            continue;
        }
        barrel.figure.update(seconds);
        if (barrel.state == kBreaking) {
            barrel.breakingSeconds += seconds;
        }
        if (barrel.state != kBreaking || !barrel.figure.finished()) {
            continue;
        }
        // One that blew up or gassed is gone once it has broken, its remains left by whoever
        // broke it (items.c's action 2: KILL_ITEM); a plain one leaves its staves lying.
        if (barrel.kind == BreakableStrike::Kind::Exploding ||
            barrel.kind == BreakableStrike::Kind::Poison) {
            barrel.gone = true;
            retired.push_back(index);
        } else {
            barrel.state = kBroken;
            barrel.figure.play(kBroken, true);
        }
    }
    return retired;
}

f32 Breakables::opacityOf(usize index) const {
    return opacity(*m_barrels[index]);
}

f32 Breakables::opacity(const Barrel& barrel) {
    if (barrel.state != kBreaking || (barrel.kind != BreakableStrike::Kind::Exploding &&
                                      barrel.kind != BreakableStrike::Kind::Poison)) {
        return 1.0f;
    }
    // It fades out as it breaks: 256 less 2.83 a tick from its sixteenth (lbl_803470A0).
    const f32 ticks = barrel.breakingSeconds * kTicksPerSecond;
    return std::clamp((kFadeFrom - kFadeRate * (ticks - kFadeStart)) / kOpaque, 0.0f, 1.0f);
}

std::vector<Obstacle> Breakables::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const std::unique_ptr<Barrel>& barrel : m_barrels) {
        if (barrel->shown && !barrel->gone && barrel->box.solid) {
            boxes.push_back(barrel->box);
        }
    }
    return boxes;
}

void Breakables::capturePresentation() {
    for (const auto& barrel : m_barrels) {
        barrel->figure.capturePresentation();
    }
}

void Breakables::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                      f32 presentationAlpha) const {
    for (const std::unique_ptr<Barrel>& barrel : m_barrels) {
        if (barrel->shown && !barrel->gone) {
            barrel->figure.draw(device, clip, lighting, opacity(*barrel), 1, nullptr,
                                TreeModel::Pass::All, presentationAlpha);
        }
    }
}

} // namespace gdl::game
