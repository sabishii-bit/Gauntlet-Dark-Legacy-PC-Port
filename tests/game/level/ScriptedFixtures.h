#pragma once
#include <functional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/HurtKind.h"
#include "game/level/FixtureHost.h"
#include "game/players/PlayerImpact.h"

namespace gdl::test {
/** What a test's fixtures report into: a callable per report, any of which may stay unset. */
struct FixtureScript {
    std::function<void(usize, f32, game::HurtKind, bool, const game::PlayerImpact&)> hurt;
    std::function<bool(s32, usize)> help; ///< whether the message went up
    std::function<void(s32, std::string_view)> card;
    /** A blast's ring reaching `radius` about a point with `damage`: the swarm, the great
     * ones, the boss and the generators within it and not yet in the blast's `reached` (ids
     * of the caller's), which a blow over two joins. */
    std::function<void(const Vec3&, f32, f32, std::vector<s32>&, u32)> opponents;
    std::function<bool(s32, const Vec3&, s32)> releaseEnemy;
    std::function<void(s32, const Vec3&)> shatterPotion;
};

/** A fixture host answering from a script it borrows, as the script stands at each report: an
 * unset report does nothing, and an unset lesson or release answers false. */
class ScriptedFixtures final : public game::FixtureHost {
public:
    /** Answers from a script with nothing set. */
    ScriptedFixtures() : ScriptedFixtures(quiet()) {}
    explicit ScriptedFixtures(const FixtureScript& script) : m_script(script) {}
    /** The script must outlive the host. */
    explicit ScriptedFixtures(FixtureScript&& script) = delete;

    void hurtMember(usize member, f32 damage, game::HurtKind kind, bool directed,
                    const game::PlayerImpact& impact) override {
        if (m_script.hurt) {
            m_script.hurt(member, damage, kind, directed, impact);
        }
    }
    bool lesson(s32 id, usize member) override {
        return m_script.help && m_script.help(id, member);
    }
    void card(s32 seat, std::string_view name) override {
        if (m_script.card) {
            m_script.card(seat, name);
        }
    }
    void blastOpponents(const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                        u32 flags) override {
        if (m_script.opponents) {
            m_script.opponents(position, radius, damage, reached, flags);
        }
    }
    bool releaseEnemy(s32 record, const Vec3& position, s32 count) override {
        return m_script.releaseEnemy && m_script.releaseEnemy(record, position, count);
    }
    void shatterPotion(s32 kind, const Vec3& position) override {
        if (m_script.shatterPotion) {
            m_script.shatterPotion(kind, position);
        }
    }

private:
    static const FixtureScript& quiet() {
        static const FixtureScript kScript;
        return kScript;
    }
    const FixtureScript& m_script;
};
} // namespace gdl::test
