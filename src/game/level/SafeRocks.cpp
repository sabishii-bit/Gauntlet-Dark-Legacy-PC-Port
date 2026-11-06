#include "game/level/SafeRocks.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/assets/AssetScope.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"

#include "game/level/CombatCover.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {

namespace {

s32 parameter(const ItemInstance& instance, usize offset) {
    // The unpacked manifest keeps the original little-endian parameter bytes.
    const u32 value = static_cast<u32>(instance.params[offset]) |
                      (static_cast<u32>(instance.params[offset + 1]) << 8);
    return value < 0x8000U ? static_cast<s32>(value) : static_cast<s32>(value) - 0x10000;
}

} // namespace

bool SafeRocks::appendBindingDigest(NativeStateDigest& digest) const {
    digest.string("SafeRocks.native.1");
    digest.resource(m_layout);
    const auto archive = [&](const ItemArchive* items) {
        // Missing optional art is a valid native cover binding. An unloaded
        // archive has no resource identity or artwork to borrow.
        digest.flag(items != nullptr);
        if (items) {
            digest.flag(items->loaded());
            if (items->loaded()) {
                digest.resource(items);
            }
        }
    };
    archive(m_items);
    archive(m_animationItems);
    digest.value(m_textureSlots);
    m_textures.appendBindingDigest(digest);
    digest.wide(m_rocks.size());
    for (const auto& rock : m_rocks) {
        if (!rock) {
            digest.invalidate();
            return false;
        }
        // Model bindings remain in this owner, not in local history. Their
        // texture/frame draw cache is reconstructed before each draw.
        for (const auto& model : rock->models) {
            digest.flag(model.bound());
            if (model.bound()) {
                digest.resource(&model);
            }
        }
    }
    return digest.good();
}

bool SafeRocks::appendNativeDigest(NativeStateDigest& digest) const {
    if (!appendBindingDigest(digest)) {
        return false;
    }
    digest.fields<3>(m_textures.clock());
    digest.wide(m_rocks.size());
    for (const auto& rock : m_rocks) {
        digest.value(static_cast<const RockState&>(*rock));
    }
    return digest.good();
}

bool SafeRocks::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    if (!appendBindingDigest(digest)) {
        return false;
    }
    digest.fields<3>(saved.m_textures);
    digest.value(saved.m_rocks);
    return digest.good();
}

bool SafeRocks::registerNativeResources(NativeStateResources& resources) const {
    if (m_items && m_items->loaded()) {
        if (!resources.refreshArchive(*m_items)) {
            return false;
        }
        for (usize index = 0; index < m_rocks.size(); ++index) {
            for (usize tier = 0; tier < m_rocks[index]->models.size(); ++tier) {
                const auto& model = m_rocks[index]->models[tier];
                if (model.bound() &&
                    !resources.registerModel(model, *m_items,
                                             std::format("safe-rock/{}/{}", index, tier))) {
                    return false;
                }
            }
        }
    }
    return ((m_animationItems == nullptr) || !m_animationItems->loaded() ||
            resources.refreshArchive(*m_animationItems)) &&
           resources.good();
}

void SafeRocks::saveLocal(LocalSnapshot& saved) const {
    saved.m_rocks.resize(m_rocks.size());
    for (usize index = 0; index < m_rocks.size(); ++index) {
        saved.m_rocks[index] = static_cast<const RockState&>(*m_rocks[index]);
    }
    saved.m_textures = m_textures.clock();
    m_localBinding.stamp(saved.m_stamp);
}

bool SafeRocks::canRestoreLocal(const LocalSnapshot& saved) const {
    return m_localBinding.admits(saved.m_stamp) && saved.m_rocks.size() == m_rocks.size();
}

bool SafeRocks::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_rocks == right.m_rocks &&
           left.m_textures == right.m_textures;
}

bool SafeRocks::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize index = 0; index < m_rocks.size(); ++index) {
        static_cast<RockState&>(*m_rocks[index]) = saved.m_rocks[index];
    }
    m_textures.restoreClock(saved.m_textures);
    return true;
}

