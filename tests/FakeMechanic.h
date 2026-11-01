#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "game/mechanics/GameplayMechanic.h"
#include "game/mechanics/MechanicCatalog.h"

namespace gdl::test {

/**
 * A mechanic whose every answer a test sets, the seam's own double. Its native state is the
 * swarm steps it was given and the seconds they covered. The facade builds its own, so each
 * one made is listed in `made()` while it lives.
 */
class FakeMechanic final : public game::GameplayMechanic,
                           public game::BlowRules,
                           public game::RunRules,
                           public game::SwarmRules,
                           public game::BirthRules,
                           public game::FallRules,
                           public game::SwarmAppearance,
                           public game::WorldOverlay,
                           public game::HudOverlay {
public:
    struct Answers {
        std::optional<game::RunEffect> runEffect;
        bool generatorTakes = true;
        std::optional<f32> quiet;
        game::SwarmTuning tuning;
        game::BodyTraits traits; ///< for a frenzied birth
        u32 draws = 0;           ///< drawn from the swarm's stream for a frenzied birth
        std::string drop;        ///< left by a body slain with an affinity, when named
        game::SwarmLook look;
        f32 scale = 1; ///< for a body with an affinity
    };
    struct State final : game::MechanicState {
        s32 steps = 0;
        f32 seconds = 0;
    };

    FakeMechanic() { made().push_back(this); }
    ~FakeMechanic() override { std::erase(made(), this); }
    FakeMechanic(const FakeMechanic&) = delete;
    FakeMechanic& operator=(const FakeMechanic&) = delete;
    FakeMechanic(FakeMechanic&&) = delete;
    FakeMechanic& operator=(FakeMechanic&&) = delete;

    /** Every one alive, in the order made. */
    static std::vector<FakeMechanic*>& made() {
        static std::vector<FakeMechanic*> all;
        return all;
    }
    /** How many have been closed, all told: a closed one may be gone the moment after. */
    static s32& closings() {
        static s32 count = 0;
        return count;
    }
    static std::unique_ptr<game::GameplayMechanic> make() {
        return std::make_unique<FakeMechanic>();
    }
    static game::MechanicDescriptor descriptor(std::string_view id, bool roomRule = true,
                                               u32 schema = 1) {
        return {.id = id, .schema = schema, .label = id, .roomRule = roomRule, .create = &make};
    }

    void open(const game::MechanicLevel& level) override {
        ++opened;
        tower = level.tower;
        steps = 0;
        seconds = 0;
    }
    void close() override {
        ++closed;
        ++closings();
    }
    void enable(bool on) override { enabled = on; }
    std::unique_ptr<game::MechanicState> makeState() const override {
        return std::make_unique<State>();
    }
    void saveState(game::MechanicState& saved) const override {
        auto& state = dynamic_cast<State&>(saved);
        state.steps = steps;
        state.seconds = seconds;
    }
    bool restoreState(const game::MechanicState& saved) override {
        const auto& state = dynamic_cast<const State&>(saved);
        steps = state.steps;
        seconds = state.seconds;
        return true;
    }
    bool appendState(NativeStateDigest& digest, const game::MechanicState* saved) const override {
        const auto* state = dynamic_cast<const State*>(saved);
        digest.word(static_cast<u32>(state != nullptr ? state->steps : steps));
        digest.real(state != nullptr ? state->seconds : seconds);
        return digest.good();
    }

    game::BlowRules* blows() override { return this; }
    game::RunRules* runs() override { return this; }
    game::SwarmRules* swarm() override { return this; }
    game::BirthRules* births() override { return this; }
    game::FallRules* falls() override { return this; }
    game::SwarmAppearance* appearance() override { return this; }
    game::WorldOverlay* world() override { return this; }
    game::HudOverlay* hud() override { return this; }

    std::optional<game::RunEffect> runEffect(const game::BlowReport& blow) const override {
        blowsAsked.push_back(blow);
        return answers.runEffect;
    }
    bool generatorTakes(game::BlowSource /*source*/) const override {
        return answers.generatorTakes;
    }
    std::optional<f32> quietWindow(s32 /*count*/) const override { return answers.quiet; }
    void step(const game::SwarmStep& step) override {
        ++steps;
        seconds += step.seconds;
        runsStepped.assign(step.runs.begin(), step.runs.end());
    }
    game::SwarmTuning tuning() const override { return answers.tuning; }
    game::BodyTraits traitsFor(const game::BirthRequest& birth, Mt19937& random) const override {
        if (!birth.frenzied) {
            return {};
        }
        for (u32 draw = 0; draw < answers.draws; ++draw) {
            (void)random();
        }
        return answers.traits;
    }
    void fell(const game::BodyFall& fall, game::BagDrops& drops) override {
        fallsSeen.push_back(fall);
        if (fall.killed && fall.affinity != 0 && !answers.drop.empty()) {
            drops.leave(answers.drop, fall.position);
        }
    }
    game::SwarmLook look(u32 /*frame*/) const override { return answers.look; }
    f32 bodyScale(s32 affinity) const override { return affinity != 0 ? answers.scale : 1.0f; }
    void drawWorld(const game::WorldOverlayView& view) override {
        ++worldDraws;
        bodiesDrawn.assign(view.bodies.begin(), view.bodies.end());
    }
    void drawHud(const game::HudOverlayView& view) override {
        ++hudDraws;
        runsDrawn.assign(view.runs.begin(), view.runs.end());
    }

    Answers answers;
    s32 steps = 0;
    f32 seconds = 0;
    bool enabled = false;
    bool tower = false;
    s32 opened = 0;
    s32 closed = 0;
    s32 worldDraws = 0;
    s32 hudDraws = 0;
    mutable std::vector<game::BlowReport> blowsAsked; ///< every blow asked about
    std::vector<game::SeatRun> runsStepped;           ///< the runs of the last swarm step
    std::vector<game::BodyFall> fallsSeen;
    std::vector<game::SwarmBodyView> bodiesDrawn; ///< the swarm the last world draw saw
    std::vector<game::SeatRunView> runsDrawn;     ///< the runs the last HUD draw saw
};

} // namespace gdl::test
