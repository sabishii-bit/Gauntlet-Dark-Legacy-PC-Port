#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/WorldCollision.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                           s32 object = 0) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = object;
    return out;
}

/** A 20x20 floor at y = 0 with a wall along x = 5 facing -x, three units tall. */
std::vector<CollisionTriangle> room() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 west{-1.0f, 0.0f, 0.0f};
    return {
        triangle({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, up),
        triangle({-10, 0, -10}, {10, 0, 10}, {-10, 0, 10}, up),
        triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, west, 1),
        triangle({5, 0, -10}, {5, 3, 10}, {5, 0, 10}, west, 1),
    };
}

TEST_CASE("native collision geometry fingerprints refresh on every geometry binding change",
          "[world][collision][native-immutable-digest]") {
    const auto hash = [](const WorldCollision& collision) {
        NativeStateDigest digest;
        REQUIRE(collision.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    WorldCollision first;
    WorldCollision independent;
    first.build(room());
    independent.build(room());
    const auto original = hash(first);
    CHECK(hash(first) == original); // Warm cache.
    CHECK(hash(independent) == original);
    WorldCollision::LocalSnapshot old;
    first.saveLocal(old);
    for (u32 changed = 0; changed < 4; ++changed) {
        CAPTURE(changed);
        auto triangles = room();
        if (changed == 0) {
            triangles.back().normal.z = 0.125f;
        }
        if (changed == 1) {
            triangles.back().vertices.back().y += 0.125f;
        }
        if (changed == 2) {
            ++triangles.back().object;
        }
        if (changed == 3) {
            triangles.back().objectFlags ^= 4;
        }
        first.build(triangles);
        independent.build(triangles);
        CHECK(hash(first) != original);
        CHECK(hash(first) == hash(independent));
        NativeStateDigest stale;
        CHECK_FALSE(first.appendLocalDigest(stale, old));
    }
    first.build(room());
    CHECK(hash(first) == original);
    const auto added = triangle({-2, 2, -2}, {2, 2, -2}, {0, 2, 2}, {0, 1, 0}, 7);
    first.append(std::array{added});
    auto expanded = room();
    expanded.push_back(added);
    independent.build(expanded);
    CHECK(hash(first) != original);
    CHECK(hash(first) == hash(independent));
    const auto staticGeometry = hash(first);
    first.setMovingObjects(std::array<s32, 1>{7});
    independent.setMovingObjects(std::array<s32, 1>{7});
    CHECK(hash(first) != staticGeometry);
    CHECK(hash(first) == hash(independent));
    WorldCollision::LocalSnapshot anchored;
    first.saveLocal(anchored);
    const auto movingGeometry = hash(first);
    first.setObjectTransform(7, glm::translate(Mat4{1}, Vec3{0, 3, 0}));
    CHECK(hash(first) != movingGeometry);
    NativeStateDigest saved;
    REQUIRE(first.appendLocalDigest(saved, anchored));
    CHECK(saved.finish() == movingGeometry);
    REQUIRE(first.restoreLocal(anchored));
    CHECK(hash(first) == movingGeometry);
    first.clear();
    independent.clear();
    CHECK(hash(first) == hash(independent));
    CHECK(hash(first) != movingGeometry);
}

TEST_CASE("repeated moving collision placement preserves first identity and corrected contact",
          "[world][collision][local-rewind][collision-placement-reuse]") {
    const auto hash = [](const WorldCollision& collision) {
        NativeStateDigest digest;
        REQUIRE(collision.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    WorldCollision collision;
    collision.build(room());
    collision.setMovingObjects(std::array<s32, 2>{0, 1});
    const auto place = [&](u32 tick) {
        const auto transform = glm::translate(Mat4{1}, Vec3{0, static_cast<f32>(tick), 0});
        collision.setObjectTransform(0, transform);
        collision.setObjectTransform(1, transform);
    };
    const auto contacts = [&](u32 tick) {
        const auto height = static_cast<f32>(tick);
        const auto floor = collision.floorAt({0, height + 1, 0}, 1, 2);
        REQUIRE(floor);
        CHECK(floor->object == 0);
        CHECK(floor->y == height);
        const Vec3 centre{4.8f, height, 1};
        CHECK(collision.resolveWalls(centre, 0.5f, height + 0.2f, height + 2.8f) != centre);
    };
    // setMovingObjects must fully place the initial identity before an equal
    // setter can skip rebuilding its triangles and broad-phase bounds.
    contacts(0);
    const auto initial = hash(collision);
    place(0);
    contacts(0);
    CHECK(hash(collision) == initial);
    WorldCollision::LocalSnapshot start;
    collision.saveLocal(start);
    std::array<WorldCollision::LocalSnapshot, 8> reference;
    std::array<NativeStateDigest::Digest, 8> expected{};
    for (usize tick = 0; tick < reference.size(); ++tick) {
        place(static_cast<u32>(tick + 1));
        collision.saveLocal(reference[tick]);
        expected[tick] = hash(collision);
    }
    for (usize distance = 1; distance <= reference.size(); ++distance) {
        CAPTURE(distance);
        REQUIRE(collision.restoreLocal(start));
        for (usize tick = 0; tick < distance; ++tick) {
            place(static_cast<u32>(100 + tick));
        }
        REQUIRE(collision.restoreLocal(start));
        place(0);
        contacts(0);
        for (usize tick = 0; tick < distance; ++tick) {
            place(static_cast<u32>(tick + 1));
            place(static_cast<u32>(tick + 1));
            contacts(static_cast<u32>(tick + 1));
        }
        WorldCollision::LocalSnapshot corrected;
        collision.saveLocal(corrected);
        CHECK(collision.equalLocal(corrected, reference[distance - 1]));
        CHECK(hash(collision) == expected[distance - 1]);
    }
    REQUIRE(collision.restoreLocal(start));
    collision.setSolid(0, false);
    collision.setContactOnly(1, true);
    collision.setFloorExitBlocked(1, true);
    const auto masked = hash(collision);
    place(0);
    CHECK(hash(collision) == masked);
    CHECK_FALSE(collision.floorAt({0, 1, 0}, 1, 2));
    CHECK(collision.contactOnly(1));
    CHECK(collision.floorExitBlocked(1));
    const Vec3 centre{4.8f, 0, 1};
    std::vector<WallContact> wallContacts;
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f, &wallContacts) == centre);
    CHECK_FALSE(wallContacts.empty());
    REQUIRE(collision.restoreLocal(start));

    const auto added = triangle({-2, 5, -2}, {2, 5, -2}, {0, 5, 2}, {0, 1, 0}, 7);
    collision.append(std::array{added});
    place(0); // Appending static geometry leaves existing moving caches intact.
    contacts(0);
    collision.setMovingObjects(std::array<s32, 2>{0, 7});
    collision.setObjectTransform(7, Mat4{1});
    const auto floor = collision.floorAt({0, 6, 0}, 1, 2);
    REQUIRE(floor);
    CHECK(floor->object == 7);
    CHECK(floor->y == 5);
    CHECK_FALSE(collision.canRestoreLocal(start));
}

TEST_CASE("moving floor locks inherit from parents without owning the inherited bit",
          "[world][collision][lift-floor-inheritance]") {
    WorldCollision collision;
    const std::array<s32, 4> parents{-1, 0, 1, -1};
    collision.build(room(), parents);
    collision.setFloorExitBlocked(0, true);
    CHECK(collision.floorExitBlocked(0));
    CHECK(collision.floorExitBlocked(1));
    CHECK(collision.floorExitBlocked(2));
    CHECK_FALSE(collision.ownFloorExitBlocked(1));
    CHECK_FALSE(collision.ownFloorExitBlocked(2));
    CHECK_FALSE(collision.floorExitBlocked(3));
    CHECK_FALSE(collision.floorExitBlocked(-1));
    WorldCollision::LocalSnapshot held;
    collision.saveLocal(held);
    collision.setFloorExitBlocked(0, false);
    CHECK_FALSE(collision.floorExitBlocked(2));
    REQUIRE(collision.restoreLocal(held));
    CHECK(collision.floorExitBlocked(2));
    collision.setFloorExitBlocked(1, true);
    collision.setFloorExitBlocked(0, false);
    CHECK(collision.floorExitBlocked(2)); // the child may independently hold its own riders
    collision.setFloorExitBlocked(1, false);
    CHECK_FALSE(collision.floorExitBlocked(2));
    NativeStateDigest parented;
    REQUIRE(collision.appendNativeDigest(parented));
    collision.build(room());
    NativeStateDigest unparented;
    REQUIRE(collision.appendNativeDigest(unparented));
    CHECK(parented.finish() != unparented.finish());
    collision.setFloorExitBlocked(0, true);
    CHECK_FALSE(collision.floorExitBlocked(1)); // rebuilding discards the previous hierarchy
}

TEST_CASE("a moving lift opened to players lets only their own probes through it and its direct "
          "children",
          "[world][collision][player-pass-through][local-rewind]") {
    // ProcessItemWobjs marks a lift in motion (WorldObj +0x35 = 1) and WorldCollide ORs each
    // object's mark with its direct parent's alone (0x8000D578): the player probes' side mask 1
    // passes the lift and its deck, a creature's mask 2 meets them, and a rail on the deck, two
    // below the marked lift, stays solid to everyone.
    constexpr s32 kLift = 0;
    constexpr s32 kDeck = 1;
    constexpr s32 kRail = 2;
    constexpr s32 kGround = 3;
    constexpr auto kPlayer = WorldCollision::Prober::Player;
    const Vec3 up{0, 1, 0};
    std::vector<CollisionTriangle> triangles;
    for (const auto& [x, object] : std::array{std::pair{0.0f, kLift}, std::pair{10.0f, kDeck},
                                              std::pair{20.0f, kRail}, std::pair{30.0f, kGround}}) {
        triangles.push_back(triangle({x - 2, 0, -2}, {x + 2, 0, -2}, {x + 2, 0, 2}, up, object));
        triangles.push_back(triangle({x - 2, 0, -2}, {x + 2, 0, 2}, {x - 2, 0, 2}, up, object));
    }
    const Vec3 west{-1, 0, 0};
    triangles.push_back(triangle({12, 0, -2}, {12, 3, -2}, {12, 3, 2}, west, kDeck));
    triangles.push_back(triangle({12, 0, -2}, {12, 3, 2}, {12, 0, 2}, west, kDeck));
    WorldCollision collision;
    collision.build(triangles, std::array<s32, 4>{-1, kLift, kDeck, -1});
    const auto above = [](f32 x) { return Vec3{x, 0.5f, 0}; };
    const auto floorFor = [&](f32 x, WorldCollision::Prober prober) {
        const auto floor =
            collision.floorAt(above(x), 1, 1, 0, WorldCollision::kPlayerSide, prober);
        return floor ? floor->object : -1;
    };
    const auto wallFor = [&](WorldCollision::Prober prober) {
        return collision
            .sweepSphere({10, 1.5f, 0}, {13, 1.5f, 0}, 0.5f, WorldCollision::kWallQueryFlags,
                         prober)
            .has_value();
    };
    WorldCollision::LocalSnapshot shut;
    collision.saveLocal(shut);
    collision.setPlayerPassThrough(-1, true); // nothing to open
    collision.setPlayerPassThrough(kLift, true);
    collision.setPlayerPassThrough(kLift, true); // idempotent
    CHECK(collision.ownPlayerPassThrough(kLift));
    CHECK(collision.playerPassThrough(kDeck));
    CHECK_FALSE(collision.ownPlayerPassThrough(kDeck));
    CHECK_FALSE(collision.playerPassThrough(kRail));
    CHECK_FALSE(collision.playerPassThrough(kGround));
    CHECK_FALSE(collision.playerPassThrough(-1));
    // Opened to players is not removed: the lift still blocks everything else.
    CHECK(collision.solid(kLift));
    CHECK(collision.solid(kDeck));
    for (const auto& [x, object] : std::array{std::pair{0.0f, kLift}, std::pair{10.0f, kDeck}}) {
        CAPTURE(object);
        CHECK(floorFor(x, WorldCollision::Prober::Other) == object);
        CHECK(floorFor(x, kPlayer) == -1);
        const auto creature = collision.floorAt(above(x), 1, 1, 0, WorldCollision::kBossSide);
        REQUIRE(creature.has_value());
        CHECK(creature->object == object);
        CHECK(collision.floorUnder(above(x) + up, 3, 0.5f).has_value());
        CHECK_FALSE(collision.floorUnder(above(x) + up, 3, 0.5f, kPlayer).has_value());
    }
    CHECK(floorFor(20, kPlayer) == kRail);
    CHECK(floorFor(30, kPlayer) == kGround);
    CHECK(wallFor(WorldCollision::Prober::Other));
    CHECK_FALSE(wallFor(kPlayer));
    // A carried rider stays aboard: the deck's lock walks up to the opened lift.
    collision.setFloorExitBlocked(kLift, true);
    CHECK(collision.floorExitBlocked(kDeck));
    collision.setFloorExitBlocked(kLift, false);

    WorldCollision::LocalSnapshot opened;
    collision.saveLocal(opened);
    CHECK_FALSE(collision.equalLocal(shut, opened));
    NativeStateDigest openedDigest;
    REQUIRE(collision.appendLocalDigest(openedDigest, opened));
    NativeStateDigest shutDigest;
    REQUIRE(collision.appendLocalDigest(shutDigest, shut));
    CHECK(openedDigest.finish() != shutDigest.finish());
    collision.setPlayerPassThrough(kLift, false);
    CHECK(floorFor(10, kPlayer) == kDeck);
    CHECK(wallFor(kPlayer));
    REQUIRE(collision.restoreLocal(opened));
    CHECK(floorFor(10, kPlayer) == -1);
    NativeStateDigest restored;
    REQUIRE(collision.appendNativeDigest(restored));
    CHECK(restored.finish() == openedDigest.finish());
    collision.build(triangles, std::array<s32, 4>{-1, kLift, kDeck, -1});
    CHECK(floorFor(10, kPlayer) == kDeck); // a new map keeps no openings
}

TEST_CASE("native floor collision sides distinguish players from bosses and rewind exactly",
          "[world][collision][collision-sides][local-rewind]") {
    WorldCollision collision;
    collision.build(room());
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -2, 0}));
    const Vec3 probe{0, 0, 0};
    REQUIRE(collision.floorAt(probe, 4, 10));
    collision.setCollisionSide(0, 1);
    CHECK_FALSE(collision.solid(0));
    CHECK(collision.solid(0, 2));
    CHECK_FALSE(collision.floorAt(probe, 4, 10));
    const auto bossFloor = collision.floorAt(probe, 4, 10, 0, 2);
    REQUIRE(bossFloor);
    CHECK(bossFloor->object == 0);
    CHECK(bossFloor->y == -2);
    CHECK_FALSE(collision.floorUnder(probe, 10, 1));
    WorldCollision::LocalSnapshot moving;
    collision.saveLocal(moving);
    NativeStateDigest before;
    REQUIRE(collision.appendLocalDigest(before, moving));
    collision.setCollisionSide(0, 2);
    CHECK(collision.floorAt(probe, 4, 10));
    CHECK_FALSE(collision.floorAt(probe, 4, 10, 0, 2));
    WorldCollision::LocalSnapshot other;
    collision.saveLocal(other);
    CHECK_FALSE(collision.equalLocal(moving, other));
    NativeStateDigest changed;
    REQUIRE(collision.appendNativeDigest(changed));
    CHECK(before.finish() != changed.finish());
    REQUIRE(collision.restoreLocal(moving));
    NativeStateDigest restored;
    REQUIRE(collision.appendNativeDigest(restored));
    CHECK(before.finish() == restored.finish());
    CHECK_FALSE(collision.floorAt(probe, 4, 10));
    CHECK(collision.floorAt(probe, 4, 10, 0, 2));
    collision.setSolid(0, false);
    CHECK_FALSE(collision.floorAt(probe, 4, 10, 0, 2));
    CHECK_FALSE(collision.floorAt(probe, 4, 10, 0, 0));
    collision.setSolid(0, true);
    collision.setCollisionSide(0, 0);
    CHECK(collision.floorAt(probe, 4, 10));
    CHECK(collision.floorAt(probe, 4, 10, 0, 2));
    collision.setCollisionSide(0, 1);
    collision.build(room());
    CHECK(collision.floorAt(probe, 4, 10)); // a new map must not inherit old side masks
}

