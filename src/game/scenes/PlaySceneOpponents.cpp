#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/bosses/BossSequence.h"
#include "game/bosses/Bosses.h"
#include "game/combat/CombatArenaTarget.h"
#include "game/combat/HurtKind.h"
#include "game/config/ControlProfiles.h"
#include "game/critters/CombatEvents.h"
#include "game/enemies/BossEncounterHost.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/OpponentHost.h"
#include "game/level/CombatCover.h"
#include "game/level/ImportedCover.h"
#include "game/level/ImportedLevel.h"
#include "game/level/SafeRockCover.h"
#include "game/level/SafeRocks.h"
#include "game/party/PartyRecords.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerImpact.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

/** The scene's answers to what its opponents report: harm to its members, the fixtures'
 * blasts and scenery bursts, experience and generators to its party, levels and lessons on
 * its HUD, and the camera's shake. */
class PlayScene::OpponentRouting final : public OpponentHost {
public:
    explicit OpponentRouting(PlayScene& scene) : m_scene(scene) {}
    void hurtMember(usize member, f32 damage, HurtKind kind, bool directed,
                    const PlayerImpact& impact) override {
        m_scene.hurt(member, damage, kind, directed ? HurtSource::Directed : HurtSource::Undirected,
                     impact);
    }
    void settleBlasts() override { m_scene.settleBlasts(); }
    void blastScenery(const PickupBlastReach& reach, std::vector<s32>& reached) override {
        m_scene.m_fixtures.blastScenery(reach.position, reach.radius, reach.damage, reach.flags,
                                        m_scene.m_players, m_scene.m_fixtureRouting, reached);
    }
    void award(s32 player, s32 amount, bool kill) override {
        m_scene.awardExperience(player, amount, kill);
    }
    void destroyedGenerator(s32 player) override {
        PartyRecords::destroyedGenerator(m_scene.m_players, player);
        if (m_scene.actor(player) != nullptr) {
            m_scene.emitOutput(RumbleOutput{player, 0, ControlFeedback::GeneratorDestroyed});
        }
    }
    void updateLevels() override { m_scene.updateLevels(); }
    bool lesson(s32 id, usize member) override { return m_scene.postHelp(id, member); }
    void shake() override { m_scene.m_sim.shake.start(); }

private:
    PlayScene& m_scene;
};

/** The scene's answers to what a boss encounter asks: the rite, the coins, the fall and the
 * victory through its boss sequence, the arena through its safe rocks; an imported level
 * takes the fall and the arena itself. */
class PlayScene::BossRouting final : public BossEncounterHost {
public:
    explicit BossRouting(PlayScene& scene) : m_scene(scene) {}
    void legend(const LegendEvent& event) override {
        m_scene.m_bossSequence.showLegend(event, m_scene.m_opponents.bosses(), m_scene.m_players);
    }
    void advanceLegend(f32 seconds) override {
        m_scene.m_bossSequence.advanceLegend(seconds, m_scene.m_opponents.bosses(),
                                             m_scene.m_players);
    }
    void spew(const CombatSpew& spew) override {
        m_scene.m_bossSequence.spewCoins(spew, m_scene.m_opponents, m_scene.m_fixtures,
                                         m_scene.m_players);
    }
    void fallen(const Vec3& where) override {
        if (auto* imported = m_scene.m_world->imported()) {
            std::vector<PlayerActor*> survivors;
            for (auto& player : m_scene.m_players) {
                if (!player.departed && player.life == PlayerLife::Standing) {
                    survivors.push_back(&player.actor);
                }
            }
            imported->bossDefeated(where + m_scene.m_opponents.bosses().rewardOffset(), survivors);
        } else {
            m_scene.m_bossSequence.fallen(where, m_scene.m_opponents.bosses(), m_scene.m_players);
        }
    }
    void advanceVictory(s32 ticks, f32 seconds) override { m_scene.updateVictory(ticks, seconds); }
    std::vector<Mat4> arenaAnchors() const override {
        return m_scene.m_world->imported() ? m_scene.m_world->imported()->arenaAnchors()
                                           : m_scene.m_fixtures.safeRocks().attackAnchors();
    }
    std::vector<CombatArenaTarget> arenaTargets() const override {
        return m_scene.m_world->imported() ? m_scene.m_world->imported()->arenaTargets()
                                           : m_scene.m_fixtures.safeRocks().arenaTargets();
    }
    void activateArena(const CombatArenaActivation& activation) override {
        if (auto* imported = m_scene.m_world->imported()) {
            imported->activateArena(activation);
        } else {
            m_scene.m_fixtures.safeRocks().scheduleActivation(activation.index, activation.delay);
        }
    }

private:
    PlayScene& m_scene;
};

void PlayScene::advanceOpponents(s32 ticks, f32 seconds) {
    OpponentRouting host{*this};
    BossRouting encounter{*this};
    const auto advance = [&](const CombatCover& cover) {
        m_opponents.update(ticks, seconds, m_players, m_fixtures.obstacles(), host, encounter,
                           cover, m_fixtures.missileStops(), m_fixtures.critterObstacles());
    };
    if (const auto* imported = m_world->imported()) {
        advance(ImportedCover{*imported});
    } else {
        advance(SafeRockCover{m_fixtures.safeRocks()});
    }
}

void PlayScene::settleOpponentRewards() {
    OpponentRouting host{*this};
    BossRouting encounter{*this};
    m_opponents.settleRewards(m_players, host, encounter);
}

} // namespace gdl::game