bool SafeRocks::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items) {
    clear();
    m_layout = &layout;
    m_items = &items;
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        const s32 override = parameter(instance, 0);
        if (info.type != kItemType || (override > 0 ? override : info.subtype) != kSubtype) {
            continue;
        }
        auto rock = std::make_unique<Rock>();
        rock->instance = static_cast<s32>(i);
        rock->baseHealth = std::max(info.hitPoints, 0);
        rock->tier = std::clamp(parameter(instance, 2), 0, kWhole);
        rock->health = rock->tier * rock->baseHealth;
        rock->armor = info.armor;
        rock->minPlayers = instance.minPlayers;
        rock->position = instance.position;
        rock->placement = itemPlacement(instance.position, instance.rotation);
        rock->obstacle.centre = Vec3{rock->placement * Vec4{info.collisionOffset, 1.0f}};
        rock->obstacle.height = info.height;
        rock->obstacle.yaw = portable::atan2(rock->placement[2].x, rock->placement[2].z);
        rock->obstacle.halfAcross = info.xSize > 0.0f ? info.xSize : info.radius;
        rock->obstacle.halfAlong = info.zSize > 0.0f ? info.zSize : info.radius;
        if (info.collisionType == 1) {
            rock->obstacle.cylinderRadius = info.radius;
        }
        for (s32 tier = 0; tier <= kWhole; ++tier) {
            const std::string base = std::format("{}{}", info.name, tier);
            bool found = false;
            if (const auto object = AssetScope(items).findObject(base)) {
                TreeInfo tree;
                tree.name = base;
                TreeNodeInfo node;
                node.name = base;
                node.object = object->name;
                node.objectFlags = info.objectFlags;
                tree.nodes.push_back(node);
                found = rock->models[static_cast<usize>(tier)].bind(tree, items.models,
                                                                    items.textures, device);
                if (found) {
                    for (const MeshPart& part : items.models.mesh(object->index).parts) {
                        if (std::ranges::find(m_textureSlots, part.texture) ==
                            m_textureSlots.end()) {
                            m_textureSlots.push_back(part.texture);
                        }
                    }
                }
            }
            if (!found) {
                log::warn("Safe rocks: no model for {}", base);
            }
        }
        m_rocks.push_back(std::move(rock));
    }
    return !m_rocks.empty();
}

void SafeRocks::bindAnimations(RenderDevice& device, ItemArchive& items,
                               std::span<TextureSet* const> lenders) {
    m_localBinding.rebind();
    m_animationItems = &items;
    std::vector<TextureAnimationInfo> animations;
    for (const auto& animation : items.trees.textureAnimations()) {
        // An item archive also contains effects keyed to other figures' sequences.
        // These static meshes consume only clocks for slots present in their health tiers.
        if (animation.freeRunning() && animation.texture >= 0 &&
            std::ranges::find(m_textureSlots, static_cast<u32>(animation.texture)) !=
                m_textureSlots.end()) {
            animations.push_back(animation);
        }
    }
    m_textures.bind(animations, items.textures, device, lenders);
}

void SafeRocks::clear() {
    m_localBinding.rebind();
    m_layout = nullptr;
    m_items = nullptr;
    m_animationItems = nullptr;
    m_rocks.clear();
    m_textureSlots.clear();
    m_textures.clear();
}

std::vector<SafeRocks::BodyResource> SafeRocks::bodyResources() const {
    std::vector<BodyResource> result;
    for (usize index = 0; index < m_rocks.size(); ++index) {
        for (s32 tier = 0; tier <= kWhole; ++tier) {
            const auto& model = m_rocks[index]->models[static_cast<usize>(tier)];
            if (model.bound()) {
                result.push_back({index, tier, &model, &m_textures});
            }
        }
    }
    return result;
}

void SafeRocks::setPlayerCount(s32 players) {
    for (const auto& rock : m_rocks) {
        rock->shown = shownToParty(rock->minPlayers, players);
    }
}