TEST_CASE("collision placement reuse does not discard signed zero transform changes",
          "[world][collision][local-rewind][collision-placement-reuse]") {
    WorldCollision collision;
    collision.build(room());
    collision.setMovingObjects(std::array<s32, 1>{0});
    WorldCollision::LocalSnapshot positive;
    WorldCollision::LocalSnapshot negative;
    collision.saveLocal(positive);
    NativeStateDigest before;
    REQUIRE(collision.appendNativeDigest(before));
    Mat4 signedZero{1};
    signedZero[3].x = -0.0f;
    REQUIRE(signedZero == Mat4{1}); // Numeric equality is too weak for native state.
    collision.setObjectTransform(0, signedZero);
    REQUIRE(collision.objectTransform(0));
    CHECK(std::signbit((*collision.objectTransform(0))[3].x));
    collision.saveLocal(negative);
    NativeStateDigest after;
    REQUIRE(collision.appendNativeDigest(after));
    CHECK(after.finish() != before.finish());
    REQUIRE(collision.restoreLocal(positive));
    CHECK_FALSE(std::signbit((*collision.objectTransform(0))[3].x));
    NativeStateDigest restored;
    REQUIRE(collision.appendNativeDigest(restored));
    CHECK(restored.finish() == before.finish());
    REQUIRE(collision.restoreLocal(negative));
    collision.setObjectTransform(0, signedZero);
    CHECK(std::bit_cast<u32>((*collision.objectTransform(0))[3].x) ==
          std::bit_cast<u32>(signedZero[3].x));
}

