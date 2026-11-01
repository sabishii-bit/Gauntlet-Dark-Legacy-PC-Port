#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

std::unique_ptr<CombatantAssets>
LegendsLevel::loadBossAssets(RenderDevice& device, const std::filesystem::path& commonRoot) const {
    if (!m_boss || platform() != LegendsPlatform::N64)
        return {};
    const auto root = m_directory.parent_path().parent_path();
    const auto program = AssetLocator(root).find("_program/game.bin");
    if (!program)
        throw FileError("Native boss requires the N64 program tables");
    std::vector<std::string> warnings;
    auto assets = loadLegendsBossAssets(device, root, commonRoot, m_boss->combat.kind,
                                        readFile(*program), {}, warnings);
    for (const auto& warning : warnings)
        log::warn("{}", warning);
    if (!assets)
        throw FormatError("Native boss family is not supported");
    return assets;
}

void LegendsLevel::bossIntro(const Vec3& position) {
    if (platform() == LegendsPlatform::N64 && m_boss && m_boss->map == "boss2" &&
        m_boss->combat.kind == 42 && !m_bossDefeated && !glm::any(glm::isnan(position)) &&
        !glm::any(glm::isinf(position))) {
        // GDL-style imported entrance: the original N64 SFXX dispatcher lacks
        // GDL's arena callback. Bind the authored debris to the actual START
        // handoff, never a map-load timer or an invented removable floor cap.
        m_world.adapter()->beginBossEntrance();
    }
}

void LegendsLevel::bossDefeated(const Vec3& position, std::span<PlayerActor*> players) {
    if (!m_boss || m_bossDefeated || players.empty()) {
        return;
    }
    m_bossDefeated = true;
    trackBoss(std::nullopt);
    m_bossDepartureSeconds = 0;
    // These are native script grants, separate from the physical shard/armor
    // drops below and from every Dark Legacy campaign flag.
    for (auto* actor : players) {
        if (!actor) {
            continue;
        }
        auto& progress = actor->save().progress().legendsProgress(platform());
        progress.recordCompleted(paths::utf8(m_directory.filename()));
        if (m_boss->sharedObelisk >= 0) {
            progress.add(LegendsRelic::Obelisk, m_boss->sharedObelisk);
        }
        if (m_boss->clearsRunestones && m_boss->finalNativeGrant == 0) {
            progress.runestones = 0;
        }
    }
    m_world.adapter()->spawnBossRewards(position, m_playerStart);
    if (m_boss->finalNativeGrant != 0) {
        // Native 0x300290 is permanent, locked Anti-Death. GDL's timed,
        // toggleable inventory must not silently turn it into another reward.
        log::warn("Legends final victory: permanent Anti-Death reward is not yet supported; "
                  "retaining runestones");
    }
}

} // namespace gdl::game
