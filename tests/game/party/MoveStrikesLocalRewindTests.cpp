#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "NativeStateSupport.h"
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
        const auto saved = test::liveDigest(replay);
        std::vector<std::vector<StrikeHit>> expected;
        for (s32 i = 0; i < length; ++i) {
            expected.push_back(step(uninterrupted, frame + i));
            replay.start(flight, 1, {100, 50, 10}, {1, 0, 0}, 500, 3);
            replay.update(kSeconds * 3, nullptr);
            if (replay.count() > 0) {
                replay.stop(replay.strike(0).id);
            }
        }
        REQUIRE(test::liveDigest(replay) != saved);
        REQUIRE(replay.restoreLocal(history));
        CHECK(test::liveDigest(replay) == saved);
        for (s32 i = 0; i < length; ++i) {
            const auto actual = step(replay, frame + i);
            CHECK(actual == expected[static_cast<usize>(i)]);
            contacts += actual.size();
        }
        CHECK(test::liveDigest(replay) == test::liveDigest(uninterrupted));
        frame += length;
    }
    CHECK(contacts > 100);
    const auto before = test::liveDigest(uninterrupted);
    CHECK_FALSE(uninterrupted.restoreLocal(history));
    CHECK(test::liveDigest(uninterrupted) == before);
    replay.clear();
    CHECK_FALSE(replay.canRestoreLocal(history));
    CHECK_FALSE(replay.restoreLocal(history));
    CHECK(replay.count() == 0);
}

TEST_CASE("strike rewind resumes area windows delayed launch reflection and exact outputs",
          "[move-strikes-local-rewind]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30, 0.25f);
    MoveStrikes native;
    MoveStrikes replay;
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.delay = 0.25f;
    burst.radius = 12;
    burst.amount = 50;
    MoveStrike flight;
    flight.type = MoveStrike::kFlies;
    flight.flags = 0x840;
    flight.loopEffect = 1;
    flight.hitRadius = 1;
    flight.speed = 20;
    flight.maxTime = 6;
    flight.amount = 70;
    u32 area = 0;
    u32 shot = 0;
    for (auto* owner : {&native, &replay}) {
        area = owner->start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 2);
        shot = owner->start(flight, 3, {0, 5, 0}, {0, 0, 1}, 10, 1);
        owner->update(0.17f, nullptr);
    }
    REQUIRE(native.count() == 2);
    REQUIRE(native.strike(1).launchIn > 0);
    MoveStrikes::LocalSnapshot nativeSaved;
    MoveStrikes::LocalSnapshot replaySaved;
    native.saveLocal(nativeSaved);
    replay.saveLocal(replaySaved);
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
        CHECK(test::liveDigest(native) == test::liveDigest(replay));
        if (tick == 20) {
            REQUIRE(native.restoreLocal(nativeSaved));
            REQUIRE(replay.restoreLocal(replaySaved));
        }
    }
    CHECK(native.start(burst, 2, {}, {1, 0, 0}, 10, 1) ==
          replay.start(burst, 2, {}, {1, 0, 0}, 10, 1));
}
} // namespace