bool SafeRocks::standing(usize index) const {
    return index < m_rocks.size() && m_rocks[index]->shown && !m_rocks[index]->dormant &&
           m_rocks[index]->health > 0 && m_rocks[index]->tier > 0;
}

bool SafeRocks::strike(usize index, f32 power) {
    if (!standing(index) || m_rocks[index]->armor < 0 || !std::isfinite(power) || power <= 0.0f) {
        return false;
    }
    Rock& rock = *m_rocks[index];
    const f32 damage =
        std::clamp(power - static_cast<f32>(rock.armor), 1.0f, static_cast<f32>(rock.health));
    rock.health -= static_cast<s32>(std::lround(damage));
    if (rock.health == 0) {
        rock.tier = 0;
    } else if (rock.health <= rock.baseHealth) {
        rock.tier = 1;
    } else if (rock.health <= 2 * rock.baseHealth) {
        rock.tier = 2;
    } else {
        rock.tier = kWhole;
    }
    return rock.health == 0;
}

void SafeRocks::activate(usize index) {
    if (index < m_rocks.size()) {
        Rock& rock = *m_rocks[index];
        rock.health = kWhole * rock.baseHealth;
        rock.tier = kWhole;
        rock.dormant = false;
        rock.activationDelay = 0;
    }
}

void SafeRocks::hideForEruptions() {
    constexpr usize kMaxAnchors = 16;
    for (usize i = 0; i < std::min(size(), kMaxAnchors); ++i) {
        m_rocks[i]->dormant = true;
        m_rocks[i]->activationDelay = 0;
    }
}

void SafeRocks::scheduleActivation(usize index, f32 delay) {
    if (index < size() && std::isfinite(delay) && delay > 0) {
        m_rocks[index]->activationDelay = delay;
    }
}

void SafeRocks::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_textures.advance(seconds);
    for (usize i = 0; i < size(); ++i) {
        auto& rock = *m_rocks[i];
        if (rock.activationDelay > 0) {
            rock.activationDelay -= seconds;
            if (rock.activationDelay <= 0) {
                activate(i);
            }
        }
    }
}

std::vector<CombatArenaTarget> SafeRocks::eruptionTargets() const {
    auto targets = arenaTargets();
    std::erase_if(targets, [](const CombatArenaTarget& target) { return target.active; });
    return targets;
}

std::vector<CombatArenaTarget> SafeRocks::arenaTargets() const {
    constexpr usize kMaxAnchors = 16;
    std::vector<CombatArenaTarget> targets;
    for (usize i = 0; i < std::min(size(), kMaxAnchors); ++i) {
        if (m_rocks[i]->shown) {
            targets.push_back({i, m_rocks[i]->placement, standing(i)});
        }
    }
    return targets;
}

std::vector<Obstacle> SafeRocks::obstacles() const {
    std::vector<Obstacle> out;
    for (usize i = 0; i < size(); ++i) {
        if (standing(i)) {
            out.push_back(m_rocks[i]->obstacle);
        }
    }
    return out;
}

std::vector<Mat4> SafeRocks::attackAnchors() const {
    constexpr usize kMaxAnchors = 16;
    std::vector<Mat4> anchors;
    for (const auto& rock : m_rocks) {
        if (rock->shown) {
            anchors.push_back(rock->placement);
            if (anchors.size() == kMaxAnchors) {
                break;
            }
        }
    }
    return anchors;
}

void SafeRocks::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     f32 presentationAlpha) const {
    for (const auto& rock : m_rocks) {
        if (rock->shown && !rock->dormant) {
            TreeModel& model = rock->models[static_cast<usize>(rock->tier)];
            m_textures.apply(model, m_textures.presentationOffset(presentationAlpha));
            model.draw(device, clip, rock->placement, lighting);
        }
    }
}

bool SafeRocks::blocksBreath(const Vec3& from, const Vec3& to) const {
    return blocksSegment(from, to, CombatCover::kBreathProbeRadius);
}

bool SafeRocks::blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const {
    for (usize i = 0; i < m_rocks.size(); ++i) {
        if (standing(i) && m_rocks[i]->obstacle.blocksSegment(from, to, radius)) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
