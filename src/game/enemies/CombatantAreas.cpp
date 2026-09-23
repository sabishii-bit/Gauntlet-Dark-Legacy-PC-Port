#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
void Combatant::startArea(Actor& critter, s32 id, const AttackDefinition& damage) {
    const CombatEffectDefinition* sound = critter.stock->data.sound(damage.sound);
    if (sound == nullptr) {
        return;
    }
    // The shipped Spider Queen and Wraith areas all use a root parent with no
    // motion, morph or linked effects. Other policies need their own effect owner.
    constexpr u32 kRootAppearanceFlags = 1U | 2U | 4U;
    if ((sound->flags & 1U) == 0 || (sound->flags & ~kRootAppearanceFlags) != 0 ||
        damage.behaviorFlags != 0 || damage.speed != 0 || damage.morph >= 0 || sound->link >= 0) {
        log::warn("critter {}: unsupported attached area policy for {}", critter.stock->data.name(),
                  sound->tree);
        return;
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
                return;
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
    const Vec3 offset = damage.offset + sound->offset;
    const Vec2 angles{damage.pitch, damage.yaw};
    CritterArea area;
    area.local = CritterArea::placement(Mat4{1}, offset, angles);
    area.radius = damage.maxDistance * critter.scale;
    area.minDot = damage.minDot;
    area.damage = damage.damage * m_scales.damage;
    area.flags = damage.flags;
    area.secondsLeft = life;
    critter.areas.push_back(area);

    CombatCue cue;
    cue.critter = id;
    cue.tree = sound->shows() ? sound->tree : std::string{};
    cue.sound = sound->soundFor(m_realm);
    cue.position = Vec3{(modelTransform(critter) * area.local)[3]};
    cue.scale = sound->scale;
    cue.life = life;
    cue.follows = true;
    cue.shakes = (sound->flags & CombatEffectDefinition::kShakes) != 0;
    cue.rootAttachment = true;
    cue.nodeOffset = offset;
    cue.pitchYaw = angles;
    cue.loop = false;
    if (!cue.tree.empty() || !cue.sound.empty() || cue.shakes) {
        m_cues.push_back(cue);
    }
}

void Combatant::updateAreas(Actor& critter, s32 id, std::span<const EnemyView> players) {
    const Mat4 parent = modelTransform(critter);
    for (const CritterArea& area : critter.areas) {
        for (const EnemyView& player : players) {
            if (!area.touches(parent, player)) {
                continue;
            }
            CombatBlow blow;
            blow.player = player.player;
            blow.critter = id;
            blow.damage = area.damage;
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
