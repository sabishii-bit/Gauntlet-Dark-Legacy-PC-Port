#include <array>
#include <bit>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/rollback/PlayerActionCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using State = PlayerAnimator::Checkpoint;
using Action = PlayerAnimator::Action;

static_assert(std::is_trivially_copyable_v<State>);

constexpr std::array kFlags{
    &State::entered,     &State::released,    &State::quickPress,     &State::strongPress,
    &State::quickHeld,   &State::strongHeld,  &State::moved,          &State::shielded,
    &State::pushed,      &State::potionUsed,  &State::potionThrown,   &State::potionThrowReleased,
    &State::dead,        &State::turboBegan,  &State::strongReleased, &State::potionShielded,
    &State::shieldAsked, &State::legendAsked, &State::legendReleased, &State::superReleased,
    &State::superHeld,   &State::rapid,       &State::speed,          &State::comboRide,
    &State::potionLatch};

State populated() {
    State state;
    state.animation.sequence = 37;
    state.animation.generation = 0x0123456789ABCDEFULL;
    state.animation.secondsPerFrame = 0.025f;
    state.animation.time = 0.12345678901234567;
    state.animation.frame = 4.125f;
    state.animation.speed = 1.3333334f;
    state.animation.transitionLength = 0.125f;
    state.animation.transitionTime = 0.0625f;
    state.animation.playing = true;
    state.animation.finished = true;
    state.animation.held = true;
    state.animation.smooth = true;
    state.action = Action::StrongThrow;
    state.footfall = PlayerAnimator::Foot::Second;
    state.meleeBlow = MeleeBlow::Power;
    state.melee = {MeleeRange::Step, true, -2.25f};
    state.character = 5;
    state.chain = 9;
    state.potionThrowTicks = 97;
    state.comboClass = 7;
    state.stillTicks = 375;
    state.fidgetTicks = 173;
    state.itemReleased = PlayerDeed::Breathe;
    state.strafe = StrafeWay::Left;
    state.guardArmor = 3.25f;
    state.attackSeconds = 0.075f;
    state.phaseSeconds = 0.9876543210987654;
    state.shotFrame = 3.25f;
    for (const auto flag : kFlags) {
        state.*flag = true;
    }
    return state;
}

std::vector<u8> encode(const State& state) {
    const auto encoded = PlayerActionCheckpoint::encode(state);
    REQUIRE(encoded);
    REQUIRE(encoded->size() == PlayerActionCheckpoint::kBytes);
    return *encoded;
}

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}

