#include <algorithm>
#include <cmath>

#include "engine/platform/Paths.h"

#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

void LegendsLevel::dreamcastBossDefeated(const Vec3& position, std::span<PlayerActor*> players) {
    const auto finite = [](Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!m_dreamcastBoss || m_bossDefeated || !finite(position)) {
        return;
    }
    std::vector<PlayerActor*> admitted;
    std::vector<Vec3> positions;
    u64 runes = 0;
    u64 shards = 0;
    const s32 realm = m_dreamcastBoss->nativeRealm;
    for (auto* actor : players) {
        if (actor == nullptr || actor->player() < 0 || actor->player() >= 4 ||
            !finite(actor->position()) || std::ranges::find(admitted, actor) != admitted.end()) {
            continue;
        }
        admitted.push_back(actor);
        positions.push_back(actor->position());
        const auto& progress = actor->save().progress().legendsProgress(LegendsPlatform::Dreamcast);
        runes |= progress.runestones;
        shards |= progress.shards;
    }
    if (admitted.empty() || realm < 1 || realm > 6) {
        return;
    }
    // BossEnd 8c071300 calls 8c0a6360 for each living participant before
    // GoodWizard examines the party. Its shard is a script grant (1<<realm),
    // not a collectible whose pickup can race the speech or next-stage load.
    shards |= u64{1} << realm;
    // BossInit 8c071120 retains the original locator for Sumner's midpoint,
    // independently of where the moving boss eventually falls. BossEnd starts the
    // key at `position`, the boss's spawn root plus its key offset, wherever it
    // fell (8c071370-8c0713b2).
    if (!m_dreamcastVisit.begin(m_dreamcastBoss->nativeKind, realm, runes, shards,
                                m_dreamcastBoss->combat.position, positions, position)) {
        return;
    }
    for (auto* actor : admitted) {
        auto& progress = actor->save().progress().legendsProgress(LegendsPlatform::Dreamcast);
        progress.add(LegendsRelic::Shard, realm);
        progress.recordCompleted(paths::utf8(m_directory.filename()));
    }
    m_bossDefeated = true;
    trackBoss(std::nullopt);
}

} // namespace gdl::game
