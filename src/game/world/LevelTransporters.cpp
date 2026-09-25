#include "game/world/LevelTransporters.h"

#include <array>
#include <bit>
#include <cmath>

#include "engine/core/Log.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
s32 parameter(const ItemInstance& instance, usize offset) {
    u32 value = 0;
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<u32>(instance.params[offset + byte]) << (byte * 8);
    }
    return std::bit_cast<s32>(value);
}
} // namespace

void LevelTransporters::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                             s32 players) {
    clear();
    const auto tree = items.trees.find("TRANS");
    if (tree) {
        m_tree = &items.trees.tree(*tree);
        m_textures.bind(items.trees.textureAnimations(), items.textures, device);
    }
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const auto& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            !shownToParty(instance.minPlayers, players)) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        if (info.type != kItemType) {
            continue;
        }
        Pad pad;
        pad.instance = static_cast<s32>(i);
        pad.id = parameter(instance, 0);
        pad.destinationId = parameter(instance, 4);
        pad.position = instance.position;
        pad.radius = info.radius;
        pad.height = info.height;
        pad.transform = itemPlacement(pad.position, instance.rotation);
        if (m_tree != nullptr && pad.model.bind(*m_tree, items.models, items.textures, device)) {
            pad.pose.rest(*m_tree);
            if (const auto sequence = m_tree->findSequence("ACTIVE")) {
                pad.animation.start(m_tree->sequences[*sequence], *sequence);
            }
        }
        m_pads.push_back(std::move(pad));
    }
    for (usize i = 0; i < m_pads.size(); ++i) {
        auto& pad = m_pads[i];
        for (usize j = 0; j < m_pads.size(); ++j) {
            if (i != j && m_pads[j].id == pad.destinationId) {
                pad.destination = j;
                break;
            }
        }
        if (!pad.destination) {
            log::warn("Transporter {}: missing destination {}", pad.id, pad.destinationId);
        }
    }
    animate(0.0f);
}

void LevelTransporters::clear() {
    m_pads.clear();
    m_tree = nullptr;
    m_textures.clear();
    m_frames = 0.0f;
}

void LevelTransporters::animate(f32 seconds) {
    m_frames += seconds * AnimationPlayer::kDefaultRate;
    const auto elapsed = static_cast<u32>(std::floor(m_frames));
    m_frames -= static_cast<f32>(elapsed);
    m_textures.step(elapsed);
    for (auto& pad : m_pads) {
        if (m_tree != nullptr && pad.animation.playing()) {
            pad.animation.advance(seconds, true);
            const auto sequence = pad.animation.sequence();
            const auto frame = static_cast<s32>(pad.animation.frame());
            pad.pose.evaluate(*m_tree, sequence, pad.animation.frame());
            pad.model.setFrame(sequence, frame);
            m_textures.apply(pad.model, *m_tree, sequence, frame);
        }
    }
}

void LevelTransporters::draw(RenderDevice& device, const Mat4& clip,
                             const WorldLighting& lighting) const {
    for (const auto& pad : m_pads) {
        if (pad.model.bound()) {
            pad.model.draw(device, clip, pad.transform, lighting, pad.pose.matrices());
        }
    }
}

std::optional<usize> LevelTransporters::contact(const Vec3& feet, f32 radius, f32 height) const {
    for (usize i = 0; i < m_pads.size(); ++i) {
        const auto& pad = m_pads[i];
        const Vec3 away = feet - pad.position;
        const f32 reach = radius + pad.radius;
        if (away.x * away.x + away.z * away.z < reach * reach &&
            feet.y <= pad.position.y + pad.height && feet.y + height >= pad.position.y) {
            return i;
        }
    }
    return std::nullopt;
}

bool LevelTransporters::visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                                const CameraView& view) {
    const Vec3 relative = position - camera.position;
    const f32 z = glm::dot(relative, camera.forward());
    const f32 horizontal = view.horizontalFov * 0.5f;
    const f32 vertical = std::atan(std::tan(horizontal) / view.aspect);
    // The entire destination sphere must fit, not just its centre.
    return z >= WorldCamera::kNear + radius && z <= WorldCamera::kFar - radius &&
           std::abs(glm::dot(relative, camera.right())) * std::cos(horizontal) <=
               z * std::sin(horizontal) - radius &&
           std::abs(glm::dot(relative, camera.up())) * std::cos(vertical) <=
               z * std::sin(vertical) - radius;
}

std::optional<Vec3> LevelTransporters::landing(usize source, f32 radius,
                                               const WorldCollision& collision,
                                               const WorldCamera& camera,
                                               const CameraView& view) const {
    if (source >= m_pads.size() || !m_pads[source].destination) {
        return std::nullopt;
    }
    Vec3 destination = m_pads[*m_pads[source].destination].position;
    if (!visible(destination, radius, camera, view)) {
        return std::nullopt;
    }
    const auto floor = collision.floorAt(destination, 4.0f, 10.0f);
    if (!floor) {
        return std::nullopt;
    }
    destination.y = floor->y;
    return destination;
}

std::string_view LevelTransporters::soundForRealm(s32 realm) {
    static constexpr std::array<std::string_view, 13> kSounds{"",
                                                              "S_TRANSPORTA",
                                                              "",
                                                              "S_TRANSPORTC",
                                                              "",
                                                              "",
                                                              "",
                                                              "S_TRANSPORTG",
                                                              "S_TRANSPORTH",
                                                              "S_TRANSPORTI",
                                                              "S_TRANSPORTJ",
                                                              "S_TRANSPORTK",
                                                              "S_TRANSPORTS3"};
    return realm >= 0 && static_cast<usize>(realm) < kSounds.size()
               ? kSounds[static_cast<usize>(realm)]
               : "";
}

} // namespace gdl::game
