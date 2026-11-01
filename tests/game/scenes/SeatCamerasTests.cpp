#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/camera/TowerCamera.h"
#include "game/party/PlayerRuntime.h"
#include "game/scenes/SeatCameras.h"

using namespace gdl;
using namespace gdl::game;

namespace {

/** Three in the party, in seats 0, 2 and 3, standing at different places. */
struct Party {
    std::array<PlayerRuntime, 3> players;
    std::array<CameraSubject, 3> subjects{{{Vec3{0, 0, 0}, Vec3{0, 1, 0}, 1},
                                           {Vec3{10, 0, 0}, Vec3{10, 1, 0}, 1},
                                           {Vec3{0, 0, 10}, Vec3{0, 1, 10}, 1}}};
    Party() {
        players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
        players[1].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
        players[2].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    }
};

/** A camera stood up on `subjects` alone, to compare a seat's against. */
TowerCamera resetOn(std::span<const CameraSubject> subjects) {
    TowerCamera camera;
    camera.reset(subjects, {}, CameraRange{}, CameraView{});
    return camera;
}

} // namespace

TEST_CASE("a standing seat's camera follows its own character", "[scenes][seat-cameras]") {
    Party party;
    SeatCameras seats;
    seats.reset(party.players, party.subjects, {}, CameraRange{}, CameraView{});
    const auto own = std::span<const CameraSubject>{party.subjects};
    CHECK(seats.camera(0).localState() == resetOn(own.subspan(0, 1)).localState());
    CHECK(seats.camera(2).localState() == resetOn(own.subspan(1, 1)).localState());
    CHECK(seats.camera(3).localState() == resetOn(own.subspan(2, 1)).localState());
    // Seat 1 is nobody's: its camera is never touched.
    CHECK(seats.camera(1).localState() == TowerCamera{}.localState());
}

TEST_CASE("a fallen seat's camera keeps with the standing party, or everyone when none stand",
          "[scenes][seat-cameras]") {
    Party party;
    party.players[1].life = PlayerLife::Dying;
    SeatCameras seats;
    seats.reset(party.players, party.subjects, {}, CameraRange{}, CameraView{});
    const std::array<CameraSubject, 2> standing{party.subjects[0], party.subjects[2]};
    CHECK(seats.camera(2).localState() == resetOn(standing).localState());
    for (auto& player : party.players) {
        player.life = PlayerLife::InTower;
    }
    seats.reset(party.players, party.subjects, {}, CameraRange{}, CameraView{});
    CHECK(seats.camera(0).localState() == resetOn(party.subjects).localState());
}

TEST_CASE("following and snapping move the same seat cameras reset stood up",
          "[scenes][seat-cameras]") {
    Party party;
    SeatCameras seats;
    seats.reset(party.players, party.subjects, {}, CameraRange{}, CameraView{});
    TowerCamera expected = resetOn(std::span<const CameraSubject>{party.subjects}.subspan(1, 1));
    party.subjects[1].feet.x = 14;
    party.subjects[1].follow.x = 14;
    seats.follow(party.players, party.subjects, {}, CameraRange{}, CameraView{}, 0.5f);
    const auto moved = std::span<const CameraSubject>{party.subjects}.subspan(1, 1);
    expected.update(moved, {}, CameraRange{}, CameraView{}, 0.5f);
    CHECK(seats.camera(2).localState() == expected.localState());
    seats.snap(party.players, party.subjects, CameraRange{});
    expected.snapAttention(moved, CameraRange{});
    CHECK(seats.camera(2).localState() == expected.localState());
}

TEST_CASE("the seat shown is the view seat while in the party, else the lowest standing",
          "[scenes][seat-cameras]") {
    Party party;
    SeatCameras seats;
    CHECK(seats.presented(party.players) == 0);
    seats.setViewSeat(3);
    CHECK(seats.presented(party.players) == 3);
    // The view seat is shown even when it has fallen; a seat nobody plays is not.
    party.players[2].life = PlayerLife::Dying;
    CHECK(seats.presented(party.players) == 3);
    seats.setViewSeat(1);
    party.players[0].life = PlayerLife::InTower;
    CHECK(seats.presented(party.players) == 2);
    for (auto& player : party.players) {
        player.life = PlayerLife::InTower;
    }
    CHECK(seats.presented(party.players) == -1);
    CHECK(seats.presented({}) == -1);
    // Out-of-range seats are clamped when asked for a camera.
    CHECK(&seats.camera(-5) == &seats.camera(0));
    CHECK(&seats.camera(9) == &seats.camera(3));
}

TEST_CASE("the seat cameras' snapshot restores them and their digest is each camera's in turn",
          "[scenes][seat-cameras]") {
    Party party;
    SeatCameras seats;
    seats.reset(party.players, party.subjects, {}, CameraRange{}, CameraView{});
    const SeatCameras::States saved = seats.states();
    party.subjects[0].feet.z = 30;
    party.subjects[0].follow.z = 30;
    seats.follow(party.players, party.subjects, {}, CameraRange{}, CameraView{}, 1.0f);
    REQUIRE(seats.states() != saved);
    seats.restore(saved);
    CHECK(seats.states() == saved);

    NativeStateDigest together;
    REQUIRE(seats.appendNativeDigest(together));
    NativeStateDigest oneByOne;
    for (s32 seat = 0; seat < static_cast<s32>(SeatCameras::kSeats); ++seat) {
        REQUIRE(seats.camera(seat).appendNativeDigest(oneByOne));
    }
    CHECK(together.finish() == oneByOne.finish());
}
