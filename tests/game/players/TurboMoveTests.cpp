#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/players/ComboMove.h"
#include "game/players/TurboMove.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Action = PlayerAnimator::Action;

struct Fixture {
    TurboMove move;
    TurboMeter meter;
    ClassStats stats;
    std::vector<std::string> calls;
    std::vector<Vec3> shots;
    TurboMove::Events events{
        .announce = [this](s32 id) { calls.push_back("help" + std::to_string(id)); },
        .dim = [this](f32) { calls.emplace_back("dim"); },
        .volley = [this](const Vec3& direction) { shots.push_back(direction); },
        .strike =
            [this](s32 row) {
                if (stats.moveStrikes[static_cast<usize>(row)].amount != 0) {
                    REQUIRE(move.owed() == 0); // Payment precedes world effects.
                }
                calls.push_back("strike" + std::to_string(row));
            }};
    Fixture() {
        meter.add(100);
        stats.moves.turboB = 0;
        MoveStrike window;
        window.type = MoveStrike::kWindow;
        window.startFrame = 0;
        window.endFrame = 3;
        window.flags = MoveStrike::kHidesWeapon | 0x10;
        window.next = 1;
        window.help = 77;
        MoveStrike hit;
        hit.startFrame = 2;
        hit.amount = 5;
        stats.moveStrikes = {window, hit};
    }
    void advance(f32 frame, Action action = Action::TurboStrong) {
        move.advance(action, frame, Vec3{0, 0, 1}, &stats, meter, events);
    }
};

TEST_CASE("turbo timeline pays at the first damaging strike and names the move once",
          "[game][players][turbo-move]") {
    Fixture f;
    REQUIRE(f.move.begin(Action::TurboStrong, &f.stats, f.meter).empty());
    f.advance(0);
    REQUIRE(f.move.weaponHidden());
    REQUIRE(f.meter.held() == 100);
    REQUIRE(f.move.owed() == TurboMeter::kStrongCost);
    f.advance(1);
    f.advance(2);
    REQUIRE(f.meter.held() == 60);
    f.advance(3);
    REQUIRE_FALSE(f.move.weaponHidden());
    REQUIRE(f.calls ==
            std::vector<std::string>{"dim", "strike0", "help77", "dim", "dim", "strike1"});
}

TEST_CASE("interrupting a turbo timeline cancels its debt and future strikes",
          "[game][players][turbo-move]") {
    Fixture f;
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(0);
    f.calls.clear();
    f.advance(1, Action::Ready);
    f.advance(10);
    REQUIRE(f.calls.empty());
    REQUIRE(f.move.owed() == 0);
    REQUIRE_FALSE(f.move.weaponHidden());
    REQUIRE(f.meter.held() == 100);
}

TEST_CASE("turbo without class rows pays immediately but strong throws stay free",
          "[game][players][turbo-move]") {
    Fixture f;
    REQUIRE(f.move.begin(Action::TurboFull, nullptr, f.meter) == "TURBOC");
    REQUIRE(f.meter.held() == 0);
    f.meter.add(100);
    REQUIRE(f.move.begin(Action::TurboStrong, nullptr, f.meter) == "TURBOB");
    REQUIRE(f.meter.held() == 60);
    f.stats.moves.turboAThrow = 0;
    REQUIRE(f.move.begin(Action::StrongThrow, &f.stats, f.meter).empty());
    f.advance(2, Action::StrongThrow);
    REQUIRE(f.meter.held() == 60);
}

TEST_CASE("the power swings and the spin play their own strong-attack rows for nothing",
          "[game][players][turbo-move][melee]") {
    struct Case {
        Action action;
        s32 ClassMoves::*row;
    };
    for (const Case& c : {Case{Action::PowerClose, &ClassMoves::turboAClose},
                          Case{Action::PowerLow, &ClassMoves::turboALow},
                          Case{Action::PowerMed, &ClassMoves::turboAStep},
                          Case{Action::Spin, &ClassMoves::turboA360}}) {
        Fixture f;
        f.stats.moves.turboB = -1;
        f.stats.moves.*c.row = 1;
        REQUIRE(f.move.begin(c.action, &f.stats, f.meter).empty());
        f.advance(2, c.action);
        CHECK(f.calls == std::vector<std::string>{"strike1"});
        CHECK(f.meter.held() == 100);
        CHECK(f.move.owed() == 0);
        // Its rows go with the action: the recovery plays none of them.
        f.move.begin(c.action, &f.stats, f.meter);
        f.calls.clear();
        f.advance(2, Action::PowerCloseRecover);
        CHECK(f.calls.empty());
    }
}

