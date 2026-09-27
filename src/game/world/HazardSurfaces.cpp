#include "game/world/HazardSurfaces.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/players/PlayerImpact.h"

namespace gdl::game {

namespace {
constexpr u32 kHarmShift = 16;
constexpr u32 kHarmKinds = 0xF;
constexpr u32 kKnockingKind = 2;
constexpr u32 kFirstFellingKind = 3;
constexpr u32 kLastFellingKind = 5;
constexpr f32 kBurn = 5.0f;
constexpr f32 kKnock = 10.0f;
constexpr f32 kFell = 15.0f;
constexpr f32 kFloorProbe = 0.5f; ///< above and below the feet
constexpr f32 kEpsilon = 1e-4f;
} // namespace

std::optional<HazardSurfaces::Harm> HazardSurfaces::harmOf(u32 flags) {
    if ((flags & kHarmMask) == 0 ||
        ((flags & kTriggered) != 0 && (flags & kHarmsWhenTriggered) == 0)) {
        return std::nullopt;
    }
    // PlayerMotion_FloorFXDamage: a burn, a blow that knocks back, or one that fells.
    const u32 kind = (flags >> kHarmShift) & kHarmKinds;
    if (kind == kKnockingKind) {
        return Harm{kKnock, PlayerImpact::kKnockBack, false};
    }
    if (kind >= kFirstFellingKind && kind <= kLastFellingKind) {
        return Harm{kFell, PlayerImpact::kKnockDown, true};
    }
    return Harm{kBurn, 0, false};
}

std::optional<HazardSurfaces::Harm> HazardSurfaces::enemyHarmOf(u32 flags) {
    constexpr u32 kBurningKind = 1;
    const auto harm = harmOf(flags);
    if (!harm) {
        return std::nullopt;
    }
    const u32 kind = (flags >> kHarmShift) & kHarmKinds;
    if (kind == kKnockingKind) {
        return Harm{kBurn, PlayerImpact::kKnockBack, false};
    }
    if (kind == kBurningKind || (kind >= kFirstFellingKind && kind <= kLastFellingKind)) {
        return harm;
    }
    return std::nullopt;
}

void HazardSurfaces::bind(const WorldLayout& layout) {
    const std::vector<WorldObject>& objects = layout.objects();
    m_flags.assign(objects.size(), 0);
    for (usize i = 0; i < objects.size(); ++i) {
        u32 flags = 0;
        usize steps = 0;
        for (s32 at = static_cast<s32>(i);
             at >= 0 && static_cast<usize>(at) < objects.size() && steps <= objects.size();
             at = objects[static_cast<usize>(at)].parent, ++steps) {
            flags |= objects[static_cast<usize>(at)].flags;
        }
        m_flags[i] = flags;
    }
}

u32 HazardSurfaces::flagsOf(s32 object) const {
    return object >= 0 && static_cast<usize>(object) < m_flags.size()
               ? m_flags[static_cast<usize>(object)]
               : 0;
}

std::optional<HazardSurfaces::Harm> HazardSurfaces::harmOfObject(s32 object) const {
    return harmOf(flagsOf(object));
}

std::optional<HazardSurfaces::Touch> HazardSurfaces::touching(const WorldCollision& collision,
                                                              const Vec3& position, f32 radius,
                                                              f32 height) const {
    if (m_flags.empty()) {
        return std::nullopt;
    }
    std::vector<WallContact> contacts;
    collision.resolveWalls(position, radius + kReach, position.y + kFloorProbe,
                           position.y + std::max(height - kFloorProbe, kFloorProbe), &contacts);
    for (const WallContact& contact : contacts) {
        if (const auto harm = harmOfObject(contact.object)) {
            Vec3 away{position.x - contact.point.x, 0.0f, position.z - contact.point.z};
            const f32 length = glm::length(away);
            away = length > kEpsilon ? away / length : Vec3{0.0f};
            return Touch{contact.object, *harm, away};
        }
    }
    if (const auto floor = collision.floorAt(position, kFloorProbe, kFloorProbe)) {
        if (const auto harm = harmOfObject(floor->object)) {
            return Touch{floor->object, *harm, Vec3{0.0f}};
        }
    }
    return std::nullopt;
}

usize HazardSurfaces::harmful() const {
    return static_cast<usize>(
        std::ranges::count_if(m_flags, [](u32 flags) { return harmOf(flags).has_value(); }));
}

} // namespace gdl::game
