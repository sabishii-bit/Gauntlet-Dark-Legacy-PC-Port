#include "expansions/shared/enemies/LegendsActors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>

#include "engine/math/PortableMath.h"

#include "expansions/n64/critters/LegendsCritters.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
const TreeInfo* idleTree(const ImportedActor& actor, ItemArchive& archive) {
    const auto find = [&](std::string_view name) -> const TreeInfo* {
        const auto index = archive.trees.find(name);
        return index ? &archive.trees.tree(*index) : nullptr;
    };
    if (actor.spawn.kind == kGolemEnemyKind) {
        return find("GOL_STATUE");
    }
    if (actor.spawn.kind == kItKind) {
        // Native e005a8e4 and e005ac34 use this same billboard both before
        // activation and while chasing. There is no corresponding GC body.
        if (const auto* tree = find("IT1")) {
            return tree;
        }
        return find("IT:WALK");
    }
    const auto prefix = enemyKind(actor.spawn.kind).prefix;
    // Exactly the body lookup used by Enemies::loadKind/initialise, including
    // the A/B/S variants. Missing variants retain their corresponding base tier.
    if (actor.spawn.tier >= 4 && actor.spawn.tier <= 6) {
        constexpr std::array kSuffixes{"A", "B", "S"};
        if (const auto* tree = find(
                std::format("{}{}", prefix, kSuffixes[static_cast<usize>(actor.spawn.tier - 4)]))) {
            return tree;
        }
    }
    const s32 tier = actor.spawn.tier < 4    ? std::clamp(actor.spawn.tier, 1, 3)
                     : actor.spawn.tier == 6 ? 1
                                             : 2;
    for (s32 t = tier; t >= 1; --t) {
        if (const auto* tree = find(std::format("{}{}", prefix, t))) {
            return tree;
        }
    }
    for (s32 t = 1; t <= 3; ++t) {
        if (const auto* tree = find(std::format("{}{}", prefix, t))) {
            return tree;
        }
    }
    return nullptr;
}
} // namespace

void LegendsActors::open(RenderDevice& device, std::span<const ImportedActor> actors,
                         const ArchiveFor& archiveFor, const WorldCollision* collision) {
    clear();
    m_collision = collision;
    m_actors.reserve(actors.size());
    for (const auto& descriptor : actors) {
        if (descriptor.spawn.kind < 0 || descriptor.spawn.kind >= kEnemyKindCount ||
            descriptor.spawn.tier < 1 || descriptor.spawn.tier > 6 ||
            !finite(descriptor.spawn.position) || !std::isfinite(descriptor.idleYaw) ||
            !std::isfinite(descriptor.activationRadius) || descriptor.activationRadius <= 0 ||
            !std::isfinite(descriptor.activationHeight) || descriptor.activationHeight < 0) {
            m_warnings.push_back(std::format("Invalid imported actor {}", descriptor.nativeIndex));
            continue;
        }
        Actor actor;
        actor.descriptor = descriptor;
        actor.initialPosition = descriptor.spawn.position;
        auto* archive = archiveFor ? archiveFor(descriptor) : nullptr;
        const auto* tree = archive != nullptr ? idleTree(descriptor, *archive) : nullptr;
        std::optional<u32> sequence;
        if (tree != nullptr) {
            if (descriptor.spawn.kind == kGolemEnemyKind && !tree->sequences.empty()) {
                sequence = 0; // GDL GOL_STATUE's established dormant pose, not ACTIVE.
            } else {
                sequence = tree->findSequence(descriptor.spawn.kind == kItKind ? "WALK" : "READY");
            }
        }
        ItemInstance instance;
        instance.position = descriptor.spawn.position;
        // Item placement uses Ry(-item.yaw), while enemy body facing is Ry(yaw).
        instance.rotation.y = -descriptor.idleYaw;
        if (tree != nullptr && sequence &&
            actor.figure.place(device, *archive, tree->name, instance, nullptr)) {
            actor.figure.play(static_cast<s32>(*sequence), ItemFigure::Playback::Loop);
        } else {
            // Missing optional native It art must not replace it with another
            // species or prevent its common tagging rules from running.
            m_warnings.push_back(std::format("Imported actor {} has no pending artwork for {}",
                                             descriptor.nativeIndex,
                                             enemyKind(descriptor.spawn.kind).name));
        }
        m_actors.push_back(std::move(actor));
    }
    syncFloors();
}