TEST_CASE("full turbo combines both chains and pays only once", "[game][players][turbo-move]") {
    Fixture f;
    f.stats.moves.turboC1 = 0;
    f.stats.moves.turboC2 = 1;
    f.move.begin(Action::TurboFull, &f.stats, f.meter);
    f.advance(2, Action::TurboFull);
    REQUIRE(f.meter.held() == 0);
    REQUIRE(f.calls == std::vector<std::string>{"help77", "dim", "strike0", "strike1", "strike1"});
}

TEST_CASE("turbo volleys catch up skipped frames without repeating shots",
          "[game][players][turbo-move]") {
    Fixture f;
    MoveStrike volley;
    volley.type = MoveStrike::kVolley;
    volley.startFrame = 0;
    volley.endFrame = 6;
    volley.delay = 2;
    volley.angle = 1;
    volley.flags = MoveStrike::kSweepsIn;
    f.stats.moveStrikes = {volley};
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(4);
    REQUIRE(f.shots.size() == 3);
    REQUIRE(f.shots[0].x == Approx(std::sin(1.0f)));
    REQUIRE(f.shots[1].x == Approx(std::sin(2.0f / 3.0f)));
    REQUIRE(f.shots[2].x == Approx(std::sin(1.0f / 3.0f)));
    f.advance(4);
    f.advance(6);
    REQUIRE(f.shots.size() == 3);
    REQUIRE(f.meter.held() == 100);
}

TEST_CASE(
    "identified turbo strikes retain chain occurrences through partial consumption and rewind",
    "[game][players][turbo-move][turbo-strike-identity]") {
    Fixture f;
    // Row 1 occurs twice in C, but fires before the earlier row 0. Removing
    // those pending entries must not renumber the remaining occurrence.
    f.stats.moveStrikes[0].startFrame = 5;
    f.stats.moveStrikes[1].startFrame = 2;
    f.stats.moves.turboC1 = 0;
    f.stats.moves.turboC2 = 1;
    std::vector<TurboMove::Strike> observed;
    f.events.identifiedStrike = [&](const auto& strike) {
        if (f.stats.moveStrikes[static_cast<usize>(strike.row)].amount != 0) {
            CHECK(f.move.owed() == 0);
            CHECK(f.meter.held() == 0);
        }
        observed.push_back(strike);
    };
    f.move.begin(Action::TurboFull, &f.stats, f.meter);
    f.advance(2, Action::TurboFull);
    CHECK(observed == std::vector<TurboMove::Strike>{{1, 1}, {1, 2}});
    CHECK(
        std::ranges::none_of(f.calls, [](const auto& call) { return call.starts_with("strike"); }));
    // The player's simulation holds its move and meter by value: copies are its rewind.
    Fixture replay;
    replay.stats = f.stats;
    replay.move = f.move;
    replay.meter = f.meter;
    std::vector<TurboMove::Strike> repeated;
    replay.events.identifiedStrike = [&](const auto& strike) { repeated.push_back(strike); };
    observed.clear();
    for (const f32 frame : {2.0f, 4.0f, 5.0f, 7.0f}) {
        f.advance(frame, Action::TurboFull);
        replay.advance(frame, Action::TurboFull);
        CHECK(repeated == observed);
        CHECK(replay.move == f.move);
        CHECK(replay.meter == f.meter);
    }
    CHECK(observed == std::vector<TurboMove::Strike>{{0, 0}});
}

TEST_CASE(
    "identified strike and volley observers preserve native dispatch order without duplication",
    "[game][players][turbo-move][turbo-strike-identity]") {
    Fixture f;
    f.stats.moveStrikes[0].type = MoveStrike::kVolley;
    f.stats.moveStrikes[0].flags = 0;
    f.stats.moveStrikes[0].help = -1;
    f.stats.moveStrikes[0].delay = 1;
    std::vector<std::string> calls;
    f.events.identifiedVolley = [&](const auto& shot) {
        calls.push_back("volley" + std::to_string(shot.index));
    };
    f.events.identifiedStrike = [&](const auto& strike) {
        calls.push_back("strike" + std::to_string(strike.slot));
    };
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(2);
    CHECK(calls == std::vector<std::string>{"volley0", "volley1", "volley2", "strike0", "strike1"});
    CHECK(f.calls.empty());
    CHECK(f.shots.empty());
    CHECK(f.meter.held() == 60);
    f.advance(2);
    CHECK(calls.size() == 5);
    f.advance(3, Action::Ready);
    f.advance(4);
    CHECK(calls.size() == 5);
}

