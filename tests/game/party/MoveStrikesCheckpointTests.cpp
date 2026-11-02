#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/combat/StrikeHit.h"
#include "game/party/MoveStrikes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
static_assert(!std::is_copy_assignable_v<MoveStrikes>);

TEST_CASE("native strike values rewind births delayed launches reflections and retirement",
          "[rollback][local-rewind][move-strikes-local-rewind]") {
    MoveStrikes uninterrupted;
    MoveStrikes replay;
    MoveStrikes::LocalSnapshot history;
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.delay = 0.1f;
    burst.radius = 12;
    burst.amount = 50;
    MoveStrike flight;
    flight.type = MoveStrike::kFlies;
    flight.flags = 0x840;
    flight.loopEffect = 1;
    flight.hitRadius = 1;
    flight.speed = 20;
    flight.maxTime = 0.75f;
    flight.amount = 70;
    constexpr f32 kSeconds = 1.0f / 30;
    const auto step = [&](MoveStrikes& owner, s32 tick) {
        if (tick % 23 == 0) {
            owner.start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 0.75f);
        }
        if (tick % 17 == 0) {
            owner.start(flight, 3, {0, 5, 0}, {0, 0, 1}, 10, 0.25f);
        }
        if (tick % 13 == 0 && owner.count() > 0) {
            owner.hitPlayer(owner.strike(owner.count() - 1).id, true, {1, 2, 3});
            owner.placeArea(owner.strike(0).id, glm::translate(Mat4{1}, Vec3{7, 8, 9}));
        }
        return owner.update(kSeconds, nullptr);
    };
    s32 frame = 0;
    usize contacts = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 length = 1 + window % 8;
        CAPTURE(window, length, frame);
        replay.saveLocal(history);
        const auto saved = replay.checkpoint();
        REQUIRE(saved);
        std::vector<std::vector<StrikeHit>> expected;
        for (s32 i = 0; i < length; ++i) {
            expected.push_back(step(uninterrupted, frame + i));
            replay.start(flight, 1, {100, 50, 10}, {1, 0, 0}, 500, 3);
            replay.update(kSeconds * 3, nullptr);
            if (replay.count() > 0) {
                replay.stop(replay.strike(0).id);
            }
        }
        REQUIRE(replay.checkpoint() != saved);
        REQUIRE(replay.restoreLocal(history));
        CHECK(replay.checkpoint() == saved);
        for (s32 i = 0; i < length; ++i) {
            const auto actual = step(replay, frame + i);
            CHECK(actual == expected[static_cast<usize>(i)]);
            contacts += actual.size();
        }
        CHECK(replay.checkpoint() == uninterrupted.checkpoint());
        frame += length;
    }
    CHECK(contacts > 100);
    const auto before = uninterrupted.checkpoint();
    CHECK_FALSE(uninterrupted.restoreLocal(history));
    CHECK(uninterrupted.checkpoint() == before);
    replay.clear();
    CHECK_FALSE(replay.canRestoreLocal(history));
    CHECK_FALSE(replay.restoreLocal(history));
    CHECK(replay.count() == 0);
}

TEST_CASE("strike checkpoint resumes area windows delayed launch reflection and exact outputs",
          "[move-strikes-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30, 0.25f);
    MoveStrikes native;
    MoveStrikes replay;
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.delay = 0.25f;
    burst.radius = 12;
    burst.amount = 50;
    const auto area = native.start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 2);
    MoveStrike flight;
    flight.type = MoveStrike::kFlies;
    flight.flags = 0x840;
    flight.loopEffect = 1;
    flight.hitRadius = 1;
    flight.speed = 20;
    flight.maxTime = 6;
    flight.amount = 70;
    const auto shot = native.start(flight, 3, {0, 5, 0}, {0, 0, 1}, 10, 1);
    native.update(0.17f, nullptr);
    const auto saved = native.checkpoint();
    REQUIRE(saved);
    REQUIRE(saved->strikes.size() == 2);
    REQUIRE(saved->strikes[1].launchIn > 0);
    REQUIRE(replay.restore(*saved));
    for (s32 tick = 0; tick < 110; ++tick) {
        CAPTURE(tick, seconds);
        if (tick == 3) {
            const auto placement = glm::translate(Mat4{1}, Vec3{7, 8, 9});
            native.placeArea(area, placement);
            replay.placeArea(area, placement);
        }
        if (tick == 8) {
            native.hitPlayer(shot, true, {1, 2, 3});
            replay.hitPlayer(shot, true, {1, 2, 3});
        }
        const auto expected = native.update(seconds, nullptr);
        const auto actual = replay.update(seconds, nullptr);
        CHECK(actual == expected);
        const auto a = native.checkpoint();
        const auto b = replay.checkpoint();
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a == b);
        if (tick == 20) {
            REQUIRE(native.restore(*saved));
            REQUIRE(replay.restore(*saved));
        }
    }
    CHECK(native.start(burst, 2, {}, {1, 0, 0}, 10, 1) ==
          replay.start(burst, 2, {}, {1, 0, 0}, 10, 1));
}

TEST_CASE("strike restoration validates before mutation and consumes only its owner's preparation",
          "[move-strikes-checkpoint]") {
    MoveStrikes native;
    MoveStrikes other;
    MoveStrike row;
    row.type = MoveStrike::kFlies;
    row.maxTime = 2;
    row.hitRadius = 1;
    row.amount = 10;
    native.start(row, 0, {}, {0, 0, 1}, 1);
    const auto saved = native.checkpoint();
    REQUIRE(saved);
    for (s32 mutation = 0; mutation < 8; ++mutation) {
        auto bad = *saved;
        switch (mutation) {
        case 0: bad.strikes.push_back(bad.strikes[0]); break;
        case 1: bad.strikes[0].owner = 4; break;
        case 2: bad.strikes[0].launchIn = -1; break;
        case 3: bad.strikes[0].radius = std::numeric_limits<f32>::quiet_NaN(); break;
        case 4: bad.strikes[0].facing.y = 0.5f; break;
        case 5: bad.next = bad.strikes[0].id; break;
        case 6: bad.strikes[0].expanding = true; break;
        case 7: bad.strikes.resize(MoveStrikes::Checkpoint::kMaxStrikes + 1); break;
        default: break;
        }
        CHECK_FALSE(native.restore(bad));
        CHECK(native.checkpoint() == saved);
    }
    auto prepared = native.prepareRestore(*saved);
    auto sibling = native.prepareRestore(*saved);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(native.commitRestore(std::move(*prepared)));
    CHECK_FALSE(native.canCommit(*prepared));
    CHECK_FALSE(native.commitRestore(std::move(*sibling)));
    prepared = native.prepareRestore(*saved);
    REQUIRE(prepared);
    native.clear();
    CHECK_FALSE(native.commitRestore(std::move(*prepared)));
    REQUIRE(native.restore(*saved));
}
} // namespace