TEST_CASE("the floor under a point is found within the probe range", "[world][collision]") {
    WorldCollision collision;
    REQUIRE_FALSE(collision.loaded());
    collision.build(room());
    REQUIRE(collision.triangleCount() == 4);

    const auto hit = collision.floorAt(Vec3{0.0f, 1.0f, 0.0f}, 2.0f, 3.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->y == Approx(0.0f));
    REQUIRE(hit->normal.y == 1.0f);
    REQUIRE(hit->object == 0);
    // A footprint touching the edge can approach it, never walk away from it.
    const auto edge = collision.floorAt({10.4f, 1, 0}, 2, 3, 0.75f);
    // An item rests on the floor nearest its authored height: a ledge's edge within reach
    // three units up does not lift it off the face under it (the Branches' lever), while a
    // landing's edge at its height keeps a switch off a lift deck eight units down (the
    // Fields' call switch). floorAt itself still takes the highest.
    WorldCollision ledged;
    const std::array<Vec3, 3> ground{Vec3{-5, 0, -5}, Vec3{5, 0, -5}, Vec3{5, 0, 5}};
    const std::array<Vec3, 3> ledge{Vec3{5.5f, 3, -5}, Vec3{9, 3, -5}, Vec3{5.5f, 3, 5}};
    ledged.build({{{0, 1, 0}, ground, 0, 4}, {{0, 1, 0}, ledge, 1, 4}});
    REQUIRE(ledged.restingFloorAt({4.9f, 0.2f, -4}, 4, 10, 1.0f));
    CHECK(ledged.restingFloorAt({4.9f, 0.2f, -4}, 4, 10, 1.0f)->object == 0);
    CHECK(ledged.floorAt({4.9f, 0.2f, -4}, 4, 10, 1.0f)->object == 1);
    REQUIRE(ledged.restingFloorAt({4.9f, 2.9f, -4}, 4, 10, 1.0f));
    CHECK(ledged.restingFloorAt({4.9f, 2.9f, -4}, 4, 10, 1.0f)->object == 1);
    REQUIRE(ledged.restingFloorAt({5.2f, 0.2f, -4}, 4, 10, 1.0f));
    CHECK(ledged.restingFloorAt({5.2f, 0.2f, -4}, 4, 10, 1.0f)->object == 0); // edges alike
    CHECK(ledged.restingFloorAt({6.1f, 0.2f, -4}, 4, 10, 1.0f)->object == 1); // under the ledge
    CHECK_FALSE(ledged.restingFloorAt({10.5f, 0.2f, -4}, 4, 10, 1.0f));       // beyond reach
    REQUIRE(edge);
    CHECK(edge->point.x == Approx(10));
    CHECK(edge->point.y == Approx(edge->y));
    CHECK(edge->point.z == Approx(0));
    CHECK_FALSE(collision.floorAt({10.8f, 1, 0}, 2, 3, 0.75f));
    CHECK(collision.floorAhead({10.4f, 1, 0}, {-0.1f, 0, 0}, 2, 3, 0.75f));
    CHECK_FALSE(collision.floorAhead({10.4f, 1, 0}, {0.1f, 0, 0}, 2, 3, 0.75f));
    CHECK_FALSE(collision.floorAhead({10.4f, 1, 0}, {0, 0, 0}, 2, 3, 0.75f));

    REQUIRE_FALSE(collision.floorAt(Vec3{0.0f, 10.0f, 0.0f}, 2.0f, 3.0f).has_value());
    REQUIRE_FALSE(collision.floorAt(Vec3{30.0f, 1.0f, 0.0f}, 2.0f, 3.0f).has_value());
    // Walls are never floors, even straight above their edge.
    REQUIRE(collision.floorAt(Vec3{5.0f, 1.0f, 0.0f}, 2.0f, 3.0f)->y == Approx(0.0f));
}

TEST_CASE("wall rays follow the finite three-dimensional segment and stored front side",
          "[world][collision][wall-ray][alpha-contact-seed]") {
    WorldCollision collision;
    collision.build(room());
    CHECK(collision.wallBetween({4, -1, 0}, {6, 3, 0}));
    CHECK_FALSE(collision.wallBetween({4, 2, 0}, {6, 6, 0}));
    CHECK_FALSE(collision.wallBetween({4, 1, 0}, {4.9f, 1, 0}));
    CHECK_FALSE(collision.wallBetween({4, 1, 11}, {6, 1, 11}));
    CHECK(collision.wallBetween({4, 1, 0}, {5, 1, 0}));
    CHECK(collision.wallBetween({5, 1, 0}, {6, 1, 0}));
    CHECK_FALSE(collision.wallBetween({6, 1, 0}, {4, 1, 0}));
    CHECK_FALSE(collision.wallBetween({5, 1, 0}, {4, 1, 0}));
    CHECK(collision.wallBetween({5, 1, 0}, {5, 1, 0}));
    CHECK_FALSE(collision.wallBetween({5, 4, 0}, {5, 4, 0}));
    // Coplanar movement does not acquire a later intersection along the plane.
    CHECK_FALSE(collision.wallBetween({5, 4, 0}, {5, 1, 0}));
    CHECK_FALSE(collision.wallBetween({0, 1, 0}, {0, -1, 0}));
    // Cursor picking remains two-sided and still includes the floor.
    CHECK(collision.pickSurface({6, 1, 0}, {4, 1, 0}));
    CHECK(collision.pickSurface({0, 1, 0}, {0, -1, 0}));
}

