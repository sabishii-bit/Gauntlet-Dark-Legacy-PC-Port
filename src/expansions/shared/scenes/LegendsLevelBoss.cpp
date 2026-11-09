#include "engine/core/Error.h"
#include "engine/core/Log.h"

#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

std::unique_ptr<CombatantAssets>
LegendsLevel::loadBossAssets(RenderDevice& device, const std::filesystem::path& commonRoot) const {
    if (m_dreamcastBoss && platform() == LegendsPlatform::Dreamcast) {
        std::vector<std::string> warnings;
        auto assets =
            loadLegendsDreamcastBossAssets(device, m_directory.parent_path().parent_path(),
                                           commonRoot, m_dreamcastBoss->combat.kind, {}, warnings);
        for (const auto& warning : warnings)
            log::warn("{}", warning);
        if (!assets)
            throw FormatError("Native Dreamcast boss family is not supported");
        return assets;
    }
    return loadN64BossAssets(device, commonRoot);
}

void LegendsLevel::bossDefeated(const Vec3& position, std::span<PlayerActor*> players) {
    if (m_dreamcastBoss)
        dreamcastBossDefeated(position, players);
    else
        n64BossDefeated(position, players);
}

void LegendsLevel::bossArenaCue(const Vec3& position) {
    if (platform() == LegendsPlatform::Dreamcast && m_dreamcastBoss &&
        m_dreamcastBoss->nativeKind == 32 && !m_bossDefeated)
        m_world.adapter()->cueDreamcastBossArena(position);
}

} // namespace gdl::game
