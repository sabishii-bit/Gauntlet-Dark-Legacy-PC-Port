#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "game/effects/EffectTrees.h"
#include "game/enemies/EnemyFeedback.h"

namespace gdl::game {
struct EnemyBirth;
struct GeneratorEvent;

/** Native generator feedback follows the damaging state transition, but precedes
 * rewards and brood detachment. Missing native artwork is intentional; a present
 * tree must use its exact prebound archive and lender context during replay. */
struct GeneratorFeedbackEmission {
    std::string sound;
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};
    Mat4 placement{1};

    static GeneratorFeedbackEmission resolve(const GeneratorEvent& event, ItemArchive* archive,
                                             const LevelInfo* level, s32 realm,
                                             std::string_view levelName);
    std::optional<EffectTrees::Prepared>
    prepare(std::span<const EffectTrees::PoolResource> resources,
            std::span<TextureSet* const> lenders, const RenderDevice& device) const;
};

/** Native GENFX birth artwork. The roster is indexed by swarm kind; absent
 * loaded archives remain null. No sound, damage or resource load is implied. */
struct EnemyBirthEmission {
    ItemArchive* archive = nullptr;
    std::string_view tree;
    Vec3 position{0};

    static EnemyBirthEmission resolve(const EnemyBirth& birth,
                                      std::span<ItemArchive* const> archives);
    std::optional<EffectTrees::Prepared>
    prepare(std::span<const EffectTrees::PoolResource> resources,
            std::span<TextureSet* const> lenders) const;
};

/** Native swarm impact presentation, resolved before its actor/slot can be
 * recycled. Resolution never starts an effect, plays sound, or awards damage.
 * Archives remain borrowed from the loaded stage, not selected by packet text. */
struct EnemyFeedbackEmission {
    std::string sound;
    ItemArchive* archive = nullptr; ///< null when native artwork is unavailable
    std::string_view tree;
    Vec3 position{0};
    EffectTrees::Setting setting;

    static EnemyFeedbackEmission resolve(const EnemyFeedback& feedback, ItemArchive* enemyArchive,
                                         ItemArchive& weapons, const LevelInfo* level);

    /** Finds the same tree and ordered native lender context in a trusted
     * preloaded catalog. Missing bindings refuse replay; they never substitute
     * another archive, suppress an available effect, or load artwork mid-tick.
     * An emission with archive == nullptr needs no visual preparation. */
    std::optional<EffectTrees::Prepared>
    prepare(std::span<const EffectTrees::PoolResource> resources,
            std::span<TextureSet* const> lenders) const;
};

} // namespace gdl::game
