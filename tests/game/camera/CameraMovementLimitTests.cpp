#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"

#include "game/camera/CameraMovementLimit.h"
#include "game/party/FloorRiding.h"
#include "game/party/PartyCombo.h"
#include "game/party/PartyMotion.h"
#include "game/party/ScriptedMotion.h"
#include "game/players/ComboMove.h"
#include "game/players/Knockback.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** How much further a spread party pushing on may still draw the camera out, as its members
 * finish sliding along the view's edges into its corners. */
constexpr f32 kCreep = 0.1f;

TEST_CASE("camera boundaries retain tangential steps and permit returning into view",
          "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 before{30, 0, 0};
    const Vec3 after = before + Vec3{1, 0, 1};
    const Vec3 result = CameraMovementLimit::constrain(before, after, Vec3{0}, camera, projection);
    CHECK(glm::distance(result, before) > 0.1f);
    CHECK(glm::distance(result, after) > 0.1f);
    const Vec3 normal = camera.right() - (1.0f - 60.0f / 640.0f) *
                                             std::tan(projection.horizontalFov / 2) *
                                             camera.forward();
    CHECK(glm::dot(result - before, normal) == Approx(0).margin(1.0e-5));
    CHECK(CameraMovementLimit::constrain(before, before - Vec3{1, 0, 0}, Vec3{0}, camera,
                                         projection) == before - Vec3{1, 0, 0});
    CHECK(CameraMovementLimit::constrain(Vec3{0}, Vec3{1, 0, 1}, Vec3{0}, camera, projection) ==
          Vec3{1, 0, 1});
    const Vec3 elevated =
        CameraMovementLimit::constrain(before, after + Vec3{0, 1, 0}, Vec3{0}, camera, projection);
    CHECK(elevated.y == 1);
}

TEST_CASE("camera boundaries allow recovery and block further separation", "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 attention{0};
    REQUIRE(CameraMovementLimit::allows(Vec3{0}, Vec3{1, 0, 0}, attention, camera, projection));
    REQUIRE_FALSE(
        CameraMovementLimit::allows(Vec3{30, 0, 0}, Vec3{31, 0, 0}, attention, camera, projection));
    REQUIRE(
        CameraMovementLimit::allows(Vec3{30, 0, 0}, Vec3{29, 0, 0}, attention, camera, projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{-30, 0, 0}, Vec3{-31, 0, 0}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{-30, 0, 0}, Vec3{-29, 0, 0}, attention, camera,
                                        projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{0, 0, 100}, Vec3{0, 0, 101}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{0, 0, 100}, Vec3{0, 0, 99}, attention, camera,
                                        projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{0, 0, -100}, Vec3{0, 0, -101}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{0, 0, -100}, Vec3{0, 0, -99}, attention, camera,
                                        projection));
}

TEST_CASE("a corner projection cannot restore movement through an already limited edge",
          "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 before{50, 0, 60};
    // Sliding along the right edge adds forward motion. Removing that motion at
    // the top edge must not leave a rightward escape through the first edge.
    const Vec3 after = before + Vec3{1, 0, 0};
    CHECK(CameraMovementLimit::constrain(before, after, Vec3{0}, camera, projection) == before);
    const Vec3 returning = before - Vec3{1, 0, 1};
    CHECK(CameraMovementLimit::constrain(before, returning, Vec3{0}, camera, projection) ==
          returning);
}

