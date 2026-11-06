#pragma once
#include <functional>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/CombatArenaTarget.h"
#include "game/combat/HurtKind.h"
#include "game/enemies/BossEncounterHost.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/OpponentHost.h"
#include "game/level/CombatCover.h"
#include "game/players/PlayerImpact.h"

namespace gdl::test {
/** What a test's opponents report into and ask of the level: a callable per report or
 * question, any of which may stay unset. */
struct OpponentScript {
    std::function<void(usize, f32, game::HurtKind, bool, const game::PlayerImpact&)> hurt;
    std::function<void()> settleBlasts;
    std::function<void(const game::LegendEvent&)> legend;
    std::function<void(f32)> advanceLegend;
    std::function<void(const Vec3&)> fallen;
    std::function<void(const game::CombatSpew&)> spew;
    std::function<void(s32, f32)> advanceVictory;
    std::function<void()> levels;
    std::function<void(s32, s32, bool)> award;
    std::function<void(s32)> destroyedGenerator;
    std::function<bool(const Vec3&, const Vec3&)> blocksBreath;
    std::function<bool(const Vec3&, const Vec3&)> blocksArea;
    std::function<std::vector<Mat4>()> arenaAnchors;
    std::function<std::vector<game::CombatArenaTarget>()> arenaTargets;
    std::function<void(const game::CombatArenaActivation&)> activateArena;
    std::function<void()> shake;
    /** A lesson for the whole party, over the first standing player. */
    std::function<bool(s32, usize)> help;
    game::EnemyMissiles::SceneryBlast blastScenery;
};

/** The opponents' hosts and cover answering from a script they borrow, as the script stands
 * at each report or question: an unset report does nothing, an unset lesson or cover answers
 * false, and the arena has no anchors or targets. */
class ScriptedOpponents final : public game::OpponentHost,
                                public game::BossEncounterHost,
                                public game::CombatCover {
public:
    /** Answers from a script with nothing set. */
    ScriptedOpponents() : ScriptedOpponents(quiet()) {}
    explicit ScriptedOpponents(const OpponentScript& script) : m_script(script) {}
    /** The script must outlive the host. */
    explicit ScriptedOpponents(OpponentScript&& script) = delete;

    void hurtMember(usize member, f32 damage, game::HurtKind kind, bool directed,
                    const game::PlayerImpact& impact) override {
        if (m_script.hurt) {
            m_script.hurt(member, damage, kind, directed, impact);
        }
    }
    void settleBlasts() override {
        if (m_script.settleBlasts) {
            m_script.settleBlasts();
        }
    }
    void blastScenery(const game::PickupBlastReach& reach, std::vector<s32>& reached) override {
        if (m_script.blastScenery) {
            m_script.blastScenery(reach, reached);
        }
    }
    void award(s32 player, s32 amount, bool kill) override {
        if (m_script.award) {
            m_script.award(player, amount, kill);
        }
    }
    void destroyedGenerator(s32 player) override {
        if (m_script.destroyedGenerator) {
            m_script.destroyedGenerator(player);
        }
    }
    void updateLevels() override {
        if (m_script.levels) {
            m_script.levels();
        }
    }
    bool lesson(s32 id, usize member) override {
        return m_script.help && m_script.help(id, member);
    }
    void shake() override {
        if (m_script.shake) {
            m_script.shake();
        }
    }

    void legend(const game::LegendEvent& event) override {
        if (m_script.legend) {
            m_script.legend(event);
        }
    }
    void advanceLegend(f32 seconds) override {
        if (m_script.advanceLegend) {
            m_script.advanceLegend(seconds);
        }
    }
    void spew(const game::CombatSpew& spew) override {
        if (m_script.spew) {
            m_script.spew(spew);
        }
    }
    void fallen(const Vec3& where) override {
        if (m_script.fallen) {
            m_script.fallen(where);
        }
    }
    void advanceVictory(s32 ticks, f32 seconds) override {
        if (m_script.advanceVictory) {
            m_script.advanceVictory(ticks, seconds);
        }
    }
    std::vector<Mat4> arenaAnchors() const override {
        return m_script.arenaAnchors ? m_script.arenaAnchors() : std::vector<Mat4>{};
    }
    std::vector<game::CombatArenaTarget> arenaTargets() const override {
        return m_script.arenaTargets ? m_script.arenaTargets()
                                     : std::vector<game::CombatArenaTarget>{};
    }
    void activateArena(const game::CombatArenaActivation& activation) override {
        if (m_script.activateArena) {
            m_script.activateArena(activation);
        }
    }

    bool blocksBreath(const Vec3& from, const Vec3& to) const override {
        return m_script.blocksBreath && m_script.blocksBreath(from, to);
    }
    bool blocksArea(const Vec3& from, const Vec3& to) const override {
        return m_script.blocksArea && m_script.blocksArea(from, to);
    }

private:
    static const OpponentScript& quiet() {
        static const OpponentScript kScript;
        return kScript;
    }
    const OpponentScript& m_script;
};
} // namespace gdl::test