TEST_CASE("native player checkpoint wire retains every clock, latch and non-default field",
          "[netplay][rollback][action-checkpoint]") {
    const auto state = populated();
    const auto bytes = encode(state);
    CHECK(bytes[0] == 37);
    CHECK(bytes[4] == 0xEF);
    CHECK(bytes[11] == 0x01);
    CHECK(bytes[120] == 0xFF);
    CHECK(bytes[123] == 0x01);
    const auto decoded = PlayerActionCheckpoint::decode(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(encode(*decoded) == bytes);
    for (const auto flag : kFlags) {
        auto changed = state;
        changed.*flag = false;
        const auto packed = encode(changed);
        CHECK(packed != bytes);
        const auto restored = PlayerActionCheckpoint::decode(packed);
        REQUIRE(restored);
        CHECK(*restored == changed);
    }
    const State empty;
    const auto decodedEmpty = PlayerActionCheckpoint::decode(encode(empty));
    REQUIRE(decodedEmpty);
    CHECK(*decodedEmpty == empty);
}

TEST_CASE("native player checkpoint wire rejects truncated, widened and unknown fields",
          "[netplay][rollback][action-checkpoint]") {
    const auto bytes = encode(populated());
    for (usize size = 0; size < bytes.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(PlayerActionCheckpoint::decode(std::span{bytes}.first(size)));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(PlayerActionCheckpoint::decode(trailing));
    // Each enum's own upper bound and an out-of-range u32 must be rejected,
    // before narrowing to its u8 representation could turn 256 into zero.
    constexpr std::array<std::pair<usize, u32>, 6> kEnums{{
        {44, static_cast<u32>(PlayerAnimator::kActionCount)},
        {48, static_cast<u32>(PlayerAnimator::Foot::Second) + 1},
        {52, static_cast<u32>(MeleeBlow::Power) + 1},
        {56, static_cast<u32>(MeleeRange::Swing) + 1},
        {92, static_cast<u32>(kPlayerDeedCount)},
        {96, static_cast<u32>(StrafeWay::Right) + 1},
    }};
    for (const auto [offset, invalid] : kEnums) {
        for (const u32 value : {invalid, 256U, 0xFFFFFFFFU}) {
            CAPTURE(offset, value);
            auto changed = bytes;
            word(changed, offset, value);
            CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
        }
    }
    for (const auto [offset, invalid] :
         std::array<std::pair<usize, u32>, 3>{{{40, 0x10}, {60, 2}, {120, 1U << 25U}}}) {
        auto changed = bytes;
        word(changed, offset, invalid);
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    for (const usize offset : {12U, 24U, 28U, 32U, 36U, 64U, 100U, 104U, 116U}) {
        auto changed = bytes;
        word(changed, offset, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    for (const usize offset : {16U, 108U}) {
        auto changed = bytes;
        word(changed, offset, 0);
        word(changed, offset + 4, 0x7FF80000U);
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    for (const usize offset : {12U, 24U, 28U, 32U, 64U, 100U, 104U, 116U}) {
        auto changed = bytes;
        word(changed, offset, std::bit_cast<u32>(std::numeric_limits<f32>::max()));
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    for (const usize offset : {16U, 108U}) {
        auto changed = bytes;
        const auto huge = std::bit_cast<u64>(std::numeric_limits<f64>::max());
        word(changed, offset, static_cast<u32>(huge));
        word(changed, offset + 4, static_cast<u32>(huge >> 32U));
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    for (const usize offset : {72U, 76U, 84U, 88U}) {
        auto changed = bytes;
        word(changed, offset, static_cast<u32>(std::numeric_limits<s32>::max()));
        CHECK_FALSE(PlayerActionCheckpoint::decode(changed));
    }
    auto overflowingGeneration = bytes;
    word(overflowingGeneration, 4, 0xFFFFFFFF);
    word(overflowingGeneration, 8, 0xFFFFFFFF);
    CHECK_FALSE(PlayerActionCheckpoint::decode(overflowingGeneration));
    auto invalid = populated();
    invalid.animation.secondsPerFrame = -1;
    CHECK_FALSE(PlayerActionCheckpoint::encode(invalid));
    invalid = populated();
    invalid.phaseSeconds = std::numeric_limits<f64>::infinity();
    CHECK_FALSE(PlayerActionCheckpoint::encode(invalid));
}

TEST_CASE("wire-restored native actions replay releases and guards exactly at subframe boundaries",
          "[netplay][rollback][action-checkpoint]") {
    TreeInfo tree;
    tree.name = "WAR";
    tree.nodes.emplace_back();
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 9;
        sequence.frameRate = 24;
        tree.sequences.push_back(sequence);
    }
    constexpr std::array kDeeds{PlayerDeed::Attack, PlayerDeed::StrongAttack, PlayerDeed::Defend,
                                PlayerDeed::None};
    const auto request = [&](s32 tick) {
        return kDeeds[static_cast<usize>(tick / 65) % kDeeds.size()];
    };
    PlayerAnimator original;
    REQUIRE(original.bind(tree, false));
    original.setCharacter(0);
    original.setGuardArmor(2.5f);
    original.setAttackSpeed(true, false);
    s32 ordinaryReleases = 0;
    s32 strongReleases = 0;
    s32 guardFrames = 0;
    for (s32 tick = 0; tick < 400; ++tick) {
        CAPTURE(tick);
        const auto decoded = PlayerActionCheckpoint::decode(encode(original.checkpoint()));
        REQUIRE(decoded);
        PlayerAnimator replayed;
        REQUIRE(replayed.bind(tree, false));
        REQUIRE(replayed.restore(*decoded));
        auto expected = original;
        for (s32 future = 0; future < 24; ++future) {
            expected.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, request(tick + future));
            replayed.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, request(tick + future));
            REQUIRE(replayed.checkpoint() == expected.checkpoint());
        }
        original.update(PlayerMotion::Stand, 1, 1.0f / 60.0f, request(tick));
        ordinaryReleases += original.released() ? 1 : 0;
        strongReleases += original.strongReleased() ? 1 : 0;
        guardFrames += original.defending() ? 1 : 0;
    }
    CHECK(ordinaryReleases > 0);
    CHECK(strongReleases > 0);
    CHECK(guardFrames > 0);
}
} // namespace
