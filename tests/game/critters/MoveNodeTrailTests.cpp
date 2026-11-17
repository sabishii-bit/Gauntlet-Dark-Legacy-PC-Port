#include <array>
#include <initializer_list>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"

#include "game/critters/MoveNodeTrail.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kFrame = 1.0f / 30.0f;
constexpr f32 kTick = 1.0f / 60.0f;

/** Steps a body's trail `seconds` on with its node at `world`, giving the travel. */
Vec3 stepBody(MoveNodeTrail& trail, f32 seconds, const Vec3& world) {
    trail.step(seconds, world, Vec3{0.0f}, Mat4{1.0f}, false);
    return trail.travel();
}

NativeStateDigest::Digest digestOf(const MoveNodeTrail& trail) {
    NativeStateDigest digest;
    digest.value(trail);
    const auto result = digest.checkedFinish();
    REQUIRE(result);
    return *result;
}

TEST_CASE("a move node's trail gives the travel of its last native frame",
          "[combatant][blow-push]") {
    MoveNodeTrail trail;
    CHECK(stepBody(trail, kFrame, Vec3{0.0f}) == Vec3{0.0f});
    CHECK(stepBody(trail, kFrame, Vec3{1.0f, 0.0f, 0.0f}) == Vec3{1.0f, 0.0f, 0.0f});
    CHECK(stepBody(trail, kFrame, Vec3{3.0f, 0.0f, 0.0f}) == Vec3{2.0f, 0.0f, 0.0f});
}

