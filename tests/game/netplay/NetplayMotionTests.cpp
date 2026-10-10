#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/netplay/ImpairedInputLink.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/SessionInputs.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** Runs real movement, collision, event dispatch and ONE camera for all four seats.
 * No renderer, assets, controllers or human interaction needed. The comparison world
 * is a test oracle for input delivery, not a proposed client-side lockstep protocol. */
struct MotionWorld {
    std::array<PlayerRuntime, 4> players;
    WorldCollision collision;
    TowerCamera camera;
    CameraRange range;
    CameraView view;
    std::array<u32, 4> potions{};
    std::array<u32, 4> toggles{};

    MotionWorld() {
        CollisionTriangle first;
        first.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, -200}, Vec3{200, 0, 200}};
        first.normal = Vec3{0, 1, 0};
        first.object = 0;
        CollisionTriangle second = first;
        second.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{-200, 0, 200}};
        collision.build({first, second});
        const std::array<s32, 4> seats{3, 0, 2, 1}; // party order is not input ownership
        std::array<CameraSubject, 4> subjects;
        for (usize i = 0; i < players.size(); ++i) {
            players[i].actor.spawn(seats[i], {}, nullptr,
                                   Vec3{static_cast<f32>(seats[i]) * 2 - 3, 0, 0}, 0);
            subjects[i] = {players[i].actor.position(), players[i].actor.followPoint()};
        }
        camera.reset(subjects, {}, range, view);
    }

    void step(const SessionInputs::Frame& inputs) {
        PartyMotion::Events events;
        events.perform = [&](usize player, PartyMotion::Action action) {
            REQUIRE(action == PartyMotion::Action::NoPotion);
            ++potions[static_cast<usize>(players[player].actor.player())];
        };
        events.select = [&](usize player, const SelectorInput& input, s32) {
            if (input.up) {
                ++toggles[static_cast<usize>(players[player].actor.player())];
            }
        };
        events.limitMovement = [&](usize player, const Vec3& from, const Vec3& to) {
            const auto& actor = players[player].actor;
            return CameraMovementLimit::constrain(from, to, camera.attention(), camera.camera(),
                                                  view, actor.followPoint() - actor.position());
        };
        const auto subjects = PartyMotion::step(players, inputs, false, camera.yaw(), 1,
                                                1.0f / 60.0f, collision, events);
        camera.update(subjects, {}, range, view, 1.0f / 60.0f);
    }

    MotionSnapshot snapshot(const InputTimeline& timeline) const {
        REQUIRE(timeline.tick() > 0);
        MotionSnapshot result;
        result.epoch = timeline.epoch();
        result.tick = timeline.tick() - 1; // state after this completed tick
        result.cameraContinuity = 1;
        result.camera = camera.camera();
        result.horizontalFov = view.horizontalFov;
        result.aspect = view.aspect;
        for (const auto& player : players) {
            const auto seat = static_cast<usize>(player.actor.player());
            result.players[seat] =
                SeatMotion{timeline.grant(seat), 1, player.actor.position(), player.actor.yaw()};
        }
        return result;
    }
};

