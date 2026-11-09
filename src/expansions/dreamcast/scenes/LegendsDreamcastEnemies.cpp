#include "expansions/dreamcast/enemies/LegendsDreamcastEnemies.h"

#include <algorithm>

#include "engine/assets/ObjectFlags.h"
#include "engine/core/Log.h"

#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/enemies/Generators.h"

namespace gdl::game {

void LegendsSceneAdapter::bindDreamcastGenerator(
    RenderDevice& device, const formats::ItemInfoRecord& info,
    const formats::ItemInstanceRecord& instance, u32 nativeIndex,
    const formats::LegendsDreamcastFixtureScope& nativeScope,
    const std::filesystem::path& nativeRoot) {
    const auto warn = [&](const std::string& text) {
        if (!text.empty() && std::ranges::find(m_warnings, text) == m_warnings.end())
            m_warnings.push_back(text);
    };
    std::string diagnostic;
    auto definition =
        legendsDreamcastGenerator(info, instance, nativeIndex, nativeScope, &diagnostic);
    warn(diagnostic);
    if (!definition)
        return;
    const usize kind = static_cast<usize>(definition->gameplay.kind);
    const bool mixed = definition->gameplay.broodTier != 0;
    if (!mixed && !m_dreamcastGeneratorArchivesLoaded[kind]) {
        m_dreamcastGeneratorArchivesLoaded[kind] = true;
        m_dreamcastGeneratorArchives[kind] = loadLegendsDreamcastGeneratorAssets(
            nativeRoot, definition->gameplay.kind, nativeScope, m_warnings);
    }
    // SPECIAL models belong to the selected Temple/Hell item bank, not any
    // of their four single-tier monster banks. Ordinary species keep their
    // owner-scoped lookup so generic low-detail models never shadow them.
    ItemArchive* speciesArchive = mixed ? nullptr : m_dreamcastGeneratorArchives[kind].get();
    AssetScope speciesScope;
    if (speciesArchive)
        speciesScope.add(*speciesArchive);
    AssetScope fallback;
    for (auto& archive : m_archives)
        fallback.add(*archive);
    std::array<ItemArchive*, 4> owners{};
    // LoadWorld admits the reduced GENERATORS bank only for gen_active<2;
    // do_items8c0abc48..8c0abca0 spawns enemies only for that same option>2.
    // Ordinary play therefore owns its generator art in the monster bank.
    // Search every suffix inside that owner before consulting fallback banks:
    // a generic GEN_TRO2L1 must not beat the native GEN_TRO2L1ROOT.
    for (usize state = 0; state < definition->models.size(); ++state) {
        auto& name = definition->models[state];
        if (name.empty())
            continue;
        // InitItemInfo binds item trees from the item banks, not the monster
        // animation bank. Generators take FindItemObject8c0a8040's static
        // name/L1/L1ROOT result, even when the spelling ends in ROOT.
        if (const auto model = speciesScope.findObject(name)) {
            owners[state] = model->archive;
            name = model->name;
            continue;
        }
        // Some small native nests deliberately have no state-zero debris.
        if (speciesArchive && state == 0) {
            name.clear();
            continue;
        }
        if (speciesArchive)
            warn("Dreamcast generator artwork is missing from its monster library: " + name +
                 "; using fallback artwork");
        if (const auto model = fallback.findObject(name)) {
            owners[state] = model->archive;
            name = model->name;
        } else {
            if (state != 0) {
                warn("Dreamcast generator damage artwork is unavailable: " + name);
                return;
            }
            name.clear();
        }
    }
    ItemInstance placement;
    placement.position = instance.position;
    placement.rotation = instance.rotation;
    constexpr u32 kPresentationFlags = 0x0f8008c0U;
    const u32 flags = (info.objectFlags & kPresentationFlags) | object_flags::kSorted;
    const usize tier = static_cast<usize>(definition->gameplay.tier);
    const usize idleFigure = m_fixtures.size();
    place(device, definition->models[tier], placement, 1, flags, owners[tier]);
    if (idleFigure == m_fixtures.size()) {
        warn("Dreamcast generator artwork could not bind: " + definition->models[tier]);
        return;
    }
    GeneratorPresentation binding;
    binding.nativeIndex = nativeIndex;
    binding.originalFigure = idleFigure;
    binding.yaw = definition->gameplay.yaw;
    for (usize state = 0; state < definition->models.size(); ++state) {
        const auto& name = definition->models[state];
        if (name.empty())
            continue;
        if (state == static_cast<usize>(definition->gameplay.tier)) {
            binding.figures[state] = idleFigure;
            continue;
        }
        const usize slot = m_fixtures.size();
        place(device, name, placement, 1, flags, owners[state]);
        if (slot == m_fixtures.size()) {
            warn("Dreamcast generator damage artwork could not bind: " + name);
            return;
        }
        binding.figures[state] = slot;
        m_hiddenFixtures[slot] = true;
    }
    m_generators.push_back(std::move(definition->gameplay));
    m_generatorPresentation.push_back(binding);
}

std::unique_ptr<ItemArchive> LegendsLevel::loadDreamcastEnemyAssets(s32 kind) const {
    const auto root = m_directory.parent_path().parent_path();
    std::vector<std::string> warnings;
    auto archive = loadLegendsDreamcastEnemyAssets(root, kind, warnings, m_dreamcastEnemyRoster);
    for (const auto& warning : warnings) {
        log::warn("Dreamcast enemy {}: {}", kind, warning);
    }
    return archive;
}

bool LegendsLevel::playDreamcastEnemyFeedback(const EnemyFeedback& feedback) {
    // AudioSetupEnemyStreams 8c0df380 and the hurt/death dispatchers
    // 8c0dfa80/8c0df9e0 use the same stem/tier/hit-count naming contract as
    // GDL. Supply DC's selected ENMY roster, never GDL's empty donor roster.
    return playOpponentSound(feedback.sound(m_dreamcastEnemyRoster), feedback.position);
}

bool LegendsLevel::playDreamcastEnemyCue(const EnemyCue& cue) {
    // 8c0dfb80/8c0dfbc0 emit arrow/bolt; 8c0dfcc0 emits pursuit yell.
    m_opponentSounds.push_back(cue.kind == EnemyCue::Kind::Arrow  ? 0x29U
                               : cue.kind == EnemyCue::Kind::Bolt ? 0x2bU
                                                                  : 0x30U);
    return true;
}

bool LegendsLevel::playDreamcastEnemyMelee(s32 kind, s32 tier) {
    if (const auto sound = EnemyFeedback::meleeSound(kind, tier, m_dreamcastEnemyRoster)) {
        return playOpponentSound(*sound, Vec3{0});
    }
    return false; // Ordinary weapon strikes retain the player's hurt cue.
}

bool LegendsLevel::playDreamcastGeneratorFeedback(const GeneratorEvent& event) {
    if (const auto token = dreamcastGeneratorSound(m_dreamcastRealm, event.destroyed)) {
        m_opponentSounds.push_back(*token);
    }
    return true;
}

} // namespace gdl::game
