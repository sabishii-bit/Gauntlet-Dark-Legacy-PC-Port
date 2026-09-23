#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
namespace {
/** The arena selector's piecewise distance; replacing it with sqrt can change near ties. */
f32 arenaDistance(const Vec3& delta) {
    const f64 high = std::max(std::abs(delta.x), std::abs(delta.z));
    const f64 low = std::min(std::abs(delta.x), std::abs(delta.z));
    if (low < 0.0001) {
        return static_cast<f32>(high);
    }
    constexpr std::array<f64, 8> kSlopes{0.064, 0.124, 0.181, 0.236, 0.287, 0.333, 0.376, 0.414};
    constexpr f64 kBandWidth = 0.125;
    for (usize i = 0; i < kSlopes.size(); ++i) {
        if (low <= high * kBandWidth * static_cast<f64>(i + 1)) {
            return static_cast<f32>(high + kSlopes[i] * low);
        }
    }
    return static_cast<f32>(high + kSlopes.back() * low);
}
} // namespace

bool Combatant::supportsArea(const AttackDefinition& damage, const CombatEffectDefinition* sound) {
    constexpr u32 kAttachedAppearanceFlags = 1U | 2U | 4U | 0x40U | 0x800U;
    return sound != nullptr && (sound->flags & ~kAttachedAppearanceFlags) == 0 &&
           ((sound->flags & 0x40U) == 0 || (sound->flags & 0x801U) == 0) &&
           damage.behaviorFlags == 0 && damage.speed == 0 && damage.morph < 0 &&
           damage.morphEnd < 0 && sound->link < 0;
}

std::optional<f32> Combatant::startArea(Actor& critter, s32 id, const AttackDefinition& damage,
                                        std::string_view node, std::optional<Mat4> worldParent) {
    const CombatEffectDefinition* sound = critter.stock->data.sound(damage.sound);
    if (sound == nullptr) {
        return std::nullopt;
    }
    // Other policies need a world-space/moving effect owner, not a guessed root.
    if (!supportsArea(damage, sound) || (worldParent.has_value() && sound->flags != 0)) {
        log::warn("critter {}: unsupported attached area policy for {}", critter.stock->data.name(),
                  sound->tree);
        return std::nullopt;
    }
    f32 life = sound->life;
    if (life <= 0) {
        // NULLFX has a zero-frame sequence at rate 30. Zero-frame effects use
        // thirty frames, not a zero-length hit or a missing visual's lifetime.
        life = 1.0f;
        if (sound->tree != "NULLFX") {
            const auto tree = critter.stock->archive.trees.find(sound->tree);
            if (!tree.has_value()) {
                log::warn("critter {}: no area effect tree {}", critter.stock->data.name(),
                          sound->tree);
                return std::nullopt;
            }
            const auto& sequences = critter.stock->archive.trees.tree(*tree).sequences;
            if (!sequences.empty()) {
                const auto& sequence = sequences.front();
                const s32 frames = sequence.frames > 0 ? sequence.frames : 30;
                const s32 rate = sequence.frameRate > 0 ? sequence.frameRate : 30;
                life = static_cast<f32>(frames * rate) * AnimationPlayer::kRateUnit;
            }
        }
    }
    // Root-parent SFXX policies replace the supplied DAMG position outright.
    // World-mode damage rotates its offset before reparenting to the stage node.
    const bool rootParent = (sound->flags & 0x801U) != 0;
    Vec3 offset = rootParent ? sound->offset : damage.offset + sound->offset;
    if (worldParent.has_value()) {
        offset =
            Vec3{modelTransform(critter) * Vec4{damage.offset, 0}} + sound->offset * critter.scale;
    }
    if ((sound->flags & 0x40U) != 0) {
        // Detached damage effects snapshot the hit node's world position. The
        // slam's hand can lift again without dragging the ground wave with it.
        const Vec3 position = Vec3{attachmentTransform(critter, node) * Vec4{sound->offset, 1}} +
                              Vec3{modelTransform(critter) * Vec4{damage.offset, 0}};
        // SfxSetMat copies the body's orientation, not the animated hand's.
        worldParent = glm::rotate(glm::translate(Mat4{1}, position), critter.yaw, Vec3{0, 1, 0});
        offset = Vec3{0};
    }
    const Vec2 angles{damage.pitch, damage.yaw};
    CritterArea area;
    area.worldParent = worldParent;
    if (!rootParent) {
        area.node = node;
    }
    area.local = CritterArea::placement(Mat4{1}, offset, angles);
    area.radius = damage.maxDistance * critter.scale;
    area.minDot = damage.minDot;
    area.damage = damage.damage * m_scales.damage;
    area.flags = damage.flags;
    area.secondsLeft = life;
    area.lifetime = life;
    area.expanding = damage.type != AttackDefinition::kAttachedArea;
    critter.areas.push_back(area);

    CombatCue cue;
    cue.critter = id;
    cue.tree = sound->shows() ? sound->tree : std::string{};
    cue.sound = sound->soundFor(m_realm);
    const Mat4 parent = worldParent.value_or(attachmentTransform(critter, area.node));
    cue.position = Vec3{(parent * area.local)[3]};
    cue.scale = sound->scale * (worldParent.has_value() ? critter.scale : 1.0f);
    cue.life = life;
    cue.follows = !worldParent.has_value();
    cue.shakes = (sound->flags & CombatEffectDefinition::kShakes) != 0;
    cue.rootAttachment = area.node.empty();
    if (!area.node.empty()) {
        cue.node = area.node;
    }
    cue.nodeOffset = offset;
    cue.pitchYaw = angles;
    cue.loop = false;
    if (worldParent.has_value()) {
        cue.placement = parent * area.local;
        cue.rootAttachment = false;
        cue.node.reset();
    }
    if (!cue.tree.empty() || !cue.sound.empty() || cue.shakes) {
        m_cues.push_back(cue);
    }
    return life;
}

