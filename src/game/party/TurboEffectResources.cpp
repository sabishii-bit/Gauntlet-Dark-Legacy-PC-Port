#include "game/party/TurboEffectResources.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "game/party/PlayerFigure.h"

namespace gdl::game {
namespace {
ItemArchive* treeArchive(ItemArchive& effects, ItemArchive& weapons, std::string_view name) {
    if (effects.trees.find(name)) {
        return &effects;
    }
    return weapons.trees.find(name) ? &weapons : nullptr;
}

bool wantsMesh(const TreeInfo& tree) {
    return std::ranges::any_of(tree.nodes, [](const TreeNodeInfo& node) {
        return !node.object.empty() || std::ranges::any_of(node.objectFrames, [](const auto& run) {
            return !run.object.empty();
        });
    });
}
} // namespace

void TurboEffectResources::clear() {
    m_resources.clear();
    m_strikes.clear();
    m_status.clear();
    m_stats = nullptr;
    m_character = -1;
}

bool TurboEffectResources::bind(RenderDevice& device, s32 character, const ClassStats& stats,
                                PlayerFigure& figure, ItemArchive& weapons,
                                std::span<TextureSet* const> lenders) {
    auto* effects = figure.effects();
    return effects != nullptr && bind(device, character, stats, *effects, weapons, lenders);
}

bool TurboEffectResources::bind(RenderDevice& device, s32 character, const ClassStats& stats,
                                ItemArchive& effects, ItemArchive& weapons,
                                std::span<TextureSet* const> lenders) {
    if (character < 0 || character >= kClassCount || !effects.loaded() || !weapons.loaded() ||
        stats.moveStrikes.size() > kMaxRows || stats.moveEffects.size() > kMaxRows) {
        return false;
    }
    TurboEffectResources next;
    next.m_character = character;
    next.m_stats = &stats;
    for (const s32 first : {stats.moves.turboB, stats.moves.turboC1, stats.moves.turboC2}) {
        std::set<s32> followed;
        for (s32 row = first; row >= 0;) {
            if (static_cast<usize>(row) >= stats.moveStrikes.size() ||
                !followed.insert(row).second) {
                return false;
            }
            if (!next.m_status.contains(row)) {
                next.m_status.emplace(row,
                                      next.prepareStrike(device, row, effects, weapons, lenders));
            }
            row = stats.moveStrikes[static_cast<usize>(row)].next;
        }
    }
    *this = std::move(next);
    return true;
}

TurboEffectResources::Status TurboEffectResources::status(s32 row) const {
    const auto found = m_status.find(row);
    return found == m_status.end() ? Status::NotTurbo : found->second;
}

const TurboEffectResources::Strike* TurboEffectResources::strike(s32 row) const {
    const auto found = m_strikes.find(row);
    return found == m_strikes.end() ? nullptr : &found->second;
}

const TurboEffectResources::Resource* TurboEffectResources::resource(u32 id) const {
    const auto found = m_resources.find(id);
    return found == m_resources.end() ? nullptr : &found->second;
}

std::optional<TurboEmission::Strike>
TurboEffectResources::prepare(s32 row, const TurboEmission::Context& context,
                              f32 meleeDamage) const {
    const auto* cached = strike(row);
    const auto finite = [](const Vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
               std::abs(value.x) <= 1e6f && std::abs(value.y) <= 1e6f && std::abs(value.z) <= 1e6f;
    };
    if (cached == nullptr || m_stats == nullptr || context.character != m_character ||
        context.detached || context.partnerAvailable || !std::isfinite(meleeDamage) ||
        meleeDamage < 0 || meleeDamage > 1e6f || !finite(context.position) ||
        !finite(context.facing)) {
        return std::nullopt;
    }
    return TurboEmission::prepare(*m_stats, row, context, meleeDamage, cached->effectSeconds);
}

std::optional<TurboEffectResources::Resource>
TurboEffectResources::bindResource(RenderDevice& device, s32 row, const MoveEffect& effect,
                                   ItemArchive& effects, ItemArchive& weapons,
                                   std::span<TextureSet* const> lenders) {
    Resource result;
    result.row = row;
    result.id = static_cast<u32>(row) + 1;
    if (effect.particle()) {
        // PsfxDoParticle does not use the tree's external texture lenders. It chooses
        // an actual image in this costume's SFX first, then the shared weapon bank.
        result.kind = Kind::Particles;
        for (auto* archive : {&effects, &weapons}) {
            const auto slot = archive->textures.find(effect.tree);
            if (slot && !archive->textures.entry(*slot).external() &&
                !archive->textures.entry(*slot).noPicture) {
                result.archive = archive;
                result.textureSlot = slot;
                result.texture = &archive->textures.texture(device, *slot);
                result.descriptor = TurboEmission::particle(effect).descriptor;
                ParticleEmitter prototype;
                prototype.start(*result.descriptor, Mat4{1});
                if (!prototype.checkpoint().valid()) {
                    return std::nullopt;
                }
                result.textures.bind(archive->trees.textureAnimations(), archive->textures, device);
                return result;
            }
        }
        return std::nullopt;
    }
    result.archive = treeArchive(effects, weapons, effect.tree);
    if (effect.tree.empty() || effect.tree == "NULLFX") {
        if (result.archive != nullptr) {
            result.tree = &result.archive->trees.tree(*result.archive->trees.find(effect.tree));
        }
        return result; // Invisible NULLFX still exposes its native lifetime tree.
    }
    if (result.archive == nullptr) {
        return std::nullopt;
    }
    result.kind = Kind::Tree;
    auto& archive = *result.archive;
    result.tree = &archive.trees.tree(*archive.trees.find(effect.tree));
    result.lenders.assign(lenders.begin(), lenders.end());
    TreePose pose;
    if (result.tree->sequences.empty()) {
        pose.rest(*result.tree);
    } else {
        pose.evaluate(*result.tree, 0, 0);
    }
    if (wantsMesh(*result.tree) &&
        !result.model.bind(*result.tree, archive.models, archive.textures, device, lenders)) {
        return std::nullopt;
    }
    result.particles.bind(*result.tree, archive, device, Mat4{1}, pose.matrices(), lenders);
    result.particles.setLocalScales(pose.poses());
    if ((!result.model.bound() && result.particles.field().size() == 0) ||
        !result.particles.checkpoint().valid()) {
        return std::nullopt;
    }
    for (usize i = 0; i < result.particles.field().size(); ++i) {
        if (result.particles.field().textureOf(i) == &device.whiteTexture()) {
            return std::nullopt;
        }
    }
    result.model.setFrame(0, 0);
    result.textures.bind(archive.trees.textureAnimations(), archive.textures, device, lenders);
    result.textures.apply(result.model, *result.tree, 0, 0);
    result.textures.apply(result.particles, *result.tree, 0, 0);
    return result;
}

TurboEffectResources::Status
TurboEffectResources::prepareStrike(RenderDevice& device, s32 row, ItemArchive& effects,
                                    ItemArchive& weapons, std::span<TextureSet* const> lenders) {
    if (m_stats == nullptr) {
        return Status::InvalidChain;
    }
    const auto& definition = m_stats->moveStrikes[static_cast<usize>(row)];
    if (definition.type == MoveStrike::kFlies || definition.speed != 0 ||
        definition.hitEffect >= 0 || definition.loopEffect >= 0) {
        return Status::UnsupportedStrike;
    }
    Strike prepared;
    prepared.row = row;
    std::map<u32, Resource> resources;
    std::set<s32> followed;
    for (s32 effectRow = definition.effect; effectRow >= 0;) {
        if (static_cast<usize>(effectRow) >= m_stats->moveEffects.size() ||
            !followed.insert(effectRow).second) {
            return Status::InvalidChain;
        }
        const auto& effect = m_stats->moveEffects[static_cast<usize>(effectRow)];
        constexpr u32 kUnsupportedParents = 0x10U | 0x40U | 0x80U | 0x800U | 0x2000U | 0x40000U;
        if ((effect.flags & kUnsupportedParents) != 0) {
            return Status::UnsupportedParent;
        }
        const u32 id = static_cast<u32>(effectRow) + 1;
        const Resource* bound = resource(id);
        if (bound == nullptr) {
            auto candidate = bindResource(device, effectRow, effect, effects, weapons, lenders);
            if (!candidate) {
                return Status::MissingResource;
            }
            bound = &resources.emplace(id, std::move(*candidate)).first->second;
        }
        if (effectRow == definition.effect) {
            // A particle's texture name is not a tree. Follow the native primary tree
            // lookup separately rather than inferring timing from its descriptor.
            auto* archive = treeArchive(effects, weapons, effect.tree);
            const TreeInfo* tree = archive == nullptr
                                       ? nullptr
                                       : &archive->trees.tree(*archive->trees.find(effect.tree));
            prepared.effectSeconds = TurboEmission::effectSeconds(effect, tree);
        }
        prepared.resources.push_back(bound->id);
        effectRow = effect.next;
    }
    m_resources.merge(resources);
    m_strikes.emplace(row, std::move(prepared));
    return Status::Ready;
}
} // namespace gdl::game
