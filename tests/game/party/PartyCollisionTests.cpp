#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/party/PartyCollision.h"
#include "game/players/ExitWait.h"
#include "game/players/Knockback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::array<PlayerRuntime, 3> party() {
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 10}, 0);
    players[2].actor.spawn(2, {}, nullptr, Vec3{0, 0, -10}, 0);
    return players;
}

TEST_CASE("a step into another member ends pushed out to their whole widths' sum",
          "[game][screens][party-collision]") {
    auto players = party();
    const f32 reach = players[0].actor.reach() + players[1].actor.reach();
    // Straight at them, it stops short of them.
    Vec3 to{0, 0, 9.5f};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0, 0, 7}, to) == std::optional<usize>{1});
    CHECK(to.z == Approx(10.0f - reach));
    CHECK(to.x == Approx(0.0f));
    // Glancing, it is pushed aside around them.
    to = Vec3{0.5f, 0, 9.0f};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0.5f, 0, 7}, to).has_value());
    CHECK(glm::distance(Vec2{to.x, to.z}, Vec2{0, 10}) == Approx(reach));
    CHECK(to.x > 0.5f);
    // What is out of reach of the step, or behind it, or not standing, is let be.
    to = Vec3{0, 0, 2};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, 0}, to).has_value());
    const f32 near = -10.0f + reach * 0.5f;
    to = Vec3{0, 0, near + 0.1f};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to).has_value());
    to = Vec3{0, 0, near - 0.1f};
    CHECK(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to) == std::optional<usize>{2});
    players[2].life = PlayerLife::InTower;
    to = Vec3{0, 0, near - 0.1f};
    CHECK_FALSE(PartyCollision::resolve(players, 0, Vec3{0, 0, near}, to).has_value());
    // A step ending on top of another is undone.
    to = Vec3{0, 0, 10};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0, 0, 8}, to).has_value());
    CHECK(to == Vec3{0, 0, 8});
}

TEST_CASE("the one run into is shoved by the step as it was meant",
          "[game][screens][party-collision]") {
    auto players = party();
    players[0].actor.place(Vec3{0, 0, 9.5f});
    REQUIRE(PartyCollision::step(players, 0, Vec3{0, 0, 9}, Vec3{0, 0, 9.5f}, 1.0f / 30.0f) ==
            std::optional<usize>{1});
    CHECK(players[0].actor.position().z < 9.0f);
    players[1].knockback.endFrame();
    CHECK(players[1].knockback.pushed());
    CHECK_FALSE(players[2].knockback.pushed());
    // The shove carries it on a little, the way it was pushed.
    const Vec3 moved = players[1].knockback.step(1.0f / 30.0f, 10.0f);
    CHECK(moved.z > 0.0f);
    CHECK(moved.x == 0.0f);
}

TEST_CASE("the step as meant meets the member, and the mover is put out from where it stands",
          "[game][screens][party-collision]") {
    auto players = party();
    const f32 reach = players[0].actor.reach() + players[1].actor.reach();
    // Something held the body where it stood, at 8, but the step it meant ran on to 9.5, into
    // the member at 10: the member is met and shoved by that step, and the body goes to the
    // point out of the member, as the original's walls see the step only once it is put out.
    players[0].actor.place(Vec3{0, 0, 8});
    Knockback shoved;
    shoved.shove(Vec3{0, 0, 1.5f}, 1.0f / 30.0f);
    REQUIRE(PartyCollision::step(players, 0, Vec3{0, 0, 8}, Vec3{0, 0, 9.5f}, 1.0f / 30.0f) ==
            std::optional<usize>{1});
    CHECK(players[1].knockback == shoved);
    CHECK(players[0].actor.position().z == Approx(10.0f - reach));
    // One standing on an exit for the others is not shoved, but still holds the step out.
    auto waiting = party();
    REQUIRE_FALSE(waiting[1].exitWait.step(true, 2));
    waiting[0].actor.place(Vec3{0, 0, 9.5f});
    REQUIRE(PartyCollision::step(waiting, 0, Vec3{0, 0, 9}, Vec3{0, 0, 9.5f}, 1.0f / 30.0f) ==
            std::optional<usize>{1});
    CHECK(waiting[1].knockback == Knockback{});
    CHECK(waiting[0].actor.position().z == Approx(10.0f - reach));
}

TEST_CASE("a step a wall cuts short is put out of a member no further back than the member asks",
          "[game][screens][party-collision]") {
    // A flat floor, a wall across the way at z = 8.8 and a member past it, at 10.2.
    std::vector<CollisionTriangle> triangles;
    const auto add = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
        CollisionTriangle triangle;
        triangle.vertices = {a, b, c};
        triangle.normal = normal;
        triangles.push_back(triangle);
    };
    add(Vec3{-50, 0, -50}, Vec3{50, 0, -50}, Vec3{50, 0, 50}, Vec3{0, 1, 0});
    add(Vec3{-50, 0, -50}, Vec3{50, 0, 50}, Vec3{-50, 0, 50}, Vec3{0, 1, 0});
    add(Vec3{-50, 0, 8.8f}, Vec3{50, 0, 8.8f}, Vec3{50, 20, 8.8f}, Vec3{0, 0, -1});
    add(Vec3{-50, 0, 8.8f}, Vec3{50, 20, 8.8f}, Vec3{-50, 20, 8.8f}, Vec3{0, 0, -1});
    WorldCollision collision;
    collision.build(triangles);
    auto players = party();
    players[1].actor.place(Vec3{0, 0, 10.2f});
    const f32 reach = players[0].actor.reach() + players[1].actor.reach();
    // The step meant from 7 to 7.6 is held by the wall where the body's width meets it.
    const Vec3 from{0, 0, 7};
    const Vec3 meant{0, 0, 7.6f};
    players[0].actor.place(from);
    players[0].actor.slide(meant - from, &collision);
    const f32 walled = players[0].actor.position().z;
    REQUIRE(walled < meant.z - 0.2f);
    REQUIRE(walled > 10.2f - reach);
    // The member asks the step's end back to 10.2 - 3 = 7.2: the body goes there, not as far
    // again back as the wall had already taken off.
    REQUIRE(PartyCollision::step(players, 0, from, meant, 1.0f / 30.0f, &collision) ==
            std::optional<usize>{1});
    CHECK(players[0].actor.position().z == Approx(10.2f - reach).margin(1e-4));
    CHECK(players[0].actor.position().x == Approx(0.0f).margin(1e-4));
}