void Combatant::eruptArena(Actor& critter, s32 id, const AttackDefinition& damage,
                           std::span<const EnemyView> players) {
    const EnemyView* player = viewOf(players, critter.target);
    const CombatArenaTarget* nearest = nullptr;
    f32 best = 1.0e21f;
    const auto count = static_cast<s32>(critter.arenaTargets.size());
    s32 selected = -1;
    for (s32 i = 0; i < count; ++i) {
        const auto& target = critter.arenaTargets[static_cast<usize>(i)];
        if (target.active) {
            continue;
        }
        if (player == nullptr) {
            // SafeRockNearestTarget (0x80035c04..c34) checks entry i but returns
            // the rotated index. Preserve that asymmetry: the returned rock can
            // already be active. Do not silently turn it into a corrected search.
            selected = (critter.lastArenaTarget + i + 1) % count;
            break;
        }
        const f32 distance = arenaDistance(Vec3{target.placement[3]} - player->position);
        if (distance < best) {
            selected = i;
            best = distance;
        }
    }
    critter.lastArenaTarget = selected;
    if (selected < 0) {
        return;
    }
    nearest = &critter.arenaTargets[static_cast<usize>(selected)];
    if (const auto life = startArea(critter, id, damage, {}, nearest->placement)) {
        // The stage becomes solid one 30 Hz effect tick before its carrier expires.
        constexpr f32 kEffectTick = 1.0f / 30.0f;
        m_arenaActivations.push_back({nearest->index, *life - kEffectTick});
    }
}

void Combatant::updateAreas(Actor& critter, s32 id, std::span<const EnemyView> players) {
    for (const CritterArea& area : critter.areas) {
        const Mat4 parent = area.worldParent.value_or(attachmentTransform(critter, area.node));
        for (const EnemyView& player : players) {
            if (!area.touches(parent, player)) {
                continue;
            }
            CombatBlow blow;
            blow.player = player.player;
            blow.critter = id;
            blow.damage = area.currentDamage();
            blow.flags = area.flags;
            if (blow.damage < 5) {
                constexpr u32 kHeavyHitFlags = 0x170;
                constexpr u32 kNoHitEffect = 0x1000000;
                blow.flags = (blow.flags & ~kHeavyHitFlags) | kNoHitEffect;
            }
            blow.origin = Vec3{(parent * area.local)[3]};
            const Vec3 away = player.position - blow.origin;
            const f32 distance = glm::length(Vec2{away.x, away.z});
            blow.direction = distance > 0 ? Vec3{away.x, 0, away.z} * (0.25f / distance) : Vec3{0};
            blow.area = true;
            blow.repeatGap = area.hitGap();
            m_blows.push_back(blow);
        }
    }
}
} // namespace gdl::game