TEST_CASE("enemy wall spheres catch faces edges and corners without blocking escape",
          "[world][collision][generator-terrain]") {
    WorldCollision collision;
    collision.build(room());
    // Stop short of the face with the centre, but overlap it with the sphere.
    CHECK_FALSE(collision.wallBetween({3, 1, 0}, {4.5f, 1, 0}));
    CHECK(collision.wallBetween({3, 1, 0}, {4.5f, 1, 0}, 0.6f));
    CHECK_FALSE(collision.wallBetween({3, 1, 0}, {4.5f, 1, 0}, 0.4f));
    // The swept centre clears the top and side edges; the body does not.
    CHECK(collision.wallBetween({3, 4, 0}, {7, 4, 0}, 1.1f));
    CHECK_FALSE(collision.wallBetween({3, 4, 0}, {7, 4, 0}, 0.9f));
    CHECK(collision.wallBetween({3, 1, 11}, {7, 1, 11}, 1.1f));
    CHECK_FALSE(collision.wallBetween({3, 1, 11}, {7, 1, 11}, 0.9f));
    CHECK(collision.wallBetween({3, 4, 11}, {7, 4, 11}, 1.5f));
    CHECK_FALSE(collision.wallBetween({3, 4, 11}, {7, 4, 11}, 1.4f));
    CHECK_FALSE(collision.wallBetween({7, 1, 0}, {3, 1, 0}, 2));
    CHECK_FALSE(collision.wallBetween({4.5f, 1, 0}, {3, 1, 0}, 1));
    CHECK(collision.wallBetween({4.5f, 1, 0}, {4.5f, 1, 0}, 1));
    CHECK(collision.wallBetween({4.5f, 1, -12}, {4.5f, 1, 12}, 1));
    // A horizontal floor is not an EnemyWallCollide surface.
    CHECK_FALSE(collision.wallBetween({0, 1, 0}, {1, 1, 0}, 2));
    collision.setSolid(1, false);
    CHECK_FALSE(collision.wallBetween({3, 1, 0}, {7, 1, 0}, 1));
    collision.setSolid(1, true);
    collision.setContactOnly(1, true);
    CHECK_FALSE(collision.wallBetween({3, 1, 0}, {7, 1, 0}, 1));
}

TEST_CASE("enemy birth wall spheres include slopes and thin walls between cylinder samples",
          "[world][collision][generator-terrain]") {
    WorldCollision collision;
    // A thin ledge at the collision centre misses the old knee/chest samples.
    collision.build({triangle({-10, 2.8f, 4}, {10, 2.8f, 4}, {0, 3.2f, 4}, {0, 0, -1})});
    const Vec3 from{0, 0, 0};
    const Vec3 to{0, 0, 8};
    CHECK(collision.sweepWalls(from, to, 1.5f, 0.1f, 5.9f) == to);
    CHECK(collision.wallBetween(from + Vec3{0, 3, 0}, to + Vec3{0, 3, 0}, 1.5f));
    const Vec3 normal{0, 0.6f, -0.8f};
    collision.build({triangle({-10, -5, -3.75f}, {10, -5, -3.75f}, {0, 5, 3.75f}, normal)});
    CHECK(collision.wallBetween(normal * 2.0f, normal * 0.5f, 1));
    const std::array moving{0};
    collision.setMovingObjects(moving);
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{20, 4, 0}));
    CHECK_FALSE(collision.wallBetween(normal * 2.0f, normal * 0.5f, 1));
    CHECK(collision.wallBetween(Vec3{20, 4, 0} + normal * 2.0f, Vec3{20, 4, 0} + normal * 0.5f, 1));
}

TEST_CASE("wall rays apply the native mask slope window and unfiltered-object exception",
          "[world][collision][wall-ray][alpha-contact-seed]") {
    WorldCollision collision;
    for (const f32 y : std::array{-0.9f, -0.866f, -0.6f, 0.0f, 0.6f, 0.866f, 0.9f}) {
        CAPTURE(y);
        const Vec3 normal{-std::sqrt(1 - y * y), y, 0};
        const Vec3 along = Vec3{normal.y, -normal.x, 0} * 10.0f;
        auto surface =
            triangle(-along + Vec3{0, 0, -10}, along + Vec3{0, 0, -10}, {0, 0, 10}, normal);
        collision.build({surface});
        CHECK(collision.wallBetween(normal, -normal) == (std::abs(y) <= 0.866f));
        surface.objectFlags |= 0x40;
        collision.build({surface});
        CHECK(collision.wallBetween(normal, -normal));
        CHECK_FALSE(collision.wallBetween(-normal, normal));
        surface.objectFlags = 0x40; // bypassing the slope window does not bypass the query mask
        collision.build({surface});
        CHECK_FALSE(collision.wallBetween(normal, -normal));
        surface.objectFlags = WorldCollision::kWallQueryFlags | WorldCollision::kLiquidSurface;
        collision.build({surface});
        CHECK_FALSE(collision.wallBetween(normal, -normal));
    }
    auto horizontal = triangle({-2, 0, -2}, {2, 0, -2}, {0, 0, 2}, {0, 1, 0});
    horizontal.objectFlags = 0x42;
    collision.build({horizontal});
    CHECK(collision.wallBetween({0, 1, 0}, {0, -1, 0}));
    CHECK_FALSE(collision.wallBetween({0, -1, 0}, {0, 1, 0}));
    collision.build({triangle({5, 0, 0}, {5, 1, 0}, {5, 2, 0}, {-1, 0, 0})});
    CHECK_FALSE(collision.wallBetween({4, 1, 0}, {6, 1, 0}));
}

TEST_CASE("wall rays follow moving geometry and current solid contact policy",
          "[world][collision][wall-ray][alpha-contact-seed]") {
    WorldCollision collision;
    collision.build(room());
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{3, 4, 0}));
    CHECK_FALSE(collision.wallBetween({4, 1, 0}, {6, 1, 0}));
    CHECK(collision.wallBetween({7, 5, 0}, {9, 5, 0}));
    CHECK_FALSE(collision.wallBetween({7, 1, 0}, {9, 1, 0}));
    collision.setContactOnly(1, true);
    CHECK_FALSE(collision.wallBetween({7, 5, 0}, {9, 5, 0}));
    collision.setContactOnly(1, false);
    CHECK(collision.wallBetween({7, 5, 0}, {9, 5, 0}));
    collision.setSolid(1, false);
    CHECK_FALSE(collision.wallBetween({7, 5, 0}, {9, 5, 0}));
    collision.setSolid(1, true);
    CHECK(collision.wallBetween({7, 5, 0}, {9, 5, 0}));
}

TEST_CASE("wall ray slope filtering uses a moving object's authored local normals",
          "[world][collision][wall-ray][alpha-contact-seed]") {
    WorldCollision collision;
    collision.build(room());
    const Mat4 rotation = glm::rotate(Mat4{1}, kPi * 0.5f, Vec3{0, 0, 1});
    SECTION("a rotated wall still participates when its world normal points downward") {
        collision.setMovingObjects(std::array<s32, 1>{1});
        collision.setObjectTransform(1, rotation);
        CHECK(collision.wallBetween({-1.5f, 4, 0}, {-1.5f, 6, 0}));
        CHECK_FALSE(collision.wallBetween({-1.5f, 6, 0}, {-1.5f, 4, 0}));
    }
    SECTION("a rotated floor does not become a wall-query surface") {
        collision.setMovingObjects(std::array<s32, 1>{0});
        collision.setObjectTransform(0, rotation);
        CHECK_FALSE(collision.wallBetween({-1, 1, 0}, {1, 1, 0}));
        CHECK(collision.pickSurface({-1, 1, 0}, {1, 1, 0}));
    }
}