TEST_CASE("at sixty updates a second the trail measures two ticks back, whichever tick it "
          "starts on",
          "[combatant][blow-push]") {
    const s32 lead = GENERATE(0, 1);
    CAPTURE(lead);
    MoveNodeTrail trail;
    for (s32 i = 0; i < lead; ++i) {
        stepBody(trail, kTick, Vec3{0.0f});
    }
    const std::array path{Vec3{0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{2.0f, 0.0f, 0.0f},
                          Vec3{2.0f, 0.0f, 5.0f}, Vec3{2.0f, 0.0f, 6.0f}};
    std::vector<Vec3> travel;
    travel.reserve(path.size());
    for (const Vec3& at : path) {
        travel.push_back(stepBody(trail, kTick, at));
    }
    // Nothing a native frame old to measure from at first, unless the lead left a sample.
    CHECK(travel[0] == Vec3{0.0f});
    CHECK(travel[1] == (lead == 0 ? Vec3{0.0f} : Vec3{1.0f, 0.0f, 0.0f}));
    // From then on each tick measures the two before it, not a frame clocked from the start.
    CHECK(travel[2] == Vec3{2.0f, 0.0f, 0.0f});
    CHECK(travel[3] == Vec3{1.0f, 0.0f, 5.0f});
    CHECK(travel[4] == Vec3{0.0f, 0.0f, 6.0f});
}

TEST_CASE("an update of no time takes the newest sample again without ageing the trail",
          "[combatant][blow-push]") {
    MoveNodeTrail trail;
    stepBody(trail, kTick, Vec3{0.0f});
    stepBody(trail, kTick, Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(stepBody(trail, kTick, Vec3{2.0f, 0.0f, 0.0f}) == Vec3{2.0f, 0.0f, 0.0f});
    trail.step(0.0f, Vec3{2.0f, 0.0f, 7.0f}, Vec3{0.0f}, Mat4{1.0f}, false);
    CHECK(trail.travel() == Vec3{2.0f, 0.0f, 0.0f});
    // Two ticks on from (1, 0, 0), as though the update of no time had not come between,
    CHECK(stepBody(trail, kTick, Vec3{3.0f, 0.0f, 0.0f}) == Vec3{2.0f, 0.0f, 0.0f});
    // and then from where the node stood as it came.
    CHECK(stepBody(trail, kTick, Vec3{4.0f, 0.0f, 0.0f}) == Vec3{2.0f, 0.0f, -7.0f});

    // An empty trail takes its first sample from an update of no time too.
    MoveNodeTrail fresh;
    fresh.step(0.0f, Vec3{5.0f, 0.0f, 0.0f}, Vec3{0.0f}, Mat4{1.0f}, false);
    CHECK(fresh.travel() == Vec3{0.0f});
    CHECK(stepBody(fresh, kFrame, Vec3{6.0f, 0.0f, 0.0f}) == Vec3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("a clock of a quarter native frame still measures a whole one, and a faster one as "
          "much of one as the trail holds",
          "[combatant][blow-push]") {
    const auto travels = [](f32 seconds) {
        MoveNodeTrail trail;
        constexpr s32 kUpdates = 12;
        std::vector<Vec3> out;
        out.reserve(static_cast<usize>(kUpdates));
        for (s32 i = 0; i < kUpdates; ++i) {
            out.push_back(stepBody(trail, seconds, Vec3{static_cast<f32>(i), 0.0f, 0.0f}));
        }
        return out;
    };
    // At 120 a second a native frame is four updates.
    const auto quarter = travels(1.0f / 120.0f);
    // At 240 the trail's four samples span half of one.
    const auto eighth = travels(1.0f / 240.0f);
    for (usize i = 0; i < quarter.size(); ++i) {
        CAPTURE(i);
        const Vec3 expected = i < MoveNodeTrail::kSamples ? Vec3{0.0f} : Vec3{4.0f, 0.0f, 0.0f};
        CHECK(quarter[i] == expected);
        CHECK(eighth[i] == expected);
    }
}

TEST_CASE("a part measures its node's travel by its body as it now stands",
          "[combatant][blow-push]") {
    // Between the two samples the body steps five along x and turns a quarter, while the node
    // moves a unit along the model's x.
    const Mat4 before{1.0f};
    const Mat4 after = portable::rotate(glm::translate(Mat4{1.0f}, Vec3{5.0f, 0.0f, 0.0f}), kHalfPi,
                                        Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 from{2.0f, 0.0f, 0.0f};
    const Vec3 to{3.0f, 0.0f, 0.0f};
    const Vec3 worldFrom{before * Vec4{from, 1.0f}};
    const Vec3 worldTo{after * Vec4{to, 1.0f}};
    MoveNodeTrail part;
    part.step(kFrame, worldFrom, from, before, true);
    part.step(kFrame, worldTo, to, after, true);
    CHECK(glm::distance(part.travel(), Vec3{after * Vec4{1.0f, 0.0f, 0.0f, 0.0f}}) < 1e-6f);
    CHECK(glm::length(part.travel()) == Approx(1.0f));
    // A body's own step and turn count in its travel.
    MoveNodeTrail body;
    body.step(kFrame, worldFrom, from, before, false);
    body.step(kFrame, worldTo, to, after, false);
    CHECK(body.travel() == worldTo - worldFrom);
    CHECK(body.travel().x == Approx(3.0f));
}

TEST_CASE("a node blow pushes twice the sum of the node's swing and the way from the body, each "
          "by the fast length",
          "[combatant][blow-push]") {
    MoveNodeTrail trail;
    stepBody(trail, kFrame, Vec3{0.0f});
    REQUIRE(stepBody(trail, kFrame, Vec3{1.0f, 0.0f, 0.0f}) == Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 push = trail.blowPush(Vec3{0.0f, 4.0f, 0.0f}, Vec3{3.0f, 0.0f, 0.0f});
    // The body's height leaves the way from it a unit high (CritterNodePlayerCollide).
    CHECK(push == 2.0f * (approximateUnit(Vec3{1.0f, 0.0f, 0.0f}) +
                          approximateUnit(Vec3{3.0f, 1.0f, 0.0f})));
    CHECK(push.x == Approx(3.8862f).margin(1e-4));
    CHECK(push.y == Approx(0.6287f).margin(1e-4));
    CHECK(push.z == 0.0f);
}

TEST_CASE("a still node pushes away lifted by a unit, and a node falling or drawing back changes "
          "the push",
          "[combatant][blow-push]") {
    const Vec3 body{0.0f, 4.0f, 0.0f};
    const Vec3 player{0.0f, 0.0f, 4.5f};
    const auto pushAfter = [&](const Vec3& travel, const Vec3& at) {
        MoveNodeTrail trail;
        stepBody(trail, kFrame, Vec3{0.0f});
        REQUIRE(stepBody(trail, kFrame, travel) == travel);
        return trail.blowPush(body, at);
    };
    CHECK(pushAfter(Vec3{0.0f}, player) == 2.0f * approximateUnit(Vec3{0.0f, 1.0f, 4.5f}));
    CHECK(pushAfter(Vec3{0.0f, -1.0f, 0.0f}, player).y < 0.0f);
    // Drawn back toward the body, the swing all but cancels the way out: a pull.
    const Vec3 drawn = pushAfter(Vec3{0.0f, 0.0f, -1.0f}, player);
    CHECK(drawn.z < 0.0f);
    CHECK(glm::length(Vec2{drawn.x, drawn.z}) < 0.1f);
    // Under the body's origin the way from it points straight up.
    CHECK(pushAfter(Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f}) == Vec3{0.0f, 2.0f, 2.0f});
}

TEST_CASE("a move node trail compares and digests every field",
          "[combatant][blow-push][native-state-digest]") {
    const auto differ = [](const MoveNodeTrail& a, const MoveNodeTrail& b) {
        CHECK_FALSE(a == b);
        CHECK(digestOf(a) != digestOf(b));
    };
    const auto bodyAt = [](std::initializer_list<std::pair<f32, Vec3>> steps) {
        MoveNodeTrail trail;
        for (const auto& [seconds, world] : steps) {
            trail.step(seconds, world, Vec3{0.0f}, Mat4{1.0f}, false);
        }
        return trail;
    };
    const auto same = bodyAt({{kFrame, Vec3{1.0f, 0.0f, 0.0f}}});
    CHECK(same == bodyAt({{kFrame, Vec3{1.0f, 0.0f, 0.0f}}}));
    CHECK(digestOf(same) == digestOf(bodyAt({{kFrame, Vec3{1.0f, 0.0f, 0.0f}}})));
    SECTION("a sample's world origin") {
        differ(same, bodyAt({{kFrame, Vec3{2.0f, 0.0f, 0.0f}}}));
    }
    SECTION("a sample's origin in the model") {
        MoveNodeTrail other;
        other.step(kFrame, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, Mat4{1.0f}, false);
        differ(same, other);
    }
    SECTION("a sample's age") {
        differ(bodyAt({{kTick, Vec3{0.0f}}, {kTick, Vec3{0.0f}}}),
               bodyAt({{kTick / 2.0f, Vec3{0.0f}}, {kTick / 2.0f, Vec3{0.0f}}}));
    }
    SECTION("how many samples it holds") {
        // A sample at the origin taken at once holds only default values.
        differ(MoveNodeTrail{}, bodyAt({{kFrame, Vec3{0.0f}}}));
    }
    SECTION("the travel") {
        // The same samples, the second turned by a body turned another way.
        const auto turned = [](const Mat4& model) {
            MoveNodeTrail trail;
            trail.step(kFrame, Vec3{0.0f}, Vec3{0.0f}, Mat4{1.0f}, true);
            trail.step(kFrame, Vec3{0.0f}, Vec3{1.0f, 0.0f, 0.0f}, model, true);
            return trail;
        };
        differ(turned(Mat4{1.0f}),
               turned(portable::rotate(Mat4{1.0f}, kHalfPi, Vec3{0.0f, 1.0f, 0.0f})));
    }
}

} // namespace