TEST_CASE("identified turbo volleys preserve native slot order and skipped-frame counters",
          "[game][players][turbo-move][turbo-volley-identity]") {
    Fixture f;
    MoveStrike volley;
    volley.type = MoveStrike::kVolley;
    volley.startFrame = 0;
    volley.endFrame = 12;
    volley.delay = 2;
    volley.angle = 1;
    volley.flags = MoveStrike::kSweepsIn;
    f.stats.moveStrikes = {volley};
    // The same authored row occurs once in each full-turbo chain. Its two
    // independent counters must not collapse to one identity or interleave.
    f.stats.moves.turboC1 = f.stats.moves.turboC2 = 0;
    std::vector<TurboMove::Volley> observed;
    f.events.identifiedVolley = [&](const TurboMove::Volley& shot) { observed.push_back(shot); };
    f.move.begin(Action::TurboFull, &f.stats, f.meter);
    f.advance(4, Action::TurboFull);
    REQUIRE(observed.size() == 6);
    CHECK(f.shots.empty()); // identified callback replaces the legacy callback
    for (usize slot = 0; slot < 2; ++slot) {
        for (s32 index = 0; index < 3; ++index) {
            const auto& shot = observed[slot * 3 + static_cast<usize>(index)];
            CHECK(shot.slot == slot);
            CHECK(shot.index == index);
            CHECK(shot.emissionIdentity() == ((slot + 1) << 16U) + index);
            const f32 angle = 1 - static_cast<f32>(index) / 6;
            CHECK(shot.direction.x == Approx(std::sin(angle)));
            CHECK(shot.direction.z == Approx(std::cos(angle)));
        }
    }
    CHECK(f.meter.held() == 100); // identity observation does not cause payment
    f.advance(4, Action::TurboFull);
    CHECK(observed.size() == 6);
    Fixture replayed;
    replayed.stats = f.stats;
    std::vector<TurboMove::Volley> repeated;
    replayed.events.identifiedVolley = [&](const TurboMove::Volley& shot) {
        repeated.push_back(shot);
    };
    // The player's simulation holds its move and meter by value: copies are its rewind.
    replayed.move = f.move;
    replayed.meter = f.meter;
    CHECK(repeated.empty());
    observed.clear();
    for (const f32 frame : {4.0f, 5.0f, 10.0f, 10.0f, 12.0f}) {
        f.advance(frame, Action::TurboFull);
        replayed.advance(frame, Action::TurboFull);
        CHECK(repeated == observed);
        CHECK(replayed.move == f.move);
        CHECK(replayed.meter == f.meter);
    }
    REQUIRE(observed.size() == 6);
    CHECK(observed.front().slot == 0);
    CHECK(observed.front().index == 3);
    CHECK(observed.back().slot == 1);
    CHECK(observed.back().index == 5);
    CHECK(replayed.shots.empty());
}

TEST_CASE("turbo emission identities reject unrepresentable coordinates without aliasing",
          "[game][players][turbo-move][turbo-volley-identity]") {
    CHECK(TurboMove::Volley{{}, 0, 0}.emissionIdentity() == 0x00010000U);
    CHECK(TurboMove::Volley{{}, 127, 65535}.emissionIdentity() == 0x0080FFFFU);
    CHECK(TurboMove::Volley{{}, 128, 0}.emissionIdentity() == 0);
    CHECK(TurboMove::Volley{{}, std::numeric_limits<usize>::max(), 0}.emissionIdentity() == 0);
    CHECK(TurboMove::Volley{{}, 0, -1}.emissionIdentity() == 0);
    CHECK(TurboMove::Volley{{}, 0, 65536}.emissionIdentity() == 0);
    CHECK(TurboMove::Volley{{}, 0, std::numeric_limits<s32>::max()}.emissionIdentity() == 0);

    Fixture f;
    MoveStrike volley;
    volley.type = MoveStrike::kVolley;
    volley.endFrame = 65537;
    volley.delay = 1;
    f.stats.moveStrikes = {volley};
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    // A shot a frame: the first 65535 go out through the plain volley observer.
    f.advance(65534);
    REQUIRE(f.shots.size() == 65535);
    std::vector<u32> identities;
    f.events.identifiedVolley = [&](const TurboMove::Volley& shot) {
        identities.push_back(shot.emissionIdentity());
    };
    f.advance(65535);
    CHECK(identities == std::vector<u32>{0x0001FFFFU});
    f.advance(65536);
    CHECK(identities == std::vector<u32>{0x0001FFFFU, 0});
}

