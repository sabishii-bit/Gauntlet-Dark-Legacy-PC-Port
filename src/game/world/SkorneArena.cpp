#include "game/world/SkorneArena.h"

#include <array>
#include <utility>

#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
constexpr s32 kObstacle = 10;
constexpr s32 kEntranceRock = 51;
constexpr FallingProfile kProfile{}; ///< the masonry falls as most loose pieces do
} // namespace
void SkorneArena::clear() {
    m_rocks.clear();
    m_phase = 0;
    m_remainder = 0;
    m_updateSeconds = 0;
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
        if (info.type != kObstacle || (subtype > 0 ? subtype : info.subtype) != kEntranceRock) {
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
    if (m_phase >= 3) {
        return;
    }
    ++m_phase;
    for (auto& rock : m_rocks) {
        FallingPiece& motion = rock.motion;
        if (!motion.visible) {
            continue;
        }
        if (m_phase == 3) {
            const Vec3 delta = motion.position - boss;
            const f32 length = glm::length(Vec2{delta.x, delta.z});
            motion.velocity = length > 0 ? Vec3{delta.x, 0, delta.z} * (10.0f / length) : Vec3{0};
        }
        constexpr std::array<f32, 3> kLifts{20, 30, 50};
        constexpr std::array<f32, 3> kSpreads{10, 15, 100};
        const f32 base = kLifts[static_cast<usize>(m_phase - 1)];
        const f32 spread = kSpreads[static_cast<usize>(m_phase - 1)];
        motion.velocity.y = base + std::uniform_real_distribution<f32>{0, spread}(m_random);
    }
}
void SkorneArena::update(f32 seconds) {
    m_updateSeconds = seconds;
    if (m_phase == 0) {
        return;
    }
    for (s32 frame = FallingPiece::framesDue(m_remainder, seconds); frame > 0; --frame) {
        for (auto& rock : m_rocks) {
            rock.motion.advance(kProfile, m_bottom);
        }
    }
}
void SkorneArena::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       f32 presentationAlpha) const {
    const f32 alpha =
        FallingPiece::presentationFraction(m_remainder, m_updateSeconds, presentationAlpha);
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
