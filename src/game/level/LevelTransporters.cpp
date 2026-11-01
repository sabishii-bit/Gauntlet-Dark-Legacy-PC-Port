#include "game/level/LevelTransporters.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"

namespace gdl::game {
namespace {
template <class T>
constexpr bool kExtraPadMember =
    requires { T{LevelTransporters::Contact{}, TreePose{}, AnimationPlayer{}, f32{}, u64{}, {}}; };

s32 parameter(const ItemInstance& instance, usize offset) {
    u32 value = 0;
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<u32>(instance.params[offset + byte]) << (byte * 8);
    }
    return std::bit_cast<s32>(value);
}
} // namespace

void LevelTransporters::appendBinding(NativeStateDigest& digest) const {
    digest.string("LevelTransporters.native.1");
    digest.resource(m_tree);
    digest.resource(m_layout);
    digest.resource(m_archive);
    digest.resource(m_collision);
    digest.value(m_players);
    m_textures.appendBindingDigest(digest);
}

void LevelTransporters::appendPad(NativeStateDigest& digest, const PadState& pad) {
    // C++ decomposition cannot span a base plus derived fields; the aggregate
    // arity checks protect this exact native extension from silent omissions.
    static_assert(requires { PadState{Contact{}, TreePose{}, AnimationPlayer{}, f32{}, u64{}}; });
    static_assert(!kExtraPadMember<PadState>);
    digest.fields<11>(static_cast<const Contact&>(pad));
    digest.values(pad.pose, pad.animation, pad.previousFrame, pad.previousGeneration);
}

bool LevelTransporters::appendNativeDigest(NativeStateDigest& digest) const {
    appendBinding(digest);
    digest.fields<3>(m_textures.clock());
    digest.value(m_presentationAdvanced);
    digest.wide(m_pads.size());
    for (const auto& pad : m_pads) {
        appendPad(digest, pad);
    }
    return digest.good();
}

bool LevelTransporters::appendLocalDigest(NativeStateDigest& digest,
                                          const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    const auto& [pads, textures, advanced, owner, binding] = saved;
    appendBinding(digest);
    digest.fields<3>(textures);
    digest.value(advanced);
    digest.wide(pads.size());
    for (const auto& pad : pads) {
        appendPad(digest, pad);
    }
    return digest.good();
}

void LevelTransporters::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_checkpointBinding;
    saved.m_textures = m_textures.clock();
    saved.m_presentationAdvanced = m_presentationAdvanced;
    saved.m_pads.resize(m_pads.size());
    for (usize i = 0; i < m_pads.size(); ++i) {
        saved.m_pads[i] = static_cast<const PadState&>(m_pads[i]);
    }
}

bool LevelTransporters::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return saved.m_owner == this && binding && binding == m_checkpointBinding &&
           saved.m_pads.size() == m_pads.size();
}

bool LevelTransporters::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < m_pads.size(); ++i) {
        static_cast<PadState&>(m_pads[i]) = saved.m_pads[i];
    }
    m_textures.restoreClock(saved.m_textures);
    m_presentationAdvanced = saved.m_presentationAdvanced;
    // Geometry and texture selections are derived again at each draw; floor contacts
    // retain their saved native phase until the next ordinary syncFloors call.
    ++m_checkpointGeneration;
    return true;
}