TEST_CASE("members keep a whole width apart, meeting only within half a height of their middles",
          "[game][party-collision][party-spacing]") {
    // PlayerCollidePlayers meets a member within the mover's col_radius and theirs, the classes'
    // whole widths, and within the mover's col_height, half its height, of their collision
    // centre, and puts the step out to the widths (LineCylinderCollide; pmotion.c 4062-4097,
    // 1155-1157; player.c 4028-4029).
    auto players = party();
    Vec3 to{0, 0, 9.5f};
    REQUIRE(PartyCollision::resolve(players, 0, Vec3{0, 0, 7}, to) == std::optional<usize>{1});
    CHECK(to.z == Approx(7.0f));
    CHECK(to.x == 0.0f);
    // On a ledge three above or four below, a member is passed under or over; two and a half
    // above or two below, it is met.
    struct Ledge {
        f32 height = 0;
        bool meets = false;
    };
    for (const Ledge ledge :
         {Ledge{3.0f, false}, Ledge{-4.0f, false}, Ledge{2.5f, true}, Ledge{-2.0f, true}}) {
        CAPTURE(ledge.height);
        players[1].actor.place(Vec3{0, ledge.height, 10});
        to = Vec3{0, 0, 9.5f};
        CHECK(PartyCollision::resolve(players, 0, Vec3{0, 0, 7}, to).has_value() == ledge.meets);
    }
}

/** A 30 Hz frame. */
constexpr f32 kFrame = 1.0f / 30.0f;

TEST_CASE("the blocking pass trims a step toward nine tenths of the mover's width and shoves "
          "nobody",
          "[game][party-collision][party-spacing]") {
    // The second PlayerCollidePlayers, after the walls, the floor and the items, meets at 0.9 of
    // the mover's col_radius (0x80347BF0) and answers through PlayerCollideWalls' per-axis trim
    // (pmotion.c 1320-1353, 3262-3302).
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 3.1f}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    const f32 held =
        PartyCollision::kBlockShare * players[0].actor.reach() + players[1].actor.reach();
    REQUIRE(held == Approx(2.85f));
    // A step in to 2.7 gives up what lies inside, and nothing is left to push.
    const Vec3 from{0, 0, 3.1f};
    Vec3 to{0, 0, 2.7f};
    CHECK(PartyCollision::block(players, 0, from, to, kFrame) == Vec3{0});
    CHECK(to.x == 0.0f);
    CHECK(to.z == Approx(held));
    CHECK(players[1].knockback == Knockback{});
    // One ending outside it goes on.
    to = Vec3{0, 0, 2.9f};
    CHECK(PartyCollision::block(players, 0, from, to, kFrame) == Vec3{0});
    CHECK(to == Vec3{0, 0, 2.9f});
    // Standing inside it, the body stays where it is, and the way out is pushed over the frame.
    to = Vec3{0, 0, 2.5f};
    const Vec3 push = PartyCollision::block(players, 0, to, to, kFrame);
    CHECK(to == Vec3{0, 0, 2.5f});
    CHECK(push.x == 0.0f);
    CHECK(push.z == Approx((held - 2.5f) / kFrame));
    // The first pass at the same share puts the step's end out to it.
    to = Vec3{0, 0, 2.7f};
    REQUIRE(PartyCollision::resolve(players, 0, from, to, PartyCollision::kBlockShare) ==
            std::optional<usize>{1});
    CHECK(to.z == Approx(held));
}

TEST_CASE("the blocking pass takes the first member in seat order it meets, not the nearest",
          "[game][party-collision][party-spacing]") {
    // The second PlayerCollidePlayers stops at the first member it meets (stopFirst,
    // pmotion.c 4072-4078, 1336-1337).
    std::array<PlayerRuntime, 3> players;
    const Vec3 from{0, 0, 3.1f};
    players[0].actor.spawn(0, {}, nullptr, from, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{2.8f, 0, 3.0f}, 0); // beside the step
    players[2].actor.spawn(2, {}, nullptr, Vec3{0}, 0);             // ahead, nearer its end
    Vec3 to{0, 0, 2.7f};
    CHECK(PartyCollision::resolve(players, 0, from, to, PartyCollision::kBlockShare) ==
          std::optional<usize>{2});
    // Put out sideways and back along the step by the one beside it, the step goes on neither
    // axis and that way out is pushed.
    to = Vec3{0, 0, 2.7f};
    const Vec3 push = PartyCollision::block(players, 0, from, to, kFrame);
    CHECK(to == from);
    CHECK(push.x < 0.0f);
    CHECK(push.z < 0.0f);
}

} // namespace