TEST_CASE("hazard contact covers caps slopes and the full body height without blocking",
          "[world][collision][contact-only]") {
    WorldCollision collision;
    collision.build({triangle({-2, 0.1f, -2}, {2, 0.1f, -2}, {0, 0.1f, 2}, {0, 1, 0}, 0),
                     triangle({-2, 5.9f, -2}, {0, 5.9f, 2}, {2, 5.9f, -2}, {0, -1, 0}, 1)});
    collision.setContactOnly(0, true);
    collision.setContactOnly(1, true);
    const auto contacts = collision.surfaceContacts({0, 0, 0}, 0.5f, 0, 6);
    CHECK(contacts.size() == 2);
    CHECK(collision.surfaceContacts({0, 0, 0}, 0.5f, 0.2f, 5.8f).empty());
    CHECK(collision.surfaceContacts({4, 0, 0}, 0.5f, 0, 6).empty());
    // Vertical edges and degenerate projections must not become infinite planes.
    collision.build({triangle({0, 0, -1}, {0, 4, -1}, {0, 0, 1}, {1, 0, 0})});
    CHECK(collision.surfaceContacts({0.4f, 0, 0}, 0.5f, 0, 6).size() == 1);
    CHECK(collision.surfaceContacts({0, 0, 4}, 0.5f, 0, 6).empty());
    CHECK(collision.surfaceContacts({0.6f, 0, 0}, 0.5f, 0, 6).empty());
    collision.setSolid(0, false);
    CHECK(collision.surfaceContacts({0, 0, 0}, 0.5f, 0, 6).empty());
}