std::vector<LevelTransporters::Contact>
LevelTransporters::contacts(const WorldLayout& layout, s32 players,
                            const WorldCollision* collision) {
    std::vector<Contact> result;
    const auto authored = itemSupportWorld(layout, collision);
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
        Contact pad;
        pad.instance = static_cast<s32>(i);
        pad.id = parameter(instance, 0);
        pad.destinationId = parameter(instance, 4);
        pad.position = instance.position;
        pad.radius = info.radius;
        pad.height = info.height;
        // The skirt clears the floor, but contact and landing keep their native anchor.
        pad.transform =
            itemPlacement(pad.position + Vec3{0, ItemFigure::kFloorLift, 0}, instance.rotation);
        const auto* supportWorld =
            itemSupportAt(instance.position, collision, authored ? &*authored : nullptr);
        if (supportWorld != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
            if (const auto floor = supportWorld->restingFloorAt(instance.position, 4, 10, 1)) {
                if (const auto transform = supportWorld->objectTransform(floor->object)) {
                    const Mat4 inverse = glm::inverse(*transform);
                    pad.support = floor->object;
                    pad.supportLocal = inverse * pad.transform;
                    pad.anchorLocal = Vec3{inverse * Vec4{pad.position, 1.0f}};
                }
            }
        }
        result.push_back(pad);
    }
    for (usize i = 0; i < result.size(); ++i) {
        for (usize j = 0; j < result.size(); ++j) {
            if (i != j && result[j].id == result[i].destinationId) {
                result[i].destination = j;
                break;
            }
        }
    }
    return result;
}

void LevelTransporters::bind(RenderDevice& device, const WorldLayout& layout,
                             const AssetScope& scope, s32 players,
                             const WorldCollision* collision) {
    clear();
    m_layout = &layout;
    m_collision = collision;
    m_players = players;
    ItemArchive* art = scope.archiveForTree("TRANS");
    const auto tree = art != nullptr ? art->trees.find("TRANS") : std::nullopt;
    if (tree) {
        m_archive = art;
        // The pointer is bound whenever the index it guards is set:
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        m_tree = &art->trees.tree(*tree);
        m_textures.bind(art->trees.textureAnimations(), art->textures, device);
    }
    for (const auto& contact : contacts(layout, players, collision)) {
        Pad pad;
        static_cast<Contact&>(pad) = contact;
        if (m_tree != nullptr && art != nullptr &&
            pad.model.bind(*m_tree, art->models, art->textures, device)) {
            pad.pose.rest(*m_tree);
            if (const auto sequence = m_tree->findSequence("ACTIVE")) {
                pad.animation.start(m_tree->sequences[*sequence], *sequence);
            }
        }
        m_pads.push_back(std::move(pad));
    }
    for (const auto& pad : m_pads) {
        if (!pad.destination) {
            log::warn("Transporter {}: missing destination {}", pad.id, pad.destinationId);
        }
    }
    animate(0.0f);
}

void LevelTransporters::syncFloors(const WorldCollision& collision) {
    for (Pad& pad : m_pads) {
        pad.sync(collision);
    }
}

void LevelTransporters::Contact::sync(const WorldCollision& collision) {
    if (support < 0) {
        return;
    }
    // Collision may switch off while the floor moves; the pad still rides it.
    if (const auto placement = collision.objectTransform(support)) {
        transform = *placement * supportLocal;
        position = Vec3{*placement * Vec4{anchorLocal, 1.0f}};
    }
}

void LevelTransporters::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_checkpointGeneration = 0;
    m_pads.clear();
    m_tree = nullptr;
    m_textures.clear();
    m_presentationAdvanced = false;
    m_layout = nullptr;
    m_archive = nullptr;
    m_collision = nullptr;
    m_players = 1;
}

void LevelTransporters::animate(f32 seconds) {
    m_textures.advance(seconds);
    m_presentationAdvanced = seconds > 0;
    for (auto& pad : m_pads) {
        if (m_tree != nullptr && pad.animation.playing()) {
            pad.previousFrame = pad.animation.presentationFrame();
            pad.previousGeneration = pad.animation.generation();
            pad.animation.advance(seconds, true);
            const auto sequence = pad.animation.sequence();
            const auto frame = static_cast<s32>(pad.animation.frame());
            pad.pose.evaluate(*m_tree, sequence, pad.animation.frame());
            pad.model.setFrame(sequence, frame);
            m_textures.apply(pad.model, *m_tree, sequence, frame);
        }
    }
}

