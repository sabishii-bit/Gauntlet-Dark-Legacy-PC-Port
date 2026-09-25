#include "game/players/ItemAttack.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
std::optional<ItemAttack> ItemAttack::select(const PowerupEffects& worn) {
    // PlayerMotion's forced-attack priority precedes its close-combat selector.
    constexpr f32 kBreathDot = 0.866f;
    if ((worn.special & (powerup::kSkorneHorns | powerup::kSkorneMask)) != 0) {
        return ItemAttack{PlayerDeed::Breathe,
                          "BOSS_BREATHE",
                          (worn.special & powerup::kSkorneHorns) != 0 ? "S_HORNS" : "S_MASK",
                          50,
                          20,
                          0,
                          kBreathDot,
                          33,
                          0,
                          0,
                          true};
    }
    if ((worn.special & powerup::kLeftGauntlet) != 0) {
        return ItemAttack{
            .deed = PlayerDeed::FireLeft, .tree = "BOSSG_ELEC", .sound = "S_GAUNTLET1", .flags = 2};
    }
    if ((worn.special & powerup::kRightGauntlet) != 0) {
        return ItemAttack{.deed = PlayerDeed::FireRight,
                          .tree = "BOSSG_ACID",
                          .sound = "S_GAUNTLET2",
                          .flags = 4};
    }
    if ((worn.weapon & powerup::kSuperShot) != 0) {
        return ItemAttack{.deed = PlayerDeed::SuperShot, .tree = "SUPERARROW", .sound = {}};
    }
    if ((worn.weapon & powerup::kThunderHammer) != 0) {
        return ItemAttack{PlayerDeed::Hammer,
                          "EXPRING",
                          "S_THUNDERHAMMER",
                          100,
                          35,
                          0.1f,
                          -1,
                          32,
                          powerup::kWeapon,
                          powerup::kThunderHammer,
                          false};
    }
    u32 element = 0;
    std::string_view tree;
    std::string_view sound;
    if ((worn.special & powerup::kFireBreath) != 0) {
        element = 1;
        tree = "FIREBREATHE";
        sound = "S_BREATHFIRE";
    } else if ((worn.special & powerup::kAcidBreath) != 0) {
        element = 4;
        tree = "ACIDBREATHE";
        sound = "S_BREATHGAS";
    } else if ((worn.special & powerup::kLightningBreath) != 0) {
        element = 2;
        tree = "ELECBREATHE";
        sound = "S_BREATHELEC";
    } else {
        return std::nullopt;
    }
    return ItemAttack{
        PlayerDeed::Breathe, tree, sound, 40, 20, 0, kBreathDot, 32 | element, powerup::kSpecial,
        powerup::kBreath,    true};
}

f32 ItemAttack::damageAt(f32 elapsed, f32 lifetime) const {
    if (elapsed < delay || lifetime <= 0 || elapsed >= lifetime) {
        return 0;
    }
    const f32 phase = lifetime <= 1.0f / 30 ? 1 : 1 - elapsed / lifetime;
    return phase > 0.33f ? damage * 1.5f * (phase - 0.33f) : 0;
}

bool ItemAttack::reaches(const Mat4& parent, const MissileTarget& target, f32 elapsed,
                         f32 lifetime) const {
    if (damageAt(elapsed, lifetime) <= 0) {
        return false;
    }
    const f32 phase = lifetime <= 1.0f / 30 ? 1 : 1 - elapsed / lifetime;
    const f32 activeRadius = radius * (1.33f - phase);
    const Vec3 delta = target.pointNear(Vec3{parent[3]}) - Vec3{parent[3]};
    const f32 distance = std::hypot(delta.x, delta.z);
    const f32 reach = activeRadius + (target.surface.empty() ? target.radius : 0.0f);
    if (!target.surface.empty() && !target.touches(Vec3{parent[3]}, activeRadius)) {
        return false;
    }
    if (distance > reach || std::abs(delta.y) > activeRadius + target.height * 0.5f) {
        return false;
    }
    if (minDot <= -1) {
        return true;
    }
    const Vec2 forward{parent[2].x, parent[2].z};
    const f32 length = glm::length(forward);
    const f32 dot = distance > 0 && length > 0
                        ? glm::dot(Vec2{delta.x, delta.z} / distance, forward / length)
                        : 0;
    return dot >= (distance < 0.3f * reach ? minDot * 0.85f : minDot);
}
} // namespace gdl::game