TEST_CASE("non-volley turbo rows never dispatch emission identities",
          "[game][players][turbo-move][turbo-volley-identity]") {
    Fixture f;
    usize emissions = 0;
    f.events.identifiedVolley = [&](const TurboMove::Volley&) { ++emissions; };
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    for (const f32 frame : {0.0f, 1.0f, 2.0f, 3.0f, 10.0f}) {
        f.advance(frame);
    }
    CHECK(emissions == 0);
    CHECK(f.shots.empty());
    CHECK(f.meter.held() == 60);
    CHECK(f.calls == std::vector<std::string>{"dim", "strike0", "help77", "dim", "dim", "strike1"});
}

TEST_CASE("the combo pays half the meter as it begins and runs its class's combo rows through "
          "the first act",
          "[game][players][turbo-move][combo]") {
    Fixture f;
    f.stats.moves.combo1 = 1; // the damaging row alone, at its second frame
    f.stats.moveStrikes[1].help = 102;
    REQUIRE(f.move.begin(Action::ComboAct1, &f.stats, f.meter).empty());
    REQUIRE(f.meter.held() == 100 - ComboMove::kCost);
    REQUIRE(f.move.owed() == 0);
    f.advance(0, Action::ComboAct1);
    REQUIRE(f.calls.empty());
    f.advance(2, Action::ComboAct1);
    REQUIRE(f.calls == std::vector<std::string>{"help102", "strike1"});
    // Out of the act, nothing more runs; a class without rows still pays.
    f.advance(3, Action::ComboAct2);
    REQUIRE(f.calls.size() == 2);
    f.stats.moves.combo1 = -1;
    REQUIRE(f.move.begin(Action::ComboAct1, &f.stats, f.meter).empty());
    REQUIRE(f.meter.held() == 100 - 2 * ComboMove::kCost);
    f.advance(2, Action::ComboAct1);
    REQUIRE(f.calls.size() == 2);
}
TEST_CASE("turbo rewind resumes delayed payment without reissuing consumed strikes",
          "[game][players][turbo-move][rollback][turbo-checkpoint]") {
    Fixture f;
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(1);
    REQUIRE(f.move.weaponHidden());
    REQUIRE(f.move.owed() == 40);
    // The player's simulation holds its move and meter by value: copies are its rewind.
    Fixture replayed;
    replayed.meter = f.meter;
    replayed.move = f.move;
    CHECK(replayed.calls.empty());
    CHECK(replayed.shots.empty());
    CHECK(replayed.meter.held() == 100);
    f.calls.clear();
    for (const f32 frame : {1.0f, 1.5f, 2.0f, 3.0f, 8.0f}) {
        f.advance(frame);
        replayed.advance(frame);
        CHECK(replayed.move == f.move);
        CHECK(replayed.meter == f.meter);
        CHECK(replayed.calls == f.calls);
    }
    CHECK(replayed.meter.held() == 60);
    CHECK(replayed.calls == std::vector<std::string>{"dim", "dim", "dim", "strike1"});
}

