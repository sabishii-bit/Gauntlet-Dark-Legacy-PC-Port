#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
        .announce = [this](int id) { calls.push_back("help" + std::to_string(id)); },
        .dim = [this](float) { calls.emplace_back("dim"); },
        .volley = [this](const Vec3& direction) { shots.push_back(direction); },
        .strike =
            [this](int row) {
                if (stats.moveStrikes[static_cast<std::size_t>(row)].amount != 0) {
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
    void advance(float frame, Action action = Action::TurboStrong) {
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
} // namespace
