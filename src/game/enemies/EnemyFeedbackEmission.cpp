#include "game/enemies/EnemyFeedbackEmission.h"

#include <algorithm>
#include <array>
#include <format>

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"

namespace gdl::game {

GeneratorFeedbackEmission GeneratorFeedbackEmission::resolve(const GeneratorEvent& event,
                                                             ItemArchive* archive,
                                                             const LevelInfo* level, s32 realm,
                                                             std::string_view levelName) {
    GeneratorFeedbackEmission result;
    result.position = event.position;
    result.placement = event.placement;
    if (event.stateChanged && archive != nullptr) {
        const std::string_view tree = event.destroyed ? "GENDIE" : "GENHIT";
        if (archive->trees.find(tree)) {
            result.archive = archive;
            result.tree = tree;
        }
    }
    if (!event.destroyed || level == nullptr || level->bossType < 0) {
        const std::string suffix =
            realm == 10 && event.kind == enemy_type::kWarlock
                ? "WAR"
                : std::string(1, levelName.empty() ? 'G' : levelName.front());
        result.sound = std::format("{}{}", event.destroyed ? "S_GENKILL" : "S_GENDAM", suffix);
    }
    return result;
}

EnemyFeedbackEmission EnemyFeedbackEmission::resolve(const EnemyFeedback& feedback,
                                                     ItemArchive* enemyArchive,
                                                     ItemArchive& weapons, const LevelInfo* level) {
    EnemyFeedbackEmission result;
    result.position = feedback.position;
    if (level != nullptr) {
        result.sound = feedback.sound(level->enemies, level->bossType);
    }
    const auto tree = feedback.effect();
    if (tree.empty()) {
        return result;
    }
    auto* archive = enemyArchive;
    if (archive == nullptr || !archive->trees.find(tree)) {
        archive = &weapons;
    }
    if (!archive->trees.find(tree)) {
        return result;
    }
    result.archive = archive;
    result.tree = tree;
    result.setting.scale = feedback.effectScale();
    result.setting.yaw = feedback.yaw;
    // Native common impacts set node alpha to 96. The two tree-creature
    // effects retain the archive's own opacity instead.
    if (tree != "TREEHIT" && tree != "TREEDIE") {
        result.setting.tint.a = 96;
    }
    return result;
}

namespace {
std::optional<EffectTrees::Prepared>
prepareEffect(ItemArchive* archive, std::string_view tree,
              std::span<const EffectTrees::PoolResource> resources,
              std::span<TextureSet* const> lenders) {
    if (archive == nullptr || tree.empty()) {
        return std::nullopt;
    }
    const auto index = archive->trees.find(tree);
    if (!index) {
        return std::nullopt;
    }
    const auto* nativeTree = &archive->trees.tree(*index);
    for (const auto& resource : resources) {
        const auto* prototype = resource.prototype;
        if (resource.id == 0 || resource.archive != archive || resource.textures == nullptr ||
            prototype == nullptr || !std::ranges::equal(resource.lenders, lenders) ||
            !std::ranges::equal(prototype->lenders, lenders) || prototype->archive != archive ||
            prototype->tree != nativeTree || !prototype->emitParticles ||
            prototype->missingObjectsAreEmpty || prototype->trails.size() != 0) {
            continue;
        }
        EffectTrees::Prepared prepared{
            archive,           nativeTree,      prototype->model.get(), &prototype->particles,
            resource.textures, resource.lenders};
        prepared.device = prototype->device;
        return prepared;
    }
    return std::nullopt;
}
} // namespace

std::optional<EffectTrees::Prepared>
EnemyFeedbackEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                               std::span<TextureSet* const> lenders) const {
    return prepareEffect(archive, tree, resources, lenders);
}

std::optional<EffectTrees::Prepared>
GeneratorFeedbackEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                                   std::span<TextureSet* const> lenders,
                                   const RenderDevice& device) const {
    auto prepared = prepareEffect(archive, tree, resources, lenders);
    if (!prepared || prepared->device != &device) {
        return std::nullopt;
    }
    return prepared;
}

EnemyBirthEmission EnemyBirthEmission::resolve(const EnemyBirth& birth,
                                               std::span<ItemArchive* const> archives) {
    EnemyBirthEmission result;
    result.position = birth.position;
    constexpr std::array<std::string_view, 3> kTrees{"GENFX1", "GENFX2", "GENFX3"};
    for (usize kind = 0; kind < std::min<usize>(archives.size(), kSwarmKindCount); ++kind) {
        ItemArchive* archive = archives[kind];
        if (archive == nullptr || !archive->trees.find(kTrees.front())) {
            continue;
        }
        // InitEffects stops at the FIRST provider, even when the birth's kind
        // lies after it. A later archive cannot extend the eligible prefix.
        if (birth.kind >= 0 && static_cast<usize>(birth.kind) <= kind &&
            0.5f * enemyKind(birth.kind).height > 2) {
            const auto own = kTrees[static_cast<usize>(std::clamp(birth.tier, 1, 3) - 1)];
            result.archive = archive;
            result.tree = archive->trees.find(own) ? own : kTrees.front();
        }
        break;
    }
    return result;
}

std::optional<EffectTrees::Prepared>
EnemyBirthEmission::prepare(std::span<const EffectTrees::PoolResource> resources,
                            std::span<TextureSet* const> lenders) const {
    return prepareEffect(archive, tree, resources, lenders);
}

} // namespace gdl::game