TEST_CASE("contact-only walls retain contacts without blocking and leave other walls solid",
          "[world][collision][contact-only]") {
    auto triangles = room();
    for (auto surface : room()) {
        if (surface.object != 1) {
            continue;
        }
        surface.object = 2;
        for (Vec3& vertex : surface.vertices) {
            vertex.x += 3;
        }
        triangles.push_back(surface);
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    collision.setContactOnly(1, true);
    collision.setContactOnly(1, true);
    CHECK(collision.contactOnly(1));
    CHECK_FALSE(collision.contactOnly(2));
    std::vector<WallContact> contacts;
    const Vec3 position{4.8f, 0, 0};
    CHECK(collision.resolveWalls(position, 0.5f, 0.2f, 2.8f, &contacts) == position);
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.front().object == 1);
    CHECK(collision.sweepWalls({4, 0, 0}, {6, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(6));
    CHECK(collision.sweepWalls({4, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(7.5f));
    collision.setSolid(1, false);
    contacts.clear();
    collision.resolveWalls(position, 0.5f, 0.2f, 2.8f, &contacts);
    CHECK(contacts.empty());
    collision.setSolid(1, true);
    collision.setContactOnly(1, false);
    CHECK(collision.resolveWalls(position, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    collision.setContactOnly(1, true);
    collision.clear();
    CHECK_FALSE(collision.contactOnly(1));
}

TEST_CASE("animated wall pushout is optional without losing contacts or sweep blocking",
          "[world][collision][moving-hazards]") {
    auto triangles = room();
    for (auto& face : triangles) {
        if (face.object == 1) {
            face.objectFlags |= 0x1000;
        }
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    const Vec3 at{4.8f, 0, 0};
    std::vector<WallContact> contacts;
    CHECK(collision.resolveWalls(at, .5f, .2f, 2.8f, &contacts, std::nullopt,
                                 WorldCollision::WallPush::StaticOnly) == at);
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.front().object == 1);
    CHECK(collision.surfaceContacts(at, .5f, .2f, 2.8f).size() == 1);
    CHECK_FALSE(collision.contactOnly(1));
    // Ordinary movement/weapon/AI queries retain their solid-wall response.
    CHECK(collision.sweepWalls({4, 0, 0}, {6, 0, 0}, .5f, .2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.resolveWalls(at, .5f, .2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.floorAt({0, 1, 0}, 1, 2)->y == 0);
    // The same option must not allow an entity push to penetrate a static wall.
    collision.build(room());
    CHECK(collision
              .resolveWalls(at, .5f, .2f, 2.8f, nullptr, std::nullopt,
                            WorldCollision::WallPush::StaticOnly)
              .x == Approx(4.5f));
}

TEST_CASE("a cylinder is pushed out of walls but left alone elsewhere", "[world][collision]") {
    WorldCollision collision;
    collision.build(room());
    const Vec3 clear = collision.resolveWalls(Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(clear == Vec3{0.0f, 0.0f, 0.0f});

    const Vec3 pushed = collision.resolveWalls(Vec3{4.8f, 0.0f, 1.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(pushed.x == Approx(4.5f).margin(0.001f));
    REQUIRE(pushed.z == Approx(1.0f));

    // Behind the wall it comes back out to the front.
    const Vec3 behind = collision.resolveWalls(Vec3{5.2f, 0.0f, 0.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(behind.x == Approx(4.5f).margin(0.001f));

    // Above the wall's top there is nothing to hit.
    const Vec3 over = collision.resolveWalls(Vec3{4.8f, 4.0f, 0.0f}, 0.5f, 4.2f, 6.0f);
    REQUIRE(over.x == Approx(4.8f));
}

TEST_CASE("wall sweeps stop thin-wall crossings and preserve the starting side",
          "[world][collision][wall-sweep]") {
    WorldCollision collision;
    auto triangles = room();
    triangles.push_back(triangle({5.1f, 0, -10}, {5.1f, 3, 10}, {5.1f, 3, -10}, {1, 0, 0}, 2));
    triangles.push_back(triangle({5.1f, 0, -10}, {5.1f, 0, 10}, {5.1f, 3, 10}, {1, 0, 0}, 2));
    collision.build(triangles);
    const Vec3 stopped = collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f);
    CHECK(stopped.x == Approx(4.5f));
    const Vec3 back = collision.sweepWalls({9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f);
    CHECK(back.x == Approx(5.6f));
    const Vec3 slide = collision.sweepWalls({0, 0, 0}, {9, 0, 3}, 0.5f, 0.2f, 2.8f);
    CHECK(slide.x == Approx(4.5f));
    CHECK(slide.z == Approx(3));
    // Contact and initial overlap permit separation, not travel through the wall.
    CHECK(collision.sweepWalls({4.5f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == 0);
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.8f));
    CHECK(collision.sweepWalls({4.9999f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.9999f));
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == 0);
    CHECK(collision.sweepWalls({5, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == 5);
    CHECK(collision.sweepWalls({0, 0, 10.6f}, {9, 0, 10.6f}, 0.5f, 0.2f, 2.8f).x == 9);
    const Vec3 rounded = collision.sweepWalls({0, 0, 10.4f}, {9, 0, 10.4f}, 0.5f, 0.2f, 2.8f);
    CHECK(rounded.z > 10.4f);
    CHECK(collision.sweepWalls({0, 4, 0}, {9, 4, 0}, 0.5f, 4.2f, 6).x == 9);
    collision.setSolid(1, false);
    collision.setSolid(2, false);
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == 9);
    collision.setSolid(1, true);
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{-2, 0, 0}));
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(2.5f));
    auto corner = room();
    corner.push_back(triangle({-10, 0, 5}, {10, 3, 5}, {10, 0, 5}, {0, 0, -1}));
    corner.push_back(triangle({-10, 0, 5}, {-10, 3, 5}, {10, 3, 5}, {0, 0, -1}));
    collision.build(corner);
    const Vec3 stoppedCorner = collision.sweepWalls({0, 0, 0}, {9, 0, 9}, 0.5f, 0.2f, 2.8f);
    CHECK(stoppedCorner.x == Approx(4.5f));
    CHECK(stoppedCorner.z == Approx(4.5f));
}

TEST_CASE("wall sweeps respect the authored front face instead of sealing one-way entrances",
          "[world][collision][wall-sweep][wall-facing]") {
    WorldCollision collision;
    collision.build(room()); // the face at x=5 points toward -x
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.sweepWalls({9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
    // Starting within the radius on the back side must still permit entry.
    CHECK(collision.sweepWalls({5.2f, 0, 0}, {4, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4));
    CHECK(collision.sweepWalls({5.2f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(9));
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
    // Moving faces use their transformed normals, not the local face direction.
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::rotate(Mat4{1}, kPi, Vec3{0, 1, 0}));
    CHECK(collision.sweepWalls({0, 0, 0}, {-9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(-4.5f));
    CHECK(collision.sweepWalls({-9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
}

TEST_CASE("wall clearance excludes only faces wholly below the caller's volume",
          "[world][collision][wall-sweep][wall-clearance]") {
    WorldCollision collision;
    collision.build({triangle({5, 0, -10}, {5, 1.9f, -10}, {5, 1.9f, 10}, {-1, 0, 0}, 7),
                     triangle({5, 0, -10}, {5, 1.9f, 10}, {5, 0, 10}, {-1, 0, 0}, 7)});
    std::vector<WallContact> contacts;
    // Unchanged cylinder callers still stop at the short face.
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f, &contacts, 2.0f).x == 9);
    CHECK(contacts.empty());
    // An overlap recheck after a successful step uses the same exclusion.
    // Other callers retain their existing cylinder behavior by default.
    CHECK(collision.resolveWalls({4.8f, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.resolveWalls({4.8f, 0, 0}, 0.5f, 0.2f, 2.8f, &contacts, 2.0f).x ==
          Approx(4.8f));
    CHECK(contacts.empty());
    CHECK(collision.resolveWalls({4.8f, 0, 0}, 0.5f, 0.2f, 2.8f, &contacts, 1.9f).x ==
          Approx(4.5f));
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.front().object == 7);
    contacts.clear();

    // The same moving face blocks again when it rises into the volume. The
    // cutoff is in world space, including when the object also turns.
    collision.setMovingObjects(std::array<s32, 1>{7});
    collision.setObjectTransform(7, glm::translate(Mat4{1}, Vec3{0, 1, 0}));
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f, &contacts, 2.0f).x ==
          Approx(4.5f));
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.front().object == 7);
    CHECK(collision.resolveWalls({4.8f, 0, 0}, 0.5f, 0.2f, 2.8f, nullptr, 2.0f).x == Approx(4.5f));
    const Mat4 turned =
        glm::rotate(glm::translate(Mat4{1}, Vec3{0, 1, 0}), kPi * 0.5f, Vec3{0, 1, 0});
    collision.setObjectTransform(7, turned);
    CHECK(collision.sweepWalls({0, 0, 0}, {0, 0, -9}, 0.5f, 0.2f, 2.8f, nullptr, 2.0f).z ==
          Approx(-4.5f));
    CHECK(collision.resolveWalls({0, 0, -4.8f}, 0.5f, 0.2f, 2.8f, nullptr, 2.0f).z ==
          Approx(-4.5f));
}

TEST_CASE("wall clearance is local along a sloping rail, not the tallest vertex of its face",
          "[world][collision][wall-clearance][sloping-rail]") {
    WorldCollision collision;
    collision.build({triangle({5, 0, -10}, {5, 1, -10}, {5, 3, 10}, {-1, 0, 0}, 7),
                     triangle({5, 0, -10}, {5, 3, 10}, {5, 0, 10}, {-1, 0, 0}, 7)});
    std::vector<WallContact> contacts;
    // The local rail top is 1.5, below the player's sphere bottom at 2.
    // Its far end reaching 3 must not make its low end a wall.
    CHECK(collision.sweepWalls({0, 0, -5}, {9, 0, -5}, 0.5f, 0.2f, 4.8f, &contacts, 2).x ==
          Approx(9));
    CHECK(collision.resolveWalls({4.8f, 0, -5}, 0.5f, 0.2f, 4.8f, &contacts, 2).x == Approx(4.8f));
    CHECK(contacts.empty());
    CHECK(collision.sweepWalls({0, 0, 5}, {9, 0, 5}, 0.5f, 0.2f, 4.8f, nullptr, 2).x ==
          Approx(4.5f));
    CHECK(collision.resolveWalls({4.8f, 0, 5}, 0.5f, 0.2f, 4.8f, nullptr, 2).x == Approx(4.5f));
}

TEST_CASE("floor edges retain tangential travel without bridging disconnected floors",
          "[world][collision][cliff]") {
    WorldCollision collision;
    const auto patch = [](f32 left, f32 right, f32 back, f32 front) {
        return std::vector<CollisionTriangle>{
            triangle({left, 0, back}, {right, 0, back}, {right, 0, front}, {0, 1, 0}),
            triangle({left, 0, back}, {right, 0, front}, {left, 0, front}, {0, 1, 0})};
    };
    auto floor = patch(-10, 0, -10, 10);
    collision.build(floor);
    const auto slide = collision.slideAlongFloor({-0.1f, 0, 0}, {0.2f, 0, 0.4f}, 1.5f, 3, 0.01f);
    REQUIRE(slide);
    CHECK(slide->x == Approx(0));
    CHECK(slide->z == Approx(0.4f));
    const auto stopped = collision.slideAlongFloor({-0.1f, 0, 0}, {0.2f, 0, 0}, 1.5f, 3, 0.01f);
    REQUIRE(stopped);
    CHECK(*stopped == Vec3{0, 0, 0});
    // The nearer destination floor is separated by a gap. Sliding must stay
    // on the starting floor instead of snapping across to the other island.
    auto island = patch(0.15f, 0.4f, 0.2f, 0.6f);
    floor.insert(floor.end(), island.begin(), island.end());
    collision.build(floor);
    const auto connected =
        collision.slideAlongFloor({-0.1f, 0, 0}, {0.45f, 0, 0.4f}, 1.5f, 3, 0.01f);
    REQUIRE(connected);
    CHECK(connected->x == Approx(0));
    CHECK(connected->z == Approx(0.4f));
    collision.clear();
    CHECK_FALSE(collision.slideAlongFloor({0, 0, 0}, {0.2f, 0, 0.4f}, 1.5f, 3));
}

TEST_CASE("world query flags distinguish walkable surfaces from wall-only geometry",
          "[world][collision]") {
    auto floor = triangle({-10, -5, -10}, {10, -5, -10}, {0, -5, 10}, {0, 1, 0});
    auto wall = triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, {-1, 0, 0});
    WorldCollision collision;
    // Temple's E1#32 points upward but has only flag 2. Normal direction alone must
    // not turn it into a floor beneath holes in the walkable mesh or beyond the stairs.
    for (const u32 flags : {0U, 2U, 0x800U}) {
        CAPTURE(flags);
        floor.objectFlags = flags;
        collision.build({floor});
        CHECK_FALSE(collision.floorAt({0, 0, 0}, 6, 6));
    }
    for (const u32 flags : {4U, 8U, 0x10U, 0x20U}) {
        CAPTURE(flags);
        floor.objectFlags = flags;
        collision.build({floor});
        REQUIRE(collision.floorAt({0, 0, 0}, 6, 6));
        CHECK(collision.floorAt({0, 0, 0}, 6, 6)->y == Approx(-5));
    }
    const Vec3 centre{4.8f, 0, 1};
    wall.objectFlags = 4; // a floor-only object is not a horizontal blocker
    collision.build({wall});
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f) == centre);
    wall.objectFlags = 2;
    collision.build({wall});
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    // Moving geometry keeps the same query policy after transformation.
    floor.objectFlags = 2;
    floor.object = 3;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{3});
    collision.setObjectTransform(3, glm::translate(Mat4{1}, Vec3{0, 5, 0}));
    CHECK_FALSE(collision.floorAt({0, 0, 0}, 6, 6));
}

TEST_CASE("water has its own surface without replacing the solid floor", "[world][collision]") {
    auto floor = triangle({-10, -2, -10}, {10, -2, -10}, {0, -2, 10}, {0, 1, 0}, 1);
    auto water = triangle({-10, 1, -10}, {10, 1, -10}, {0, 1, 10}, {0, 1, 0}, 2);
    // Mixed solid flags do not bypass the secondary-channel rule either.
    for (const auto flags : {0x200U, 0x206U}) {
        water.objectFlags = flags;
        WorldCollision collision;
        collision.build({floor, water});
        const auto solid = collision.floorAt(Vec3{0}, 5, 5);
        const auto surface = collision.liquidAt(Vec3{0}, 5, 5);
        REQUIRE(solid);
        REQUIRE(surface);
        CHECK(solid->object == 1);
        CHECK(solid->y == -2);
        CHECK(surface->object == 2);
        CHECK(surface->y == 1);
        CHECK_FALSE(collision.liquidAt(Vec3{0}, 0.5f, 0.5f));
        collision.setSolid(2, false);
        CHECK_FALSE(collision.liquidAt(Vec3{0}, 5, 5));
        collision.setSolid(2, true);
        collision.setMovingObjects(std::array<s32, 1>{2});
        collision.setObjectTransform(2, glm::translate(Mat4{1}, Vec3{0, 2, 0}));
        REQUIRE(collision.liquidAt(Vec3{0}, 5, 5));
        CHECK(collision.liquidAt(Vec3{0}, 5, 5)->y == 3);
        CHECK(collision.floorAt(Vec3{0}, 5, 5)->y == -2);
    }
    WorldCollision collision;
    collision.build({water});
    CHECK_FALSE(collision.floorAt(Vec3{0}, 5, 5));
    auto wall = triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, {-1, 0, 0}, 3);
    wall.objectFlags = 0x202;
    collision.build({wall});
    const Vec3 centre{4.8f, 0, 1};
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f) == centre);
}

TEST_CASE("a moving object's triangles stay in its own space and follow its transform",
          "[world][collision]") {
    WorldCollision collision;
    std::vector<CollisionTriangle> triangles = room();
    // A square platform half a unit up in object 3's space, and a pane along its front.
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 south{0.0f, 0.0f, 1.0f};
    triangles.push_back(triangle({-1, 0.5f, -1}, {1, 0.5f, -1}, {1, 0.5f, 1}, up, 3));
    triangles.back().objectFlags = WorldObject::kFloor;
    triangles.push_back(triangle({-1, 0.5f, -1}, {1, 0.5f, 1}, {-1, 0.5f, 1}, up, 3));
    triangles.push_back(triangle({-1, 0.5f, 1}, {-1, 3, 1}, {1, 3, 1}, south, 3));
    triangles.push_back(triangle({-1, 0.5f, 1}, {1, 3, 1}, {1, 0.5f, 1}, south, 3));
    collision.build(triangles);
    const std::array<s32, 1> movers{3};
    collision.setMovingObjects(movers);
    REQUIRE(collision.moving(3));
    REQUIRE_FALSE(collision.moving(1));
    REQUIRE(collision.movingObjectCount() == 1);
    REQUIRE(collision.triangleCount() == 8);
    // Not yet placed, it sits where the file put it: over the room's floor at the origin.
    auto hit = collision.floorAt(Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 2.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 3);
    REQUIRE(hit->y == Approx(0.5f));
    REQUIRE(hit->objectFlags == WorldObject::kFloor);
    // Placed, it is found there, and nowhere else.
    collision.setObjectTransform(3, glm::translate(Mat4{1.0f}, Vec3{-5.0f, 2.0f, -5.0f}));
    hit = collision.floorAt(Vec3{-5.0f, 3.0f, -5.0f}, 1.0f, 1.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 3);
    REQUIRE(hit->y == Approx(2.5f));
    REQUIRE(hit->objectFlags == WorldObject::kFloor);
    hit = collision.floorAt(Vec3{0.0f, 0.5f, 0.0f}, 1.0f, 1.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 0); // the room's floor again
    // Its pane blocks until it is told not to.
    const Vec3 centre{-5.0f, 2.0f, -3.6f};
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) != centre);
    collision.setSolid(3, false);
    REQUIRE_FALSE(collision.solid(3));
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) == centre);
    collision.setSolid(3, true);
    REQUIRE(collision.solid(3));
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) != centre);
    collision.setObjectTransform(9, Mat4{1.0f}); // not moving: ignored
    // Where a moving object stands can be asked; a still one has no placement.
    const auto placed = collision.objectTransform(3);
    REQUIRE(placed.has_value());
    REQUIRE(Vec3{(*placed)[3]} == Vec3{-5.0f, 2.0f, -5.0f});
    REQUIRE_FALSE(collision.objectTransform(1).has_value());
    REQUIRE_FALSE(collision.objectTransform(9).has_value());
    collision.clear();
    REQUIRE(collision.movingObjectCount() == 0);
}

TEST_CASE("collision files load their world-space triangles, skipping decoration",
          "[world][collision]") {
    const auto dir = test::scratchDirectory("world-collision");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "ROOM", "position": [10, 0, 0], "next": -1, "child": 1},
    {"name": "FLOOR", "position": [0, 2, 0], "flags": 4, "next": 2, "child": -1},
    {"name": "DECOR", "position": [0, 0, 0], "next": -1, "child": -1, "noCollision": true}
  ],
  "locators": []
})");
    writeTextFile(dir / "collision.json", R"({
  "objects": [
    {"object": 1, "normals": [0, 1, 0], "vertices": [5, 2, -5, 15, 2, -5, 15, 2, 5]},
    {"object": 2, "normals": [0, 1, 0], "vertices": [-5, 0, -5, 5, 0, 5, -5, 0, 5]}
  ]
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(layout.objects()[2].noCollision);
    WorldCollision collision;
    REQUIRE(collision.load(dir, layout));
    REQUIRE(collision.triangleCount() == 1); // the decoration's triangle is skipped
    const auto hit = collision.floorAt(Vec3{12.0f, 3.0f, -1.0f}, 2.0f, 3.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->y == Approx(2.0f)); // the file's coordinates, not offset by the object
    REQUIRE(hit->object == 1);
    REQUIRE(hit->objectFlags == WorldObject::kFloor);

    writeTextFile(dir / "collision.json",
                  R"({"objects": [{"object": 7, "normals": [], "vertices": []}]})");
    REQUIRE_FALSE(collision.load(dir, layout));
    REQUIRE_FALSE(collision.loaded());
    REQUIRE_FALSE(collision.load(test::scratchDirectory("world-collision-none"), layout));
}

