#include "game/world/SkorneArena.h"

#include <array>
#include <utility>

#include "engine/core/Types.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
constexpr s32 kObstacle = 10;
constexpr s32 kEntranceRock = 51;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr f32 kGravityStep = 2;
constexpr f32 kSpin = 0.34906585f;
constexpr f32 kDiscardDepth = 200;
constexpr f32 kFloorSentinelMargin = 4.5f;
constexpr std::array<f32, 8> kSpinDirections{-4, -3, -2, -1, 1, 2, 3, 4};
} // namespace
void SkorneArena::clear() {
    m_rocks.clear();
    m_phase = 0;
    m_remainder = 0;
    m_bottom = 0;
}
void SkorneArena::bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
                       TextureSet& textures) {
    clear();
    // The floor-query sentinel is 4.5 below worldmin; debris retires 200 below it.
    m_bottom = layout.minBounds().y - kFloorSentinelMargin - kDiscardDepth;
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
        rock.position = instance.position;
        rock.rotation = instance.rotation;
        rock.instance = i;
        m_rocks.push_back(std::move(rock));
    }
}
void SkorneArena::cue(const Vec3& boss) {
    if (m_phase >= 3) {
        return;
    }
    ++m_phase;
    for (auto& rock : m_rocks) {
        if (!rock.visible) {
            continue;
        }
        if (m_phase == 3) {
            const Vec3 delta = rock.position - boss;
            const f32 length = glm::length(Vec2{delta.x, delta.z});
            rock.velocity = length > 0 ? Vec3{delta.x, 0, delta.z} * (10.0f / length) : Vec3{0};
        }
        constexpr std::array<f32, 3> kLifts{20, 30, 50};
        constexpr std::array<f32, 3> kSpreads{10, 15, 100};
        const f32 base = kLifts[static_cast<usize>(m_phase - 1)];
        const f32 spread = kSpreads[static_cast<usize>(m_phase - 1)];
        rock.velocity.y = base + std::uniform_real_distribution<f32>{0, spread}(m_random);
    }
}
void SkorneArena::update(f32 seconds) {
    if (m_phase == 0) {
        return;
    }
    m_remainder += seconds;
    while (m_remainder + 0.000001f >= kStep) {
        m_remainder -= kStep;
        for (auto& rock : m_rocks) {
            if (!rock.visible) {
                continue;
            }
            rock.velocity.y -= kGravityStep;
            rock.rotation.x += kSpin * kSpinDirections[rock.instance & 7U] * kStep;
            rock.rotation.z += kSpin * kSpinDirections[(~rock.instance) & 7U] * kStep;
            rock.position += rock.velocity * kStep;
            rock.visible = rock.position.y >= m_bottom;
        }
    }
}
void SkorneArena::draw(RenderDevice& device, const Mat4& clip,
                       const WorldLighting& lighting) const {
    for (const auto& rock : m_rocks) {
        if (rock.visible) {
            rock.model.draw(device, clip, itemPlacement(rock.position, rock.rotation), lighting);
        }
    }
}
} // namespace gdl::game
