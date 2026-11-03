#pragma once
#include <functional>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/hud/PowerupSelector.h"
#include "game/party/PartyCombo.h"
#include "game/party/PartyMotion.h"
#include "game/players/PlayerAnimator.h"

namespace gdl::test {
/** What a test's level answers a combo's flier or charger with: a callable per question, any of
 * which may stay unset. */
struct ComboScript {
    /** A flier or charger against the level's items: true when it struck one. */
    std::function<bool(usize, usize, f32)> impact;
    std::function<void(usize, s32, f32)> advanceTurbo;
    /** The shared view's limit on a step: where `after` may end, given `before`. */
    std::function<Vec3(usize, const Vec3&, const Vec3&)> limitMovement;
};

/** A combo's level answering from a script it borrows, as the script stands at each question:
 * an unset report does nothing, an unset impact strikes nothing and an unset limit holds no
 * step back. */
class ScriptedCombo final : public game::PartyCombo::Level {
public:
    /** Answers from a script with nothing set. */
    ScriptedCombo() : ScriptedCombo(quiet()) {}
    explicit ScriptedCombo(const ComboScript& script) : m_script(script) {}
    /** The script must outlive the level. */
    explicit ScriptedCombo(ComboScript&& script) = delete;

    bool comboImpact(usize body, usize thrower, f32 blow) override {
        return m_script.impact && m_script.impact(body, thrower, blow);
    }
    void advanceTurbo(usize index, s32 ticks, f32 seconds) override {
        if (m_script.advanceTurbo) {
            m_script.advanceTurbo(index, ticks, seconds);
        }
    }
    Vec3 limitMovement(usize index, const Vec3& before, const Vec3& after) const override {
        return m_script.limitMovement ? m_script.limitMovement(index, before, after) : after;
    }

private:
    static const ComboScript& quiet() {
        static const ComboScript script;
        return script;
    }
    const ComboScript& m_script;
};

/** What a test's level answers the party's movement with: a callable per question, any of
 * which may stay unset, and whether automatic melee chains held swings. */
struct MotionScript {
    std::function<void(usize, game::PartyMotion::Action)> perform;
    std::function<void(usize, const game::SelectorInput&, s32)> select;
    std::function<void(usize, s32, f32)> advanceTurbo;
    std::function<void(usize, f32)> thrownImpact;
    std::function<std::optional<Vec3>(usize)> aim;
    /** Shared-view tangent projection, rechecked against world and creature collision. */
    std::function<Vec3(usize, const Vec3&, const Vec3&)> limitMovement;
    /** What the attack buttons ask: strong or not, and whether the stick moves. */
    std::function<game::PlayerDeed(usize, bool, bool, const Vec3&)> attackDeed;
    /** A walking/running contact without pressing attack, along the requested heading. */
    std::function<game::PlayerDeed(usize, const Vec3&)> automaticMeleeDeed;
    /** Where the nearest thing to strike lies, an attack button held or not. */
    std::function<game::MeleeSense(usize, bool, const Vec3&)> meleeSense;
    /** Pure contact query before movement; a current hold suppresses attack input. */
    std::function<std::optional<Vec3>(usize)> deathContact;
    /** Where the Death a halo wearer holds stands, when one is held this step (ticks, and
     * whether a hold may be made at all). Applied once, after resolved movement. */
    std::function<std::optional<Vec3>(usize, s32, bool)> grabDeath;
    /** Dynamic creature collision, before the camera limit and action events. */
    std::function<Vec3(usize, const Vec3&, const Vec3&)> resolveMovement;
    /** Where a body fallen out of the world with nobody to stand beside goes. */
    std::function<std::optional<Vec3>()> startPoint;
    /** A combo's flier or charger (its index, its thrower's, the blow it deals) against
     * the level's items; true when it struck one, which turns it. */
    std::function<bool(usize, usize, f32)> comboImpact;
    /** Whether the developer console holds a player's body aloft (noclip). */
    std::function<bool(usize)> flying;
    /** Automatic-melee mode counts held close swings toward heavy finishers. */
    bool chainHeldMelee = false;
};

/** A movement level answering from a script it borrows, as the script stands at each
 * question, the way a level with nothing in it would where the script is unset: reports do
 * nothing, the attack buttons ask a plain or strong attack, nothing is aimed at, sensed, held
 * or struck, nobody flies, and every step stands as it was taken. */
class ScriptedMotion final : public game::PartyMotion::Level {
public:
    /** Answers from a script with nothing set. */
    ScriptedMotion() : ScriptedMotion(quiet()) {}
    explicit ScriptedMotion(const MotionScript& script) : m_script(script) {}
    /** The script must outlive the level. */
    explicit ScriptedMotion(MotionScript&& script) = delete;