TEST_CASE("turbo volleys resume the next shot after a rewind and preserve unpaid harmless rows",
          "[game][players][turbo-move][rollback][turbo-checkpoint]") {
    Fixture f;
    MoveStrike volley;
    volley.type = MoveStrike::kVolley;
    volley.startFrame = 0;
    volley.endFrame = 12;
    volley.delay = 2;
    volley.angle = 1;
    volley.flags = MoveStrike::kSweepsIn;
    f.stats.moveStrikes = {volley};
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(4);
    REQUIRE(f.shots.size() == 3);
    REQUIRE(f.move.owed() == 40);
    // The player's simulation holds its move and meter by value: copies are its rewind.
    Fixture replayed;
    replayed.stats = f.stats;
    replayed.move = f.move;
    replayed.meter = f.meter;
    CHECK(replayed.shots.empty());
    f.calls.clear();
    f.shots.clear();
    for (const f32 frame : {4.0f, 5.0f, 6.0f, 10.0f, 12.0f}) {
        f.advance(frame);
        replayed.advance(frame);
        CHECK(replayed.shots == f.shots);
        CHECK(replayed.calls == f.calls);
        CHECK(replayed.move == f.move);
        CHECK(replayed.meter == f.meter);
    }
    CHECK(replayed.shots.size() == 3);
    CHECK(replayed.meter.held() == 100);
}

TEST_CASE("a shove replaces prior turbo rows without spending or replaying effects",
          "[game][players][turbo-move][rollback][shove-prediction]") {
    Fixture f;
    f.move.begin(Action::TurboStrong, &f.stats, f.meter);
    f.advance(1);
    REQUIRE(f.move.owed() == TurboMeter::kStrongCost);
    REQUIRE(f.move.weaponHidden());
    f.calls.clear();
    REQUIRE(f.move.begin(Action::Shove, &f.stats, f.meter).empty());
    CHECK(f.move == TurboMove{});
    CHECK(f.meter.held() == 100);
    CHECK(f.calls.empty());
    CHECK(f.shots.empty());

    // The player's simulation holds its move by value: a copy is its rewind.
    Fixture replayed;
    replayed.move = f.move;
    for (const f32 frame : {0.0f, 1.0f, 6.0f, 20.0f}) {
        f.advance(frame, Action::Shove);
        replayed.advance(frame, Action::Shove);
        CHECK(f.move == TurboMove{});
        CHECK(replayed.move == TurboMove{});
    }
    // PlayerAttacks owns the time-based drain. Restoring or stepping the rowless
    // move must not charge a lump cost, emit strikes, or restore old effects.
    CHECK(f.meter.held() == 100);
    CHECK(replayed.meter.held() == 100);
    CHECK(f.calls.empty());
    CHECK(replayed.calls.empty());
    CHECK(replayed.shots.empty());
    replayed.advance(0, Action::DefendRaise);
    CHECK(replayed.move == TurboMove{});
    CHECK(PlayerAnimator::rollbackShoveAction(Action::Shove));
    CHECK_FALSE(PlayerAnimator::rollbackTurboAction(Action::Shove));
    CHECK_FALSE(PlayerAnimator::rollbackShoveAction(Action::TurboStrong));
    CHECK_FALSE(PlayerAnimator::rollbackShoveAction(Action::TurboFull));
}

TEST_CASE("native B and C turbo rows resume every rewind boundary with identical events",
          "[game][players][turbo-move][rollback][turbo-checkpoint][assets]") {
    const s32 character = GENERATE(0, 1, 3, 6);
    const Action action = GENERATE(Action::TurboStrong, Action::TurboFull);
    ClassDataSet classes;
    const auto classPath = test::assetOrSkip("PDATA/WAR.WAD").parent_path();
    REQUIRE(classes.load(classPath));
    const ClassStats* stats = classes.stats(character);
    REQUIRE(stats);
    Fixture original;
    original.stats = *stats;
    original.move.begin(action, stats, original.meter);
    // A move with rows owes its cost until its first damaging strike; one without pays at once.
    REQUIRE(original.move.owed() != 0);
    for (s32 halfFrame = 0; halfFrame < 240; ++halfFrame) {
        CAPTURE(character, action, halfFrame);
        // The player's simulation holds its move and meter by value: copies are its rewind.
        Fixture replayed;
        replayed.stats = *stats;
        replayed.move = original.move;
        replayed.meter = original.meter;
        CHECK(replayed.calls.empty());
        CHECK(replayed.shots.empty());
        original.calls.clear();
        original.shots.clear();
        const f32 frame = static_cast<f32>(halfFrame) / 2;
        original.advance(frame, action);
        replayed.advance(frame, action);
        CHECK(replayed.calls == original.calls);
        CHECK(replayed.shots == original.shots);
        CHECK(replayed.move == original.move);
        CHECK(replayed.meter == original.meter);
    }
}

} // namespace