void LevelTransporters::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                             f32 renderAlpha) const {
    for (const auto& pad : m_pads) {
        if (pad.model.bound() && m_tree != nullptr) {
            const f32 blend = renderAlpha < 0 || m_presentationAdvanced ? renderAlpha : 1.0f;
            f32 frame = pad.animation.frame();
            TreePose visualPose;
            const TreePose* pose = &pad.pose;
            if (blend >= 0 && pad.animation.playing()) {
                frame = pad.previousGeneration == pad.animation.generation()
                            ? portable::lerp(pad.previousFrame, pad.animation.presentationFrame(),
                                             std::clamp(blend, 0.0f, 1.0f))
                            : pad.animation.presentationFrame();
                visualPose.evaluate(*m_tree, pad.animation.sequence(), frame, false, true);
                pose = &visualPose;
            }
            pad.model.setPresentationFrame(pad.animation.sequence(), frame);
            m_textures.apply(pad.model, *m_tree, pad.animation.sequence(), frame,
                             m_textures.presentationOffset(blend));
            pad.model.draw(device, clip, pad.transform, lighting, pose->matrices());
        }
    }
}

std::optional<LevelTransporters::Presentation> LevelTransporters::presentation(usize index) const {
    if (index >= m_pads.size() || m_tree == nullptr || !m_pads[index].model.bound()) {
        return std::nullopt;
    }
    const auto& pad = m_pads[index];
    Presentation result;
    result.archive = m_archive;
    result.tree = m_tree;
    result.instance = static_cast<u32>(pad.instance) + 1;
    result.placement = pad.transform;
    result.playing = pad.animation.playing();
    if (result.playing) {
        result.sequence = pad.animation.sequence();
        result.generation = pad.animation.generation();
        result.frame = pad.animation.presentationFrame();
        result.continuous = pad.previousGeneration == result.generation;
        result.previousFrame = result.continuous ? pad.previousFrame : result.frame;
    }
    result.advanced = m_presentationAdvanced;
    result.textureClock =
        static_cast<f32>(m_textures.frame()) + m_textures.presentationOffset(1).value_or(0);
    result.previousTextureClock = result.advanced ? static_cast<f32>(m_textures.frame()) +
                                                        m_textures.presentationOffset(0).value_or(0)
                                                  : result.textureClock;
    return result;
}

std::optional<usize> LevelTransporters::contact(const Vec3& feet, f32 radius, f32 height) const {
    for (usize i = 0; i < m_pads.size(); ++i) {
        const auto& pad = m_pads[i];
        if (pad.touches(feet, radius, height)) {
            return i;
        }
    }
    return std::nullopt;
}

bool LevelTransporters::Contact::touches(const Vec3& feet, f32 bodyRadius, f32 bodyHeight) const {
    const Vec3 away = feet - position;
    const f32 reach = bodyRadius + radius;
    return away.x * away.x + away.z * away.z < reach * reach && feet.y <= position.y + height &&
           feet.y + bodyHeight >= position.y;
}

bool LevelTransporters::visible(const Vec3& position, f32 radius, const WorldCamera& camera,
                                const CameraView& view) {
    const Vec3 relative = position - camera.position;
    const f32 z = glm::dot(relative, camera.forward());
    const f32 horizontal = view.horizontalFov * 0.5f;
    const f32 vertical = portable::atan(portable::tan(horizontal) / view.aspect);
    // The entire destination sphere must fit, not just its centre.
    return z >= WorldCamera::kNear + radius && z <= WorldCamera::kFar - radius &&
           std::abs(glm::dot(relative, camera.right())) * portable::cos(horizontal) <=
               z * portable::sin(horizontal) - radius &&
           std::abs(glm::dot(relative, camera.up())) * portable::cos(vertical) <=
               z * portable::sin(vertical) - radius;
}

std::optional<Vec3> LevelTransporters::landing(usize source, f32 radius,
                                               const WorldCollision& collision,
                                               const WorldCamera& camera,
                                               const CameraView& view) const {
    if (source >= m_pads.size()) {
        return std::nullopt;
    }
    const auto destinationIndex = m_pads[source].destination;
    if (!destinationIndex.has_value()) {
        return std::nullopt;
    }
    Vec3 destination = m_pads[*destinationIndex].position;
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
