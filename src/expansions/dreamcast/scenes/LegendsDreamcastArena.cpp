#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ObjectFlags.h"

#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::bindDreamcastArenaRock(RenderDevice& device,
                                                 const formats::ItemInfoRecord& info,
                                                 const formats::ItemInstanceRecord& instance,
                                                 u32 /*nativeIndex*/) {
    auto admission = legendsDreamcastArenaRock(info, instance, m_arenaRocks.size());
    if (!admission.diagnostic.empty()) {
        // Each kind of safe rock the arena does not stand is reported once.
        admission.diagnostic += ": " + info.name;
        if (std::ranges::find(m_warnings, admission.diagnostic) == m_warnings.end()) {
            m_warnings.push_back(std::move(admission.diagnostic));
        }
    }
    if (!admission.rock) {
        // One past the roster stays scenery; one the arena cannot stand is never left as it.
        return !admission.ordinary;
    }
    const auto& rock = admission.rock;
    ArenaRockPresentation binding;
    binding.definition = rock->body;
    // SetItem 8c0a8f80 multiplies native base HP by the placed segment count.
    // AddItemWobj 8c0a9d80 changes the static model, not the item's position/body.
    binding.tier = rock->tier;
    binding.health = rock->tier * rock->body.tierHealth;
    binding.armor = rock->armor;
    binding.dormant = rock->dormant;
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    ItemInstance placement;
    placement.position = instance.position;
    placement.rotation = instance.rotation;
    for (usize state = 0; state < binding.figures.size(); ++state) {
        const auto model = scope.findObject(std::format("{}{}", info.name, state));
        if (!model) {
            continue; // Native AddItemWobj hides a genuinely absent damage model.
        }
        const usize figure = m_fixtures.size();
        place(device, model->name, placement, 1, object_flags::kSorted, model->archive);
        if (figure == m_fixtures.size()) {
            continue;
        }
        binding.figures[state] = figure;
        m_hiddenFixtures[figure] = true;
    }
    // AddItemWobj 8c0a9d80 emits FX23 on the final tier change. InitFX 8c0d33d0
    // binds that one-shot to this stage's native SAFEREXP tree.
    if (const auto burst = scope.findTree("SAFEREXP")) {
        const usize figure = m_fixtures.size();
        place(device, "SAFEREXP", placement, 1, object_flags::kSorted, burst->archive);
        if (figure != m_fixtures.size()) {
            binding.burstFigure = figure;
            m_hiddenFixtures[figure] = true;
        }
    }
    m_arenaRocks.push_back(binding);
    selectArenaRock(m_arenaRocks.size() - 1);
    return true;
}

bool LegendsSceneAdapter::bindDreamcastEntranceRock(const formats::ItemInfoRecord& info,
                                                    const formats::ItemInstanceRecord& instance,
                                                    u32 index, usize figure) {
    if (figure >= m_fixtures.size() || !LegendsDreamcastBossEntrance::accepts(info, instance)) {
        return false;
    }
    // The caller supplies the same AddItemSub placement as the visible figure.
    m_dreamcastBossEntrance.add(index, instance.position, instance.rotation);
    m_dreamcastBossEntranceFigures.push_back(figure);
    return true;
}

void LegendsSceneAdapter::cueDreamcastBossArena(Vec3 boss) {
    m_dreamcastBossEntrance.cue(boss);
}

void LegendsSceneAdapter::updateDreamcastBossArena(f32 seconds) {
    m_dreamcastBossEntrance.update(seconds);
    const auto pieces = m_dreamcastBossEntrance.pieces();
    for (usize i = 0; i < pieces.size(); ++i) {
        const auto figure = m_dreamcastBossEntranceFigures[i];
        const auto& motion = pieces[i].motion;
        m_hiddenFixtures[figure] = !motion.visible;
        m_fixtures[figure].placeAt(legendsDreamcastItemPlacement(motion.position, motion.rotation));
    }
}

} // namespace gdl::game
