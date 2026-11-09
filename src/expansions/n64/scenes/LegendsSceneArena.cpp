#include <cmath>
#include <format>

#include "engine/assets/AssetScope.h"
#include "engine/core/Error.h"

#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/level/BossKinds.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Arena(RenderDevice& device, const ImportedBoss& boss) {
    if (boss.arenaRocks.size() > 16 || !m_arenaRocks.empty()) {
        throw FormatError("N64 Legends arena: invalid cover roster");
    }
    m_hasBossEntrance = boss.kind == boss_type::kSkorne1;
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    for (const auto& definition : boss.arenaRocks) {
        if (!std::isfinite(definition.position.x) || !std::isfinite(definition.position.y) ||
            !std::isfinite(definition.position.z) || !std::isfinite(definition.yaw) ||
            !std::isfinite(definition.radius) || !std::isfinite(definition.height) ||
            definition.radius <= 0 || definition.height <= 0 || definition.tierHealth < 1 ||
            definition.tierHealth > 100000) {
            throw FormatError("N64 Legends arena: invalid native cover body");
        }
        ArenaRockPresentation binding;
        binding.definition = definition;
        binding.health = definition.tierHealth * 3;
        // Native Boss.bin e0888cd4 starts every rock at L3. The imported Yeti
        // intentionally uses GDL's dormant/eruption mechanic, retaining the native
        // authored anchor roster instead of the counterpart stage's positions.
        binding.dormant = boss.kind == boss_type::kYeti;
        ItemInstance placement;
        placement.position = definition.position;
        placement.rotation.y = definition.yaw;
        const auto bind = [&](std::string_view sequence, bool required) -> std::optional<usize> {
            const auto name = formats::legendsN64TreeName("SAFEROCK", sequence);
            if (!scope.findTree(name)) {
                if (required) {
                    throw FormatError("N64 Legends arena: missing " + name);
                }
                return std::nullopt;
            }
            const usize index = m_fixtures.size();
            place(device, name, placement);
            if (m_fixtures.size() == index) {
                throw FormatError("N64 Legends arena: cannot bind " + name);
            }
            m_hiddenFixtures[index] = true;
            return index;
        };
        for (usize tier = 0; tier < binding.figures.size(); ++tier) {
            binding.figures[tier] = bind(std::format("L{}", tier), tier != 0);
        }
        binding.burstFigure = bind("EXP", false);
        m_arenaRocks.push_back(binding);
        selectArenaRock(m_arenaRocks.size() - 1);
    }
}

void LegendsSceneAdapter::beginBossEntrance() {
    m_bossEruption.begin();
}

} // namespace gdl::game