TEST_CASE("opposing players cannot zoom the shared camera out without bound",
          "[game][camera-limit]") {
    std::array<CameraSubject, 2> party{
        {{Vec3{-2, 0, 0}, Vec3{-2, 2.5f, 0}}, {Vec3{2, 0, 0}, Vec3{2, 2.5f, 0}}}};
    TowerCamera camera;
    const CameraRange range;
    const CameraView view;
    camera.reset(party, {}, range, view);
    for (s32 frame = 0; frame < 3000; ++frame) {
        for (usize i = 0; i < party.size(); ++i) {
            const Vec3 step{i == 0 ? -0.2f : 0.2f, 0, 0};
            if (CameraMovementLimit::allows(party[i].feet, party[i].feet + step, camera.attention(),
                                            camera.heldView(party.size(), {}, range), view)) {
                party[i].feet += step;
                party[i].follow += step;
            }
        }
        camera.update(party, {}, range, view, 1.0f / 30.0f);
    }
    // Held inside the view from the kept distance, the pair never takes the camera past it
    // by more than the fit's slack.
    REQUIRE(camera.distance() <=
            camera.keptDistance(party.size(), {}, range) + TowerCamera::kFarGap);
    REQUIRE(party[1].feet.x - party[0].feet.x < 40);
    REQUIRE(party[1].feet.x - party[0].feet.x > 10);
    REQUIRE(CameraMovementLimit::allows(party[0].feet, party[0].feet + Vec3{0.2f, 0, 0},
                                        camera.attention(),
                                        camera.heldView(party.size(), {}, range), view));
}
TEST_CASE("four opposing players stay within the shared view and can walk back together",
          "[game][camera-limit][multiplayer]") {
    const std::array<Vec3, 4> directions{Vec3{-1, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, -1},
                                         Vec3{0, 0, 1}};
    std::array<CameraSubject, 4> party;
    for (usize i = 0; i < party.size(); ++i) {
        party[i] = {directions[i] * 2.0f, directions[i] * 2.0f + Vec3{0, 2.5f, 0}};
    }
    TowerCamera camera;
    const CameraRange range;
    const CameraView view;
    camera.reset(party, {}, range, view);
    constexpr s32 kFrames = 1800;
    // Where the camera stood two thirds of the way through, and the furthest it stood after.
    f32 settled = 0;
    f32 furthest = 0;
    for (s32 frame = 0; frame < kFrames; ++frame) {
        for (usize i = 0; i < party.size(); ++i) {
            const Vec3 after = CameraMovementLimit::constrain(
                party[i].feet, party[i].feet + directions[i] * 0.2f, camera.attention(),
                camera.heldView(party.size(), {}, range), view, party[i].follow - party[i].feet);
            party[i].follow += after - party[i].feet;
            party[i].feet = after;
        }
        camera.update(party, {}, range, view, 1.0f / 30.0f);
        if (frame == kFrames * 2 / 3) {
            settled = camera.distance();
        } else if (frame > kFrames * 2 / 3) {
            furthest = std::max(furthest, camera.distance());
        }
    }
    // Pushing on out, the party draws the camera no further once it has spread.
    CHECK(settled > camera.keptDistance(party.size(), {}, range));
    CHECK(furthest <= settled + kCreep);
    for (const auto& player : party) {
        // The shared view may translate while edge-tangent motion is preserved.
        // Bound separation from the party, not travel from the world's origin.
        CHECK(glm::distance(player.feet, camera.attention()) < range.radiusMax * 2.0f);
        for (const Vec3& point : {player.feet, player.follow}) {
            const Vec3 relative = point - camera.camera().position;
            const f32 depth = glm::dot(relative, camera.camera().forward());
            REQUIRE(depth > 0);
            const f32 halfWidth = depth * std::tan(view.horizontalFov * 0.5f);
            CHECK(std::abs(glm::dot(relative, camera.camera().right())) < halfWidth);
            CHECK(std::abs(glm::dot(relative, camera.camera().up())) < halfWidth / view.aspect);
        }
        const Vec3 towards = camera.attention() - player.feet;
        const Vec3 back = player.feet + glm::normalize(Vec3{towards.x, 0, towards.z}) * 0.2f;
        const Vec3 recovered = CameraMovementLimit::constrain(
            player.feet, back, camera.attention(), camera.heldView(party.size(), {}, range), view,
            player.follow - player.feet);
        CHECK(glm::distance(recovered, back) == Approx(0).margin(1.0e-5));
    }
}

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr u32 kFloorQuery = 0x4;  ///< of WorldCollision::kFloorQueryFlags
constexpr s32 kGround = 0;        ///< the wide floor the party stands on
constexpr s32 kPlatform = 1;      ///< a lift, a moving floor or a trapdoor
constexpr s32 kPit = 2;           ///< the floor under a trapdoor
constexpr f32 kGroundReach = 300; ///< the floor's half width
constexpr f32 kPlatformReach = 2; ///< a lift's or a trapdoor's half width
constexpr f32 kPitDepth = 30;     ///< how far under the floor a trapdoor drops its rider
constexpr f32 kCarried = 12;      ///< how far a lift or a moving floor takes its rider
constexpr f32 kCarrySeconds = 3;  ///< and how long it takes over it
constexpr f32 kPartyHeading = 0;  ///< the yaw the first camera marker looks along
constexpr f32 kTurnedHeading = kPi / 2;
constexpr f32 kMarkerPitch = 0.6f;
constexpr f32 kMarkerDistance = 200; ///< the first marker stands this far behind the party