    void perform(usize index, game::PartyMotion::Action action) override {
        if (m_script.perform) {
            m_script.perform(index, action);
        }
    }
    void stepSelector(usize index, const game::SelectorInput& input, s32 ticks) override {
        if (m_script.select) {
            m_script.select(index, input, ticks);
        }
    }
    void thrownImpact(usize index, f32 damage) override {
        if (m_script.thrownImpact) {
            m_script.thrownImpact(index, damage);
        }
    }
    std::optional<Vec3> aim(usize index) const override {
        return m_script.aim ? m_script.aim(index) : std::nullopt;
    }
    game::PlayerDeed attackDeed(usize index, bool strong, bool moved,
                                const Vec3& facing) const override {
        if (m_script.attackDeed) {
            return m_script.attackDeed(index, strong, moved, facing);
        }
        return strong ? game::PlayerDeed::StrongAttack : game::PlayerDeed::Attack;
    }
    game::PlayerDeed automaticMeleeDeed(usize index, const Vec3& facing) const override {
        return m_script.automaticMeleeDeed ? m_script.automaticMeleeDeed(index, facing)
                                           : game::PlayerDeed::None;
    }
    std::optional<game::MeleeSense> meleeSense(usize index, bool held,
                                               const Vec3& facing) const override {
        if (m_script.meleeSense) {
            return m_script.meleeSense(index, held, facing);
        }
        return std::nullopt;
    }
    /** A hold the contact answers is kept after movement only by grabDeath's answer, so a
     * script that answers the one must answer the other. */
    std::optional<Vec3> deathContact(usize index) const override {
        if (!m_script.deathContact) {
            return std::nullopt;
        }
        CHECK(static_cast<bool>(m_script.grabDeath));
        return m_script.deathContact(index);
    }
    std::optional<Vec3> grabDeath(usize index, s32 ticks, bool allowed) override {
        return m_script.grabDeath ? m_script.grabDeath(index, ticks, allowed) : std::nullopt;
    }
    bool chainsHeldMelee() const override { return m_script.chainHeldMelee; }
    Vec3 resolveMovement(usize index, const Vec3& from, const Vec3& to) const override {
        return m_script.resolveMovement ? m_script.resolveMovement(index, from, to) : to;
    }
    std::optional<Vec3> startPoint() const override {
        return m_script.startPoint ? m_script.startPoint() : std::nullopt;
    }
    bool flying(usize index) const override { return m_script.flying && m_script.flying(index); }
    bool comboImpact(usize body, usize thrower, f32 blow) override {
        return m_script.comboImpact && m_script.comboImpact(body, thrower, blow);
    }
    void advanceTurbo(usize index, s32 ticks, f32 seconds) override {
        if (m_script.advanceTurbo) {
            m_script.advanceTurbo(index, ticks, seconds);
        }
    }
    Vec3 limitMovement(usize index, const Vec3& before, const Vec3& after) const override {
        return m_script.limitMovement ? m_script.limitMovement(index, before, after) : after;
    }

private:
    static const MotionScript& quiet() {
        static const MotionScript script;
        return script;
    }
    const MotionScript& m_script;
};
} // namespace gdl::test