TEST_CASE("floor contact margins stay bounded and do not change projectile or liquid probes",
          "[world][collision][terrain-seams]") {
    WorldCollision collision;
    // Ends at a grid boundary; nearby contact must also search the adjacent cell.
    collision.build({triangle({0, 0, 0}, {8, 0, 0}, {8, 0, 8}, {0, 1, 0}),
                     triangle({0, 0, 0}, {8, 0, 8}, {0, 0, 8}, {0, 1, 0}),
                     triangle({16, 2, 0}, {20, 2, 0}, {20, 2, 8}, {0, 1, 0})});
    const Vec3 nearEdge{8.01f, 0, 4};
    REQUIRE_FALSE(collision.floorAt(nearEdge, 1, 1));
    REQUIRE(collision.floorAt(nearEdge, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt({8.04f, 0, 4}, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt({8.01f, 2, 4}, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt(nearEdge, 1, 1, -1));
    REQUIRE_FALSE(collision.projectileFloorAt(nearEdge, 1, 1));
    REQUIRE_FALSE(collision.liquidAt(nearEdge, 1, 1));
    // Corner distance is radial, not an expanded bounding rectangle.
    REQUIRE_FALSE(collision.floorAt({8.025f, 0, 8.025f}, 1, 1, 0.03125f));
    // A floor on a moving platform uses the same contact test after its placement.
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}));
    const auto moved = collision.floorAt({8.01f, 2, 4}, 0.5f, 0.5f, 0.03125f);
    REQUIRE(moved);
    REQUIRE(moved->y == Approx(2));
}

