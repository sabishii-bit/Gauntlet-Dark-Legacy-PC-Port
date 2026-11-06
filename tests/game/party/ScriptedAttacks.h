#pragma once
#include <functional>

#include "engine/core/Types.h"

#include "game/combat/HurtKind.h"
#include "game/party/AttackHost.h"
#include "game/players/PlayerImpact.h"

namespace gdl::test {
/** What a test's attacks report into: a callable per report, either of which may stay unset. */
struct AttackScript {
    /** One player's blow or burn on another, by the struck member's party index. */
    std::function<void(usize, f32, game::HurtKind, const game::PlayerImpact&)> hurt;
    std::function<void(s32)> meleeHit; ///< by the attacker's input slot
};

/** An attack host answering from a script it borrows, as the script stands at each report: an
 * unset report does nothing. */
class ScriptedAttacks final : public game::AttackHost {
public:
    /** Answers from a script with nothing set. */
    ScriptedAttacks() : ScriptedAttacks(quiet()) {}
    explicit ScriptedAttacks(const AttackScript& script) : m_script(script) {}
    /** The script must outlive the host. */
    explicit ScriptedAttacks(AttackScript&& script) = delete;

    void hurtByAttack(usize member, f32 damage, game::HurtKind kind,
                      const game::PlayerImpact& impact) override {
        if (m_script.hurt) {
            m_script.hurt(member, damage, kind, impact);
        }
    }
    void meleeHit(s32 seat) override {
        if (m_script.meleeHit) {
            m_script.meleeHit(seat);
        }
    }

private:
    static const AttackScript& quiet() {
        static const AttackScript kScript;
        return kScript;
    }
    const AttackScript& m_script;
};
} // namespace gdl::test
