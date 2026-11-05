#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/widgets/PointerTargets.h"

namespace {

using namespace gdl;
using gdl::game::pointerMoved;

enum class Action : u8 { Buy, Sell, Continue };

/** A shop-like control: whose lane it is in, what it does, and the catalog row it names. */
struct Control {
    s32 seat = 0;
    Action action = Action::Continue;
    usize row = 0;
    bool operator==(const Control&) const = default;
};

using Targets = gdl::game::PointerTargets<Control>;

/** The second player's lane of the shop, the bounds its controls are cut to. */
constexpr Rect kLane{128.0f, 0.0f, 128.0f, 320.0f};

TEST_CASE("the pointer target registered last is on top where two overlap", "[game][menu][mouse]") {
    Targets targets;
    targets.add(Rect{0.0f, 0.0f, 100.0f, 50.0f}, {0, Action::Buy, 1});
    targets.add(Rect{50.0f, 0.0f, 100.0f, 50.0f}, {0, Action::Continue, 0});

    const auto* overlap = targets.at(Vec2{75.0f, 25.0f});
    REQUIRE(overlap != nullptr);
    CHECK(overlap->control == Control{0, Action::Continue, 0});
    const auto* lower = targets.at(Vec2{25.0f, 25.0f});
    REQUIRE(lower != nullptr);
    CHECK(lower->control == Control{0, Action::Buy, 1});
    CHECK(targets.at(Vec2{150.0f, 25.0f}) == nullptr);
    CHECK(targets.at(Vec2{75.0f, 50.0f}) == nullptr);
}

TEST_CASE("a Sell inside its Buy row wins", "[game][menu][mouse]") {
    Targets targets;
    targets.add(Rect{136.0f, 40.0f, 112.0f, 12.0f}, {1, Action::Buy, 3}, kLane);
    targets.add(Rect{216.0f, 40.0f, 24.0f, 12.0f}, {1, Action::Sell, 3}, kLane);

    const auto* sell = targets.at(Vec2{220.0f, 45.0f});
    REQUIRE(sell != nullptr);
    CHECK(sell->control == Control{1, Action::Sell, 3});
    const auto* buy = targets.at(Vec2{140.0f, 45.0f});
    REQUIRE(buy != nullptr);
    CHECK(buy->control == Control{1, Action::Buy, 3});
}

TEST_CASE("a pointer target is cut to its bounds and a cut with nothing left shows nothing",
          "[game][menu][mouse]") {
    Targets targets;
    targets.add(Rect{120.0f, -10.0f, 40.0f, 30.0f}, {1, Action::Continue, 0}, kLane);
    REQUIRE(targets.targets().size() == 1);
    CHECK(targets.targets()[0].area == Rect{128.0f, 0.0f, 32.0f, 20.0f});
    CHECK(targets.at(Vec2{124.0f, 5.0f}) == nullptr);
    CHECK(targets.at(Vec2{130.0f, -5.0f}) == nullptr);
    CHECK(targets.at(Vec2{130.0f, 5.0f}) != nullptr);

    targets.add(Rect{0.0f, 0.0f, 100.0f, 20.0f}, {1, Action::Buy, 0}, kLane);
    targets.add(Rect{100.0f, 0.0f, 28.0f, 20.0f}, {1, Action::Buy, 1}, kLane);
    targets.add(Rect{136.0f, 320.0f, 40.0f, 12.0f}, {1, Action::Buy, 2}, kLane);
    CHECK(targets.targets().size() == 1);

    targets.add(Rect{0.0f, 0.0f, 100.0f, 20.0f}, {0, Action::Continue, 0});
    REQUIRE(targets.targets().size() == 2);
    CHECK(targets.targets()[1].area == Rect{0.0f, 0.0f, 100.0f, 20.0f});
}

TEST_CASE("a pointer target its predicate refuses passes the pointer to the one below",
          "[game][menu][mouse]") {
    Targets targets;
    targets.add(Rect{0.0f, 0.0f, 128.0f, 320.0f}, {0, Action::Continue, 0});
    targets.add(Rect{0.0f, 0.0f, 128.0f, 320.0f}, {1, Action::Continue, 0});
    const auto seat = [](s32 wanted) {
        return [wanted](const Targets::Target& target) { return target.control.seat == wanted; };
    };

    const auto* first = targets.at(Vec2{10.0f, 10.0f}, seat(0));
    REQUIRE(first != nullptr);
    CHECK(first->control.seat == 0);
    const auto* second = targets.at(Vec2{10.0f, 10.0f}, seat(1));
    REQUIRE(second != nullptr);
    CHECK(second->control.seat == 1);
    CHECK(targets.at(Vec2{10.0f, 10.0f}, seat(2)) == nullptr);
    CHECK(targets.at(Vec2{10.0f, 10.0f}) == second);
}

TEST_CASE("clearing pointer targets forgets the controls shown before", "[game][menu][mouse]") {
    Targets targets;
    targets.add(Rect{0.0f, 0.0f, 100.0f, 20.0f}, {0, Action::Buy, 4});
    targets.clear();
    CHECK(targets.targets().empty());
    CHECK(targets.at(Vec2{10.0f, 10.0f}) == nullptr);

    targets.add(Rect{0.0f, 0.0f, 100.0f, 20.0f}, {0, Action::Sell, 5});
    REQUIRE(targets.targets().size() == 1);
    const auto* shown = targets.at(Vec2{10.0f, 10.0f});
    REQUIRE(shown != nullptr);
    CHECK(shown->control == Control{0, Action::Sell, 5});
}

TEST_CASE("a pointer has moved once it appears or goes past the threshold", "[game][menu][mouse]") {
    constexpr f32 kThreshold = 2.0f;
    const Vec2 origin{10.0f, 10.0f};
    CHECK(pointerMoved(std::nullopt, origin, kThreshold));
    CHECK_FALSE(pointerMoved(origin, origin, kThreshold));
    CHECK_FALSE(pointerMoved(origin, Vec2{11.0f, 11.0f}, kThreshold));
    CHECK_FALSE(pointerMoved(origin, Vec2{12.0f, 10.0f}, kThreshold));
    CHECK(pointerMoved(origin, Vec2{12.5f, 10.0f}, kThreshold));
    CHECK(pointerMoved(origin, Vec2{8.0f, 8.0f}, kThreshold));
    CHECK(pointerMoved(std::nullopt, origin, 0.0f));
    CHECK_FALSE(pointerMoved(origin, origin, 0.0f));
}

} // namespace
