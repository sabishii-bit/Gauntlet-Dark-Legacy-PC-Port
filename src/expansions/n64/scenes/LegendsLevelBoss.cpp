#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/level/BossKinds.h"

namespace gdl::game {

std::unique_ptr<CombatantAssets>
LegendsLevel::loadN64BossAssets(RenderDevice& device,
                                const std::filesystem::path& commonRoot) const {
    if (!m_boss || platform() != LegendsPlatform::N64) {
        return {};
    }
    const auto root = m_directory.parent_path().parent_path();
    const auto program = AssetLocator(root).find("_program/game.bin");
    if (!program) {
        throw FileError("Native boss requires the N64 program tables");
    }
    std::vector<std::string> warnings;
    auto assets = loadLegendsBossAssets(device, root, commonRoot, m_boss->combat.kind,
                                        readFile(*program), {}, warnings);
    for (const auto& warning : warnings) {
        log::warn("{}", warning);
    }
    if (!assets) {
        throw FormatError("Native boss family is not supported");
    }
    return assets;
}

void LegendsLevel::bossIntro(const Vec3& position) {
    if (platform() == LegendsPlatform::N64 && m_boss && m_boss->map == "boss2" &&
        m_boss->combat.kind == boss_type::kSkorne1 && !m_bossDefeated &&
        !glm::any(glm::isnan(position)) && !glm::any(glm::isinf(position))) {
        // GDL-style imported entrance: the original N64 SFXX dispatcher lacks
        // GDL's arena callback. Bind the authored debris to the actual START
        // handoff, never a map-load timer or an invented removable floor cap.
        m_world.adapter()->beginBossEntrance();
    }
}

void LegendsLevel::bossDeathPlayed(const Vec3& position) {
    // Boss.bin's DEATH raises e088eaf0 on every frame of its hold (e0884808); the play loop
    // runs the critters (e0044e80) before e006a134 (e0044f0c), which, its departure clock not
    // yet set, throws the rewards the frame it first sees the flag (e006a5a8-e006a61c) about
    // e088f990, where the boss's process last published it (e0884d60-e0884dac, e0889850).
    if (platform() == LegendsPlatform::N64 && m_boss && m_bossCamera && !m_bossDefeated) {
        m_world.adapter()->spawnBossRewards(legendsN64BossFocus(*m_bossCamera, position),
                                            m_playerStart);
    }
}

void LegendsLevel::n64BossDefeated(const Vec3& /*keySpot*/, std::span<PlayerActor*> players) {
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
    // The N64 drops no key; its rewards flew as the death played (bossDeathPlayed).
    if (m_boss->finalNativeGrant != 0) {
        // Native 0x300290 is permanent, locked Anti-Death. GDL's timed,
        // toggleable inventory must not silently turn it into another reward.
        log::warn("Legends final victory: permanent Anti-Death reward is not yet supported; "
                  "retaining runestones");
    }
}

} // namespace gdl::game
