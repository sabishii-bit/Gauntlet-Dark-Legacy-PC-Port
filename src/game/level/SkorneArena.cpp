#include "game/level/SkorneArena.h"

#include <array>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Mt19937.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {
namespace {
constexpr s32 kEntranceRock = 51;
constexpr FallingProfile kProfile{}; ///< the masonry falls as most loose pieces do
} // namespace
bool SkorneArena::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("SkorneArena/1");
    digest.value(m_bottom);
    digest.value(saved ? saved->m_state : m_sim);
    digest.wide(m_rocks.size());
    for (usize i = 0; i < m_rocks.size(); ++i) {
        digest.value(saved ? saved->m_motions[i] : m_rocks[i].motion);
    }
    // Rock models are immutable bound-stage draw resources, not mutable simulation.
    return digest.good();
}

bool SkorneArena::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool SkorneArena::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [state, motions, binding, owner] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

void SkorneArena::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_motions.resize(m_rocks.size());
    for (usize index = 0; index < m_rocks.size(); ++index) {
        saved.m_motions[index] = m_rocks[index].motion;
    }
    saved.m_binding = m_localBinding;
    saved.m_owner = this;
}

bool SkorneArena::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_localBinding && saved.m_binding.lock() == m_localBinding &&
           saved.m_motions.size() == m_rocks.size();
}

bool SkorneArena::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state &&
           left.m_motions == right.m_motions;
}

bool SkorneArena::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    for (usize index = 0; index < m_rocks.size(); ++index) {
        m_rocks[index].motion = saved.m_motions[index];
    }
    return true;
}

void SkorneArena::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_rocks.clear();
    m_sim.phase = 0;
    m_sim.remainder = 0;
    m_sim.updateSeconds = 0;
    m_bottom = 0;
}
void SkorneArena::bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
                       TextureSet& textures) {
    clear();
    m_bottom = FallingPiece::bottomOf(layout);
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const auto& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        const auto subtype =
            static_cast<s32>(instance.params[0]) | (static_cast<s32>(instance.params[1]) << 8);
        if (info.type != ItemInfo::kObstacle ||
            (subtype > 0 ? subtype : info.subtype) != kEntranceRock) {
            continue;
        }
        TreeInfo tree;
        tree.name = instance.name;
        TreeNodeInfo node;
        node.name = instance.name;
        node.object = instance.name;
        node.objectFlags = info.objectFlags;
        tree.nodes.push_back(node);
        Rock rock;
        if (!rock.model.bind(tree, models, textures, device)) {
            continue;
        }
        rock.motion.place(instance.position, instance.rotation, i);
        m_rocks.push_back(std::move(rock));
    }
}
void SkorneArena::cue(const Vec3& boss) {
    if (m_sim.phase >= 3) {
        return;
    }
    ++m_sim.phase;
    for (auto& rock : m_rocks) {
        FallingPiece& motion = rock.motion;
        if (!motion.visible) {
            continue;
        }
        if (m_sim.phase == 3) {
            const Vec3 delta = motion.position - boss;
            const f32 length = glm::length(Vec2{delta.x, delta.z});
            motion.velocity = length > 0 ? Vec3{delta.x, 0, delta.z} * (10.0f / length) : Vec3{0};
        }
        constexpr std::array<f32, 3> kLifts{20, 30, 50};
        constexpr std::array<f32, 3> kSpreads{10, 15, 100};
        const f32 base = kLifts[static_cast<usize>(m_sim.phase - 1)];
        const f32 spread = kSpreads[static_cast<usize>(m_sim.phase - 1)];
        motion.velocity.y = base + Mt19937::float24(static_cast<u32>(m_sim.random())) * spread;
    }
}
void SkorneArena::update(f32 seconds) {
    m_sim.updateSeconds = seconds;
    if (m_sim.phase == 0) {
        return;
    }
    for (s32 frame = FallingPiece::framesDue(m_sim.remainder, seconds); frame > 0; --frame) {
        for (auto& rock : m_rocks) {
            rock.motion.advance(kProfile, m_bottom);
        }
    }
}
void SkorneArena::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       f32 presentationAlpha) const {
    const f32 alpha =
        FallingPiece::presentationFraction(m_sim.remainder, m_sim.updateSeconds, presentationAlpha);
    for (const auto& rock : m_rocks) {
        if (rock.motion.visible) {
            rock.model.draw(device, clip,
                            itemPlacement(rock.motion.presentedPosition(alpha),
                                          rock.motion.presentedRotation(alpha)),
                            lighting);
        }
    }
}
} // namespace gdl::game