PlayInput scripted(u64 tick, usize seat) {
    const std::array<Vec2, 4> outward{Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    PlayInput input;
    input.move = {tick < 500 ? outward[seat] : -outward[seat], 1};
    input.usePotion = tick % 59 == seat;
    input.selector.up = tick % 73 == seat;
    return input;
}

TEST_CASE("delayed lossy reordered input drives four real players and the shared camera",
          "[netplay][netplay-harness][multiplayer]") {
    SessionInputs host;
    auto& timeline = host.timeline();
    REQUIRE(timeline.assign(1, 2));
    REQUIRE(timeline.assign(2, 2));
    REQUIRE(timeline.assign(3, 3));
    std::array<InputHistory, 2> history;
    test::ImpairedInputLink link({1, 3, 5, 3, 10}); // 17-67 ms, 20% loss, delayed duplicates
    constexpr u64 kLead = 7;
    MotionWorld actual;
    MotionWorld reference;
    // One-way output is independent of the input-delivery oracle. A remote
    // presentation never steps actors, collision, inventory or its own camera.
    SnapshotPlayback client;
    REQUIRE(client.begin(1, timeline.epoch()));
    test::ImpairedInputLink snapshots({1, 5, 5, 3, 10});
    std::vector<MotionSnapshot> hostStates;
    u64 staleSnapshots = 0;
    u64 duplicates = 0;
    u64 late = 0;
    for (u64 now = 0; now < 1000; ++now) {
        for (usize remote = 0; remote < history.size(); ++remote) {
            const auto peer = remote + 2;
            std::vector<InputCommand> frame;
            for (usize seat = 1; seat < InputCommand::kSeats; ++seat) {
                if (timeline.owner(seat) == peer) {
                    frame.push_back(SessionInputs::command(scripted(now, seat), timeline.epoch(),
                                                           now + kLead, timeline.grant(seat),
                                                           static_cast<u8>(seat)));
                }
            }
            REQUIRE(history[remote].record(frame));
            const auto bytes = InputPacket::encode(history[remote].commands());
            REQUIRE(bytes);
            link.send(now, peer, *bytes);
        }
        for (const auto admission : link.deliver(now, timeline)) {
            CHECK((admission == InputTimeline::Admission::Accepted ||
                   admission == InputTimeline::Admission::Duplicate ||
                   admission == InputTimeline::Admission::TooLate));
            duplicates += admission == InputTimeline::Admission::Duplicate ? 1 : 0;
            late += admission == InputTimeline::Admission::TooLate ? 1 : 0;
        }
        SessionInputs::Frame local;
        local[0] = scripted(now, 0);
        auto expected = local;
        if (now >= kLead) {
            for (usize seat = 1; seat < expected.size(); ++seat) {
                expected[seat] = scripted(now - kLead, seat);
            }
        }
        actual.step(host.advance(local));
        reference.step(expected);
        hostStates.push_back(actual.snapshot(timeline));
        if (now % 3 == 0) { // 20 snapshots/sec, simulation still 60 ticks/sec
            const auto bytes = MotionPacket::encode(hostStates.back());
            REQUIRE(bytes);
            snapshots.send(now, 1, *bytes);
        }
        for (const auto admission : snapshots.deliver(now, client)) {
            CHECK((admission == SnapshotPlayback::Admission::Accepted ||
                   admission == SnapshotPlayback::Admission::Stale));
            staleSnapshots += admission == SnapshotPlayback::Admission::Stale ? 1 : 0;
        }
        if (const auto* latest = client.latest()) {
            const auto& authoritative = hostStates.at(static_cast<usize>(latest->tick));
            CHECK(latest->camera.position == authoritative.camera.position);
            CHECK(latest->camera.yaw == authoritative.camera.yaw);
            CHECK(latest->camera.pitch == authoritative.camera.pitch);
            for (usize seat = 0; seat < 4; ++seat) {
                REQUIRE(latest->players[seat]);
                CHECK(latest->players[seat]->position == authoritative.players[seat]->position);
                CHECK(latest->players[seat]->grant == timeline.grant(seat));
            }
            const auto shown = client.sample(now > 9 ? now - 9 : 0, 0.5f);
            REQUIRE(shown);
            CHECK(shown->valid());
            CHECK(shown->tick <= latest->tick);
        }
        for (usize i = 0; i < actual.players.size(); ++i) {
            CHECK(actual.players[i].actor.position() == reference.players[i].actor.position());
        }
        CHECK(actual.potions == reference.potions);
        CHECK(actual.toggles == reference.toggles);
        CHECK(actual.camera.attention() == reference.camera.attention());
        CHECK(actual.camera.distance() == reference.camera.distance());
        CHECK(actual.camera.distance() <= actual.range.radiusMax + 0.01f);
        // With the whole party represented, every player's anchor fits the one view.
        for (const auto& player : actual.players) {
            const Vec3 relative = player.actor.followPoint() - actual.camera.camera().position;
            const f32 depth = glm::dot(relative, actual.camera.camera().forward());
            REQUIRE(depth > 0);
            const f32 halfWidth = depth * std::tan(actual.view.horizontalFov * 0.5f);
            CHECK(std::abs(glm::dot(relative, actual.camera.camera().right())) < halfWidth);
            CHECK(std::abs(glm::dot(relative, actual.camera.camera().up())) <
                  halfWidth / actual.view.aspect);
        }
    }
    CHECK(link.dropped() > 0);
    CHECK(link.duplicated() > 0);
    CHECK(link.reordered() > 0);
    CHECK(link.malformed() == 0);
    CHECK(duplicates > 0);
    CHECK(late > 0);
    CHECK(snapshots.dropped() > 0);
    CHECK(snapshots.duplicated() > 0);
    CHECK(snapshots.reordered() > 0);
    CHECK(staleSnapshots > 0);
    // A fresh full state converges after an outage without requesting a delta base.
    const auto finalPacket = MotionPacket::encode(hostStates.back());
    REQUIRE(finalPacket);
    REQUIRE(client.receive(1, *finalPacket) == SnapshotPlayback::Admission::Accepted);
    const auto converged = client.sample(5000);
    REQUIRE(converged);
    CHECK(converged->camera.position == actual.camera.camera().position);
    for (const auto& player : actual.players) {
        const auto seat = static_cast<usize>(player.actor.player());
        CHECK(converged->players[seat]->position == player.actor.position());
    }
    for (const auto count : actual.potions) {
        CHECK(count > 0);
    }
}

TEST_CASE("a stalled remote connection stops a real player without stopping the host",
          "[netplay][netplay-harness][multiplayer]") {
    SessionInputs host;
    auto& timeline = host.timeline();
    REQUIRE(timeline.assign(3, 2));
    MotionWorld world;
    test::ImpairedInputLink link({});
    PlayInput remote;
    remote.move = {{1, 0}, 1};
    remote.usePotion = true;
    const auto bytes = InputPacket::encode(std::array{
        SessionInputs::command(remote, timeline.epoch(), timeline.tick(), timeline.grant(3), 3)});
    REQUIRE(bytes);
    link.send(0, 2, *bytes);
    REQUIRE(link.deliver(0, timeline).size() == 1);
    world.step(host.advance({}));
    for (u64 tick = 0; tick < InputTimeline::kHoldTicks + 2; ++tick) {
        world.step(host.advance({}));
    }
    const Vec3 stopped = world.players[0].actor.position();     // seat 3
    const Vec3 localBefore = world.players[1].actor.position(); // seat 0
    SessionInputs::Frame local;
    local[0].move = {{0, 1}, 1};
    for (s32 tick = 0; tick < 20; ++tick) {
        world.step(host.advance(local));
    }
    CHECK(glm::distance(world.players[0].actor.position(), stopped) == Approx(0).margin(0.0001));
    CHECK(glm::distance(world.players[1].actor.position(), localBefore) > 0.1f);
    CHECK(world.potions[3] == 1);
}
} // namespace