/** A square of floor from `min` to `max` at height `y`, belonging to `object`. */
std::vector<CollisionTriangle> square(Vec2 min, Vec2 max, f32 y, s32 object,
                                      u32 flags = kFloorQuery) {
    CollisionTriangle first;
    first.vertices = {Vec3{min.x, y, min.y}, Vec3{max.x, y, max.y}, Vec3{max.x, y, min.y}};
    first.normal = Vec3{0, 1, 0};
    first.object = object;
    first.objectFlags = flags;
    CollisionTriangle second = first;
    second.vertices = {Vec3{min.x, y, min.y}, Vec3{min.x, y, max.y}, Vec3{max.x, y, max.y}};
    return {first, second};
}

WorldLocator marker(const Vec3& position, f32 yaw) {
    WorldLocator out;
    out.kind = LocatorKind::TriggerCamera;
    out.position = position;
    out.rotation = Vec3{kMarkerPitch, yaw, 0.0f};
    return out;
}

/** How far `point` lies outside `camera`'s whole frame, as a share of the frame's half size
 * at its depth: nought or less on screen. */
f32 outsideFrame(const WorldCamera& camera, const CameraView& view, const Vec3& point) {
    const Vec3 relative = point - camera.position;
    const f32 depth = glm::dot(relative, camera.forward());
    if (depth <= WorldCamera::kNear) {
        return 1.0f;
    }
    const f32 tanX = std::tan(view.horizontalFov * 0.5f);
    const f32 tanY = tanX / view.aspect;
    return std::max(std::abs(glm::dot(relative, camera.right())) / (depth * tanX),
                    std::abs(glm::dot(relative, camera.up())) / (depth * tanY)) -
           1.0f;
}

/**
 * A party of three or four on wide ground, run out as far as the shared camera's view lets
 * it, stepping `rate` times a second. Each step is held inside the camera's held view as the
 * play scene holds it (PlayScene::limitStep), then the camera follows the party. Pushing out,
 * the members slide along the view's edges into its corners and the view drifts after them,
 * until the camera stands at a distance the party cannot draw it past.
 */
