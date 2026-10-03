#include <array>
#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/ComboMove.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Attach = ComboOrders::Attach;

constexpr s32 kTicks = 2;

/** A pair tied together, the grabber of `grabberClass`. */
struct Pair {
    ComboState grabber;
    ComboState partner;
    explicit Pair(s32 grabberClass) { ComboMove::link(grabber, 0, partner, 1, grabberClass); }
    ComboOrders act1(f32 frame) {
        return ComboMove::advance(grabber, partner, ComboPhase{true, false, frame}, kTicks);
    }
    ComboOrders act2(f32 frame) {
        return ComboMove::advance(grabber, partner, ComboPhase{false, true, frame}, kTicks);
    }
    ComboOrders done() {
        return ComboMove::advance(grabber, partner, ComboPhase{false, false, 0.0f}, kTicks);
    }
};

TEST_CASE("a partner is the nearest standing ahead within five and three up or down",
          "[game][players][combo]") {
    const Vec3 facing{0.0f, 0.0f, 1.0f};
    std::array<ComboMove::Candidate, 4> others{{
        {Vec3{0.0f, 0.0f, 4.0f}, true}, // ahead, in reach
        {Vec3{0.0f, 0.0f, 2.0f}, true}, // nearer
        {Vec3{0.0f, 0.0f, 6.0f}, true}, // too far
        {Vec3{3.0f, 0.0f, 0.5f}, true}, // aside, out of the cone
    }};
    REQUIRE(ComboMove::findPartner(1, Vec3{0.0f}, facing, others) == 0);
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, facing, others) == 1);
    others[1].eligible = false;
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, facing, others) == std::nullopt);
    others[1].eligible = true;
    others[1].position.y = 3.5f; // too high
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, facing, others) == std::nullopt);
    // Just inside the cone counts; a stride over the reach does not.
    others[3].position = Vec3{2.0f, 0.0f, 2.1f};
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, facing, others) == 3);
    others[3].position = Vec3{0.0f, 0.0f, 5.01f};
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, facing, others) == std::nullopt);
    // Nobody with no facing to look along.
    REQUIRE(ComboMove::findPartner(0, Vec3{0.0f}, Vec3{0.0f}, others) == std::nullopt);
}

TEST_CASE("linking ties both sides to each other and the grabber's class",
          "[game][players][combo]") {
    const Pair pair(ComboMove::kSorceress);
    REQUIRE(pair.grabber.role == ComboRole::Grabber);
    REQUIRE(pair.grabber.partner == 1);
    REQUIRE(pair.partner.role == ComboRole::Held);
    REQUIRE(pair.partner.partner == 0);
    REQUIRE(pair.partner.grabberClass == ComboMove::kSorceress);
    REQUIRE(pair.partner.active());
    REQUIRE_FALSE(ComboMove::flies(pair.partner));
    // Nothing happens to a pair that is not tied.
    ComboState loose;
    ComboState other;
    const ComboOrders none =
        ComboMove::advance(loose, other, ComboPhase{true, false, 0.0f}, kTicks);
    REQUIRE(none.attach == Attach::None);
    REQUIRE_FALSE(none.unlink);
}

TEST_CASE("the warrior lifts its partner, lets it fly at frame thirty and lets go after four "
          "seconds",
          "[game][players][combo]") {
    Pair pair(ComboMove::kWarrior);
    ComboOrders orders = pair.act1(0.0f);
    REQUIRE(orders.attach == Attach::PartnerOnGrabber);
    REQUIRE(pair.partner.riding);
    REQUIRE(pair.partner.graceSeconds == Approx(ComboMove::kThrowerGrace));
    orders = pair.act1(15.0f);
    REQUIRE(orders.attach == Attach::None); // already in hand
    REQUIRE(pair.partner.role == ComboRole::Held);
    orders = pair.act1(ComboMove::kThrowFrame);
    REQUIRE(orders.releasePartner);
    REQUIRE_FALSE(pair.partner.riding);
    REQUIRE(pair.partner.role == ComboRole::Thrown);
    REQUIRE(ComboMove::flies(pair.partner));
    REQUIRE(pair.grabber.ticksLeft == ComboMove::kFlightTicks);
    // The flight lasts its 240 ticks whatever the warrior does after.
    s32 ticks = 0;
    while (!pair.done().unlink) {
        ticks += kTicks;
        REQUIRE(ticks < 1000);
    }
    REQUIRE(ticks == ComboMove::kFlightTicks - kTicks);
    REQUIRE_FALSE(pair.grabber.active());
    REQUIRE_FALSE(pair.partner.active());
}

TEST_CASE("a flier turns three eighths round at what it hits, once in ten ticks, and reflects "
          "off walls",
          "[game][players][combo]") {
    ComboState flier;
    REQUIRE(ComboMove::takeTurn(flier));
    REQUIRE_FALSE(ComboMove::takeTurn(flier));
    ComboMove::tick(flier, ComboMove::kTurnGap - 1, 0.1f);
    REQUIRE_FALSE(ComboMove::takeTurn(flier));
    ComboMove::tick(flier, 1, 0.1f);
    REQUIRE(ComboMove::takeTurn(flier));
    REQUIRE(ComboMove::bounceYaw(0.0f) == Approx(3.0f * std::numbers::pi_v<f32> / 4.0f));
    REQUIRE(ComboMove::bounceYaw(std::numbers::pi_v<f32>) ==
            Approx(-std::numbers::pi_v<f32> / 4.0f));
    // Flying along +z into a wall facing -z comes straight back; a wall to the side turns
    // it across.
    REQUIRE(std::abs(ComboMove::reflectedYaw(0.0f, Vec3{0.0f, 0.0f, -1.0f})) ==
            Approx(std::numbers::pi_v<f32>));
    REQUIRE(ComboMove::reflectedYaw(std::numbers::pi_v<f32> / 4.0f, Vec3{-1.0f, 0.0f, 0.0f}) ==
            Approx(-std::numbers::pi_v<f32> / 4.0f));
    // No wall to speak of: the plain turn.
    REQUIRE(ComboMove::reflectedYaw(0.0f, Vec3{0.0f}) == Approx(ComboMove::bounceYaw(0.0f)));
    flier.graceSeconds = 1.0f;
    ComboMove::tick(flier, 0, 0.4f);
    REQUIRE(flier.graceSeconds == Approx(0.6f));
    ComboMove::tick(flier, 0, 1.0f);
    REQUIRE(flier.graceSeconds == 0.0f);
}