void LegendsActors::clear() {
    m_actors.clear();
    m_warnings.clear();
    m_collision = nullptr;
}

void LegendsActors::syncFloors() {
    for (auto& actor : m_actors) {
        if (!actor.pending) {
            continue;
        }
        Vec3 position = actor.initialPosition;
        if (m_collision != nullptr && actor.descriptor.floorObject >= 0) {
            // Imported world transforms are deltas applied to already-world-space
            // native vertices. Retain the exact selected floor owner, not overlap.
            if (const auto motion = m_collision->objectTransform(actor.descriptor.floorObject)) {
                position = Vec3{*motion * Vec4{actor.initialPosition, 1}};
            }
        }
        actor.descriptor.spawn.position = position;
        if (actor.figure.hasFigure()) {
            actor.figure.placeAt(portable::rotate(glm::translate(Mat4{1}, position),
                                                  actor.descriptor.idleYaw, Vec3{0, 1, 0}));
        }
    }
}

void LegendsActors::update(f32 seconds, std::span<const EnemyView> players, bool timeStopped,
                           const Spawn& spawn) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return;
    }
    for (auto& actor : m_actors) {
        if (actor.pending || actor.liveSlot >= 0) {
            actor.figure.capturePresentation();
        }
    }
    syncFloors();
    for (auto& actor : m_actors) {
        if (actor.pending && spawn && legendsActorReady(actor.descriptor, players, timeStopped) &&
            spawn(actor.descriptor)) {
            actor.pending = false;
        }
        if ((actor.pending || actor.liveSlot >= 0) && !timeStopped && actor.figure.hasFigure()) {
            actor.figure.update(seconds);
        }
    }
}

void LegendsActors::bindLive(u32 nativeIndex, s32 slot, u64 instance) {
    if (slot < 0 || instance == 0) {
        return;
    }
    const auto found = std::ranges::find_if(m_actors, [&](const Actor& actor) {
        return actor.pending && actor.descriptor.nativeIndex == nativeIndex &&
               actor.descriptor.spawn.kind == kItKind;
    });
    if (found != m_actors.end() && found->figure.hasFigure()) {
        found->liveSlot = slot;
        found->liveInstance = instance;
    }
}

void LegendsActors::syncLive(const Enemies& enemies) {
    for (auto& actor : m_actors) {
        if (actor.pending || actor.liveSlot < 0) {
            continue;
        }
        const auto state = enemies.presentation(actor.liveSlot);
        if (!state || state->instance != actor.liveInstance || state->kind != kItKind ||
            !enemies.alive(actor.liveSlot) || enemies.dying(actor.liveSlot)) {
            actor.liveSlot = -1;
            actor.liveInstance = 0;
            continue;
        }
        // Enemy AI advances at 30 Hz. Match its end-of-this-60-Hz-tick
        // interpolation, then ItemFigure interpolates these successive poses.
        // Drawing never advances native animation or mutates the swarm.
        const f32 blend = std::clamp(state->blendEnd, 0.0f, 1.0f);
        const Vec3 position = Enemies::supportedPresentation(
            m_collision, state->previousPosition, state->position, blend, EnemyAction::Walk, true);
        const f32 yaw =
            state->previousYaw + TreePose::wrapAngle(state->yaw - state->previousYaw) * blend;
        actor.figure.placeAt(
            portable::rotate(glm::translate(Mat4{1}, position), yaw, Vec3{0, 1, 0}));
    }
}

void LegendsActors::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera, TreeModel::Pass pass,
                         f32 presentationAlpha) const {
    for (const auto& actor : m_actors) {
        if ((actor.pending || actor.liveSlot >= 0) && actor.figure.hasFigure()) {
            actor.figure.draw(device, clip, lighting, 1, 1, camera, pass, presentationAlpha);
        }
    }
}

} // namespace gdl::game