struct SpreadParty {
    /** The way each member heads out, against the camera: left, right, toward the camera and
     * away from it. */
    static constexpr std::array<Vec2, 4> kOutward{Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    static constexpr usize kLeft = 0;
    static constexpr usize kRight = 1;
    static constexpr usize kNear = 2;
    static constexpr f32 kSpreadSeconds = 15;
    static constexpr f32 kSettleSeconds = 1;
    static constexpr f32 kGatherSeconds = 6;

    /** What a stretch of updates saw at its worst. */
    struct Watch {
        f32 outside = -1; ///< the furthest anyone stood outside the frame, as outsideFrame
        f32 distance = 0; ///< the furthest the camera stood back
    };

    usize count;
    s32 rate;
    std::array<PlayerRuntime, 4> members;
    std::array<PlayInput, 4> inputs{};
    WorldCollision collision;
    std::vector<WorldLocator> markers{marker(Vec3{0, 0, -kMarkerDistance}, kPartyHeading)};
    CameraRange range;
    CameraView view;
    TowerCamera camera;
    test::MotionScript events;
    test::ScriptedMotion level{events};
    f32 plateau = 0; ///< how far back the spread party holds the camera

    SpreadParty(usize size, s32 updates) : count(size), rate(updates) {
        ground({});
        for (usize i = 0; i < count; ++i) {
            const Vec3 out = outward(i);
            members[i].actor.spawn(static_cast<s32>(i), {}, nullptr, out * 2.0f, 0.0f);
            members[i].actor.turnTo(std::atan2(out.x, out.z));
        }
        events.limitMovement = [this](usize i, const Vec3& before, const Vec3& after) {
            const PlayerActor& actor = members[i].actor;
            return CameraMovementLimit::constrain(before, after, camera.attention(),
                                                  camera.heldView(count, markers, range), view,
                                                  actor.followPoint() - actor.position());
        };
        std::vector<CameraSubject> subjects;
        for (const PlayerRuntime& member : party()) {
            subjects.push_back({member.actor.position(), member.actor.followPoint()});
        }
        camera.reset(subjects, markers, range, view);
        headOut(1);
        run(kSpreadSeconds);
        inputs = {};
        run(kSettleSeconds);
        plateau = camera.distance();
    }
    ~SpreadParty() = default;
    // The limit it hands the level points back at it.
    GDL_NON_COPYABLE_NON_MOVABLE(SpreadParty);

    std::span<PlayerRuntime> party() { return std::span{members}.first(count); }
    std::span<const PlayerRuntime> party() const { return std::span{members}.first(count); }
    /** Member `i`'s way out, along the ground. */
    static Vec3 outward(usize i) { return Vec3{kOutward[i].x, 0, kOutward[i].y}; }
    /** Every member pushes its stick out by `magnitude`. */
    void headOut(f32 magnitude) {
        for (usize i = 0; i < count; ++i) {
            inputs[i].move = MoveInput{kOutward[i], magnitude};
        }
    }
    /** The floor, with a trapdoor over a pit under `trapdoor` when given, and `extra`. */
    void ground(std::vector<CollisionTriangle> extra, const Vec3* trapdoor = nullptr) {
        std::vector<CollisionTriangle> faces;
        const auto add = [&faces](std::vector<CollisionTriangle> more) {
            faces.insert(faces.end(), more.begin(), more.end());
        };
        if (trapdoor == nullptr) {
            add(square({-kGroundReach, -kGroundReach}, {kGroundReach, kGroundReach}, 0, kGround));
        } else {
            const Vec2 low{trapdoor->x - kPlatformReach, trapdoor->z - kPlatformReach};
            const Vec2 high{trapdoor->x + kPlatformReach, trapdoor->z + kPlatformReach};
            add(square({-kGroundReach, -kGroundReach}, {kGroundReach, low.y}, 0, kGround));
            add(square({-kGroundReach, high.y}, {kGroundReach, kGroundReach}, 0, kGround));
            add(square({-kGroundReach, low.y}, {low.x, high.y}, 0, kGround));
            add(square({high.x, low.y}, {kGroundReach, high.y}, 0, kGround));
            add(square(low, high, 0, kPlatform));
            add(square(low, high, -kPitDepth, kPit));
        }
        add(std::move(extra));
        collision.build(std::move(faces));
    }
    /** A floor under member `i` that moves, standing where it stands, a little proud of the
     * ground so that it is the floor under the member. */
    Vec3 platformUnder(usize i) {
        const Vec3 at = members[i].actor.position() + Vec3{0, 0.25f, 0};
        ground(square({-kPlatformReach, -kPlatformReach}, {kPlatformReach, kPlatformReach}, 0,
                      kPlatform, FloorRiding::kMoving | kFloorQuery));
        const std::array<s32, 1> moving{kPlatform};
        collision.setMovingObjects(moving);
        collision.setObjectTransform(kPlatform, glm::translate(Mat4{1}, at));
        return at;
    }
    /** One update: the party moves, then the camera follows it. */
    void step() {
        const f32 seconds = 1.0f / static_cast<f32>(rate);
        const auto moved = PartyMotion::step(party(), inputs, false, camera.yaw(), 60 / rate,
                                             seconds, collision, level);
        camera.update(moved, markers, range, view, seconds);
    }
    /** The furthest any member stands outside the frame now, feet or followed point. */
    f32 worst() const {
        f32 out = -1;
        for (const PlayerRuntime& member : party()) {
            for (const Vec3& point : {member.actor.position(), member.actor.followPoint()}) {
                out = std::max(out, outsideFrame(camera.camera(), view, point));
            }
        }
        return out;
    }
    /** The furthest the party on its own may still draw the camera out. */
    f32 cap() const { return plateau + kCreep; }
    /** Updates for `seconds`, `before(n)` setting up each update n first. */
    Watch run(f32 seconds, const std::function<void(s32)>& before = {}) {
        Watch seen;
        const auto updates = static_cast<s32>(std::lround(seconds * static_cast<f32>(rate)));
        for (s32 n = 0; n < updates; ++n) {
            if (before) {
                before(n);
            }
            step();
            seen.outside = std::max(seen.outside, worst());
            seen.distance = std::max(seen.distance, camera.distance());
        }
        return seen;
    }
    /** Updates while `going` holds, for at most `seconds`. */
    Watch runWhile(const std::function<bool()>& going, f32 seconds) {
        Watch seen;
        const auto updates = static_cast<s32>(std::lround(seconds * static_cast<f32>(rate)));
        for (s32 n = 0; n < updates && going(); ++n) {
            step();
            seen.outside = std::max(seen.outside, worst());
            seen.distance = std::max(seen.distance, camera.distance());
        }
        return seen;
    }
    /** Every member walks toward where the camera looks, for `seconds`. */
    Watch gather(f32 seconds) {
        return run(seconds, [this](s32) {
            for (usize i = 0; i < count; ++i) {
                const Vec3 in = camera.attention() - members[i].actor.position();
                const f32 heading = std::atan2(in.x, in.z) - camera.yaw();
                inputs[i].move = MoveInput{Vec2{std::sin(heading), std::cos(heading)}, 1};
            }
        });
    }
    /** The camera's ring of updates, after which it has taken in a change. */
    f32 settling() const { return static_cast<f32>(TowerCamera::kRing) / static_cast<f32>(rate); }
};

TEST_CASE("a party spread as far as the shared view lets it cannot walk, run, be knocked, shoved "
          "or flung by a partner out of it, nor draw the camera any further out",
          "[game][camera-limit][multiplayer]") {
    const auto count = GENERATE(as<usize>{}, 3, 4);
    const auto rate = GENERATE(30, 60);
    CAPTURE(count, rate);
    SpreadParty party(count, rate);
    // Run out as far as they can, the party has the camera standing back past the distance it
    // keeps, everyone still in frame.
    REQUIRE(party.worst() <= 0);
    REQUIRE(party.plateau > party.camera.keptDistance(count, party.markers, party.range));

    SECTION("walking and running on out") {
        party.headOut(0.4f);
        const auto walked = party.run(2);
        party.headOut(1);
        const auto ran = party.run(2);
        CHECK(walked.outside <= 0);
        CHECK(ran.outside <= 0);
        CHECK(walked.distance <= party.cap());
        CHECK(ran.distance <= party.cap());
    }
    SECTION("knocked and shoved on out") {
        for (usize i = 0; i < count; ++i) {
            party.members[i].knockback.queue(SpreadParty::outward(i), Knockback::kBlownAway, 10.0f);
        }
        const auto knocked = party.run(1.5f);
        const auto shoved = party.run(1.5f, [&](s32) {
            for (usize i = 0; i < count; ++i) {
                PlayerRuntime& member = party.members[i];
                member.knockback.shove(SpreadParty::outward(i) * member.actor.speed() /
                                           static_cast<f32>(rate),
                                       1.0f / static_cast<f32>(rate));
            }
        });
        CHECK(knocked.outside <= 0);
        CHECK(shoved.outside <= 0);
        CHECK(knocked.distance <= party.cap());
        CHECK(shoved.distance <= party.cap());
    }
    SECTION("flung on out by a partner") {
        PartyCombo::begin(party.party(), SpreadParty::kLeft, SpreadParty::kRight);
        PlayerRuntime& flier = party.members[SpreadParty::kRight];
        party.members[SpreadParty::kLeft].combo.grabberClass = ComboMove::kWarrior;
        flier.combo.grabberClass = ComboMove::kWarrior;
        flier.combo.role = ComboRole::Thrown;
        flier.combo.ticksLeft = ComboMove::kFlightTicks;
        flier.actor.turnTo(kPartyHeading + kPi / 2);
        // The flight's four seconds; its thrower, with no figure here, never lets go.
        const auto flown = party.run(static_cast<f32>(ComboMove::kFlightTicks) / 60.0f);
        REQUIRE(PartyCombo::aside(flier));
        CHECK(flown.outside <= 0);
        CHECK(flown.distance <= party.cap());
    }
}

TEST_CASE("whatever carries a member of a spread party out of the shared view, the camera keeps "
          "them in frame until they come back in",
          "[game][camera-limit][multiplayer]") {
    const auto count = GENERATE(as<usize>{}, 3, 4);
    const auto rate = GENERATE(30, 60);
    CAPTURE(count, rate);
    SpreadParty party(count, rate);
    REQUIRE(party.worst() <= 0);
    const f32 cap = party.cap();

    SECTION("thrown out by an enemy") {
        PlayerRuntime& thrown = party.members[SpreadParty::kRight];
        const Vec3 hand = thrown.actor.followPoint() + Vec3{0, 1, 0};
        thrown.capture.attach(0, false, glm::translate(Mat4{1}, hand), thrown.actor);
        const auto held = party.run(0.5f);
        thrown.capture.release(SpreadParty::outward(SpreadParty::kRight) * 40.0f + Vec3{0, 40, 0},
                               0);
        const auto flight = party.runWhile([&] { return thrown.capture.active(); }, 3);
        REQUIRE_FALSE(thrown.capture.active());
        const auto landed = party.run(party.settling());
        // Out past where the party could have walked it, the camera has stood back to show it.
        CHECK(landed.distance > cap);
        CHECK(held.outside <= 0);
        CHECK(flight.outside <= 0);
        CHECK(landed.outside <= 0);
        // Pushing on out, the view holds it; walking back in is free, and the camera comes back
        // in with the party.
        party.inputs[SpreadParty::kRight].move =
            MoveInput{SpreadParty::kOutward[SpreadParty::kRight], 1};
        const auto pushed = party.run(1);
        CHECK(pushed.outside <= 0);
        const auto back = party.gather(SpreadParty::kGatherSeconds);
        CHECK(back.outside <= 0);
        CHECK(party.camera.distance() <= cap);
    }
    SECTION("lifted up") {
        const Vec3 at = party.platformUnder(SpreadParty::kRight);
        party.run(0.5f);
        const PlayerRuntime& rider = party.members[SpreadParty::kRight];
        REQUIRE(rider.floor.object == kPlatform);
        const f32 rise = kCarried / (kCarrySeconds * static_cast<f32>(rate));
        const auto lifted = party.run(kCarrySeconds, [&](s32 n) {
            party.collision.setObjectTransform(
                kPlatform,
                glm::translate(Mat4{1}, at + Vec3{0, rise * static_cast<f32>(n + 1), 0}));
        });
        CHECK(rider.actor.position().y == Approx(at.y + kCarried).margin(0.01f));
        const auto aloft = party.run(party.settling());
        CHECK(aloft.distance > cap);
        CHECK(lifted.outside <= 0);
        CHECK(aloft.outside <= 0);
        // Brought down again, the camera comes back in with the party.
        party.collision.setObjectTransform(kPlatform, glm::translate(Mat4{1}, at));
        const auto down = party.gather(SpreadParty::kGatherSeconds);
        CHECK(down.outside <= 0);
        CHECK(party.camera.distance() <= cap);
    }
    SECTION("carried out on a moving floor") {
        const Vec3 at = party.platformUnder(SpreadParty::kLeft);
        party.run(0.5f);
        const PlayerRuntime& rider = party.members[SpreadParty::kLeft];
        REQUIRE(rider.floor.object == kPlatform);
        const f32 slide = kCarried / (kCarrySeconds * static_cast<f32>(rate));
        const Vec3 away = SpreadParty::outward(SpreadParty::kLeft);
        const auto carried = party.run(kCarrySeconds, [&](s32 n) {
            party.collision.setObjectTransform(
                kPlatform, glm::translate(Mat4{1}, at + away * (slide * static_cast<f32>(n + 1))));
        });
        CHECK(glm::dot(rider.actor.position() - at, away) == Approx(kCarried).margin(0.01f));
        const auto there = party.run(party.settling());
        CHECK(there.distance > cap);
        CHECK(carried.outside <= 0);
        CHECK(there.outside <= 0);
        // Walking back in is free, and the camera comes back in with the party.
        const auto back = party.gather(SpreadParty::kGatherSeconds);
        CHECK(back.outside <= 0);
        CHECK(party.camera.distance() <= cap);
    }
    SECTION("dropped through a trapdoor") {
        PlayerRuntime& faller = party.members[SpreadParty::kNear];
        const Vec3 at = faller.actor.position();
        party.ground({}, &at);
        party.run(0.5f);
        party.collision.setSolid(kPlatform, false);
        const auto fell =
            party.runWhile([&] { return faller.actor.position().y > -kPitDepth + 0.01f; }, 4);
        CHECK(faller.actor.position().y == Approx(-kPitDepth).margin(0.01f));
        const auto below = party.run(party.settling());
        CHECK(below.distance > cap);
        CHECK(fell.outside <= 0);
        CHECK(below.outside <= 0);
    }
    SECTION("turned to another marker") {
        party.markers.push_back(marker(party.camera.attention(), kTurnedHeading));
        const auto turning =
            party.runWhile([&] { return party.camera.marker() != 1 || party.camera.turning(); }, 3);
        REQUIRE(party.camera.marker() == 1);
        REQUIRE_FALSE(party.camera.turning());
        CHECK(party.camera.yaw() == Approx(kTurnedHeading).margin(1.0e-4f));
        const auto turned = party.run(party.settling());
        CHECK(turning.outside <= 0);
        CHECK(turned.outside <= 0);
        // Gathering in again, the party has the camera back in.
        const auto gathered = party.gather(SpreadParty::kGatherSeconds);
        CHECK(gathered.outside <= 0);
        CHECK(party.camera.distance() <= cap);
    }
}

} // namespace