TEST_CASE("the native tower has floors under its start points", "[world][collision][assets]") {
    const std::filesystem::path dir = test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldCollision collision;
    REQUIRE(collision.load(dir, layout));
    REQUIRE(collision.triangleCount() > 5000);
    usize found = 0;
    for (const WorldLocator& locator : layout.locators()) {
        if (locator.kind != LocatorKind::Start) {
            continue;
        }
        const auto hit = collision.floorAt(locator.position, 3.0f, 3.0f);
        if (hit.has_value()) {
            ++found;
            REQUIRE(hit->y == Approx(locator.position.y).margin(1.5f));
        }
    }
    REQUIRE(found >= 10);
}

TEST_CASE("a swept sphere meets a wall's front and never its back, first contact first",
          "[world][collision][sphere-sweep]") {
    // PlayerWallCollide's sphere, as BTriLineCol tests it: a face the centre starts behind
    // never counts, a face it parts from never counts, and of two ahead the nearer stops it.
    WorldCollision collision;
    collision.build({
        triangle({5, 0, -10}, {5, 8, -10}, {5, 8, 10}, {-1, 0, 0}),
        triangle({5, 0, -10}, {5, 8, 10}, {5, 0, 10}, {-1, 0, 0}),
        triangle({8, 0, -10}, {8, 8, -10}, {8, 8, 10}, {-1, 0, 0}),
        triangle({8, 0, -10}, {8, 8, 10}, {8, 0, 10}, {-1, 0, 0}),
    });
    const auto hit = collision.sweepSphere({0, 3.5f, 0}, {6, 3.5f, 0}, 1.5f);
    REQUIRE(hit.has_value());
    CHECK(hit->normal == Vec3{-1, 0, 0});
    CHECK(hit->time == Approx(3.5f / 6.0f)); // the centre a width and a half from x = 5
    CHECK(hit->point.x == Approx(5.0f));
    CHECK(hit->point.y == Approx(3.5f));
    // Already within its width of the wall, it is met at once; parting from it, never.
    const auto touching = collision.sweepSphere({4, 3.5f, 0}, {4, 3.5f, 1}, 1.5f);
    REQUIRE(touching.has_value());
    CHECK(touching->time == 0.0f);
    CHECK_FALSE(collision.sweepSphere({4, 3.5f, 0}, {3, 3.5f, 0}, 1.5f).has_value());
    // Behind the face, moving at it: nothing holds the body inside a wall.
    CHECK_FALSE(collision.sweepSphere({6, 3.5f, 0}, {4, 3.5f, 0}, 1.5f).has_value());
    // Over the wall's top, out of reach.
    CHECK_FALSE(collision.sweepSphere({0, 10, 0}, {6, 10, 0}, 1.5f).has_value());
    // A wall's end is met by the sphere's side, before its face is under the centre.
    const auto corner = collision.sweepSphere({4, 3.5f, -11.5f}, {4, 3.5f, -9.5f}, 1.5f);
    REQUIRE(corner.has_value());
    CHECK(corner->point.x == Approx(5.0f));
    CHECK(corner->point.z == Approx(-10.0f));
    CHECK(corner->time == Approx((1.5f - std::sqrt(1.25f)) / 2.0f).margin(0.001f));
}

TEST_CASE("the floor under a point outranks a nearer edge, and an edge within the width "
          "serves when there is no face",
          "[world][collision][floor-under]") {
    // FloorCollide: CTriListCollide ranks a face crossing far ahead of an edge contact, so
    // a lower floor under the point is the floor, whatever ledge stands beside it.
    WorldCollision collision;
    collision.build({
        triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, {0, 1, 0}),
        triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, {0, 1, 0}),
        triangle({0, -2, -4}, {10, -2, -4}, {10, -2, 4}, {0, 1, 0}),
        triangle({0, -2, -4}, {10, -2, 4}, {0, -2, 4}, {0, 1, 0}),
    });
    const auto lower = collision.floorUnder({0.2f, 2.5f, 0}, 5.5f, 1.5f);
    REQUIRE(lower.has_value());
    CHECK(lower->y == Approx(-2.0f));
    CHECK(lower->point.x == Approx(0.2f));
    // Past both faces, the nearest edge within the width is the contact.
    const auto edge = collision.floorUnder({-10.5f, 2.5f, 0}, 5.5f, 1.5f);
    REQUIRE(edge.has_value());
    CHECK(edge->y == 0.0f);
    CHECK(edge->point.x == Approx(-10.0f));
    CHECK_FALSE(collision.floorUnder({-12, 2.5f, 0}, 5.5f, 1.5f).has_value());
    // A floor above the origin is behind the sweep; one below its reach is out of it.
    CHECK_FALSE(collision.floorUnder({-5, -1, 0}, 5.5f, 1.5f).has_value());
    CHECK_FALSE(collision.floorUnder({-5, 20, 0}, 5.5f, 1.5f).has_value());
    CHECK(collision.floorUnder({-5, 7, 0}, 5.5f, 1.5f).has_value()); // the sphere's reach
}

TEST_CASE("a sphere is pushed out of a wall it is in front of and never through one it is "
          "behind or on",
          "[world][collision][sphere-push]") {
    // SlideAlongWall corrects along the face's normal, and BTriLineCol never hits a face
    // from behind: a body beside a one-sided railing is never put through it.
    WorldCollision collision;
    collision.build({
        triangle({5, 0, -10}, {5, 8, -10}, {5, 8, 10}, {-1, 0, 0}),
        triangle({5, 0, -10}, {5, 8, 10}, {5, 0, 10}, {-1, 0, 0}),
    });
    CHECK(collision.resolveSphere({4, 3.5f, 0}, 1.5f).x == Approx(3.5f));
    CHECK(collision.resolveSphere({2, 3.5f, 0}, 1.5f).x == Approx(2.0f));      // clear of it
    CHECK(collision.resolveSphere({6, 3.5f, 0}, 1.5f).x == Approx(6.0f));      // behind it
    CHECK(collision.resolveSphere({5, 3.5f, 0}, 1.5f).x == Approx(5.0f));      // on it
    CHECK(collision.resolveSphere({4, 10, 0}, 1.5f).x == Approx(4.0f));        // over its top
    CHECK(collision.resolveSphere({4, 3.5f, -11.2f}, 1.5f).x == Approx(4.0f)); // past its end
    CHECK(collision.resolveSphere({4, 3.5f, -10.5f}, 1.5f).x == Approx(3.5f)); // its corner
}

} // namespace