TEST_CASE("thrower grace starts at attachment and is not renewed while held",
          "[game][players][combo][multiplayer]") {
    for (const s32 family : {ComboMove::kWarrior, ComboMove::kDwarf}) {
        CAPTURE(family);
        Pair pair(family);
        pair.act1(0);
        REQUIRE(pair.partner.graceSeconds == Approx(ComboMove::kThrowerGrace));
        ComboMove::tick(pair.partner, 60, 1.0f);
        pair.act1(29);
        CHECK(pair.partner.graceSeconds == Approx(0.5f));
        pair.act2(0);
        CHECK(pair.partner.role == ComboRole::Thrown);
        CHECK(pair.partner.graceSeconds == Approx(0.5f));
    }
}

TEST_CASE("the dwarf climbs on its partner's back, steers it for four seconds and gets off",
          "[game][players][combo]") {
    Pair pair(ComboMove::kDwarf);
    ComboOrders orders = pair.act1(0.0f);
    REQUIRE(orders.attach == Attach::GrabberOnPartner);
    REQUIRE(orders.turnPartnerAway);
    REQUIRE(pair.grabber.riding);
    REQUIRE(pair.grabber.rideAsked);
    REQUIRE(pair.partner.role == ComboRole::Held);
    REQUIRE(pair.grabber.ticksLeft == ComboMove::kFlightTicks);
    pair.act1(10.0f);
    REQUIRE(pair.grabber.ticksLeft == ComboMove::kFlightTicks); // held up while it climbs on
    orders = pair.act2(0.0f);
    REQUIRE(pair.partner.role == ComboRole::Thrown);
    REQUIRE(ComboMove::ridden(pair.partner));
    REQUIRE_FALSE(ComboMove::flies(pair.partner));
    REQUIRE_FALSE(orders.unlink);
    s32 ticks = kTicks;
    while (true) {
        orders = pair.act2(5.0f);
        ticks += kTicks;
        if (orders.unlink) {
            break;
        }
        REQUIRE(ticks < 1000);
    }
    REQUIRE(ticks == ComboMove::kFlightTicks);
    REQUIRE(orders.detachGrabber);
    REQUIRE_FALSE(pair.grabber.rideAsked);
    REQUIRE_FALSE(pair.grabber.active());
}

TEST_CASE("the valkyrie and the archer are lifted by their partner and set back down after",
          "[game][players][combo]") {
    for (const s32 grabberClass : {ComboMove::kValkyrie, ComboMove::kArcher}) {
        Pair pair(grabberClass);
        ComboOrders orders = pair.act1(0.0f);
        REQUIRE(orders.attach == Attach::GrabberOnPartner);
        REQUIRE(orders.turnPartnerAway);
        REQUIRE(pair.grabber.riding);
        REQUIRE_FALSE(pair.grabber.rideAsked);
        orders = pair.act1(40.0f);
        REQUIRE(orders.attach == Attach::None);
        REQUIRE_FALSE(orders.unlink);
        orders = pair.done();
        REQUIRE(orders.restoreGrabber);
        REQUIRE(orders.unlink);
        REQUIRE_FALSE(pair.grabber.riding);
        REQUIRE_FALSE(pair.partner.active());
    }
}

TEST_CASE("the wizard, knight and sorceress lift their partner for the move; the jester for a "
          "frame",
          "[game][players][combo]") {
    for (const s32 grabberClass : {ComboMove::kWizard, ComboMove::kKnight, ComboMove::kSorceress}) {
        Pair pair(grabberClass);
        ComboOrders orders = pair.act1(0.0f);
        REQUIRE(orders.attach == Attach::PartnerOnGrabber);
        REQUIRE_FALSE(orders.turnPartnerAway);
        REQUIRE(pair.partner.riding);
        orders = pair.act1(50.0f);
        REQUIRE_FALSE(orders.unlink);
        orders = pair.done();
        REQUIRE(orders.restorePartner);
        REQUIRE(orders.unlink);
        REQUIRE_FALSE(pair.partner.active());
    }
    Pair jester(ComboMove::kJester);
    ComboOrders orders = jester.act1(0.0f);
    REQUIRE(orders.attach == Attach::PartnerOnGrabber);
    orders = jester.act1(1.0f);
    REQUIRE(orders.restorePartner);
    REQUIRE(orders.unlink);
    REQUIRE_FALSE(jester.partner.active());
}

TEST_CASE("a class without a combo of its own lets go at once", "[game][players][combo]") {
    Pair pair(8);
    const ComboOrders orders = pair.act1(0.0f);
    REQUIRE(orders.unlink);
    REQUIRE_FALSE(pair.grabber.active());
}

} // namespace
