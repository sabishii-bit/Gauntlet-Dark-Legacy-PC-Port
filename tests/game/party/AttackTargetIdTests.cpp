#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/level/ImportedTargets.h"
#include "game/party/AttackTargetId.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kPastTheSpans = kImportedTargetBase + kImportedTargetCount;

TEST_CASE("every family keeps its span of target ids", "[game][party][target-id]") {
    struct Row {
        TargetFamily family;
        s32 base;
        s32 end;
    };
    constexpr std::array<Row, 14> kRows{{
        {TargetFamily::Barrel, 0, 1000},
        {TargetFamily::Enemy, 1000, 2000},
        {TargetFamily::Generator, 2000, 3000},
        {TargetFamily::Critter, 3000, 4000},
        {TargetFamily::Boss, 4000, 5000},
        {TargetFamily::SafeRock, 5000, 6000},
        {TargetFamily::Wall, 6000, 7000},
        {TargetFamily::Switch, 7000, 7500},
        {TargetFamily::ItemStop, 7500, 8000},
        {TargetFamily::Potion, 8000, 9000},
        {TargetFamily::Chest, 9000, 9500},
        {TargetFamily::Statue, 9500, 10000},
        {TargetFamily::Player, 10000, 20000},
        {TargetFamily::Imported, 20000, 52768},
    }};
    REQUIRE(AttackTargetId::kSpans.size() == kRows.size());
    for (usize at = 0; at < kRows.size(); ++at) {
        CAPTURE(at);
        CHECK(AttackTargetId::kSpans[at].family == kRows[at].family);
        CHECK(AttackTargetId::kSpans[at].base == kRows[at].base);
        CHECK(AttackTargetId::kSpans[at].end == kRows[at].end);
    }
}

TEST_CASE("the target spans join end to end, from nought to past the imported fixtures",
          "[game][party][target-id]") {
    s32 next = 0;
    for (const AttackTargetId::Span& span : AttackTargetId::kSpans) {
        CAPTURE(span.base);
        CHECK(span.base == next);
        CHECK(span.end > span.base);
        next = span.end;
    }
    CHECK(next == kPastTheSpans);
}

TEST_CASE("an id outside the spans names no family and no index", "[game][party][target-id]") {
    for (const s32 id : {-1, kPastTheSpans, kPastTheSpans + 1, std::numeric_limits<s32>::min(),
                         std::numeric_limits<s32>::max()}) {
        CAPTURE(id);
        const AttackTargetId target = AttackTargetId::fromId(id);
        CHECK(target.family() == TargetFamily::None);
        CHECK(target.index() == -1);
        CHECK(target.id() == id);
        CHECK_FALSE(target.creature());
    }
    CHECK(AttackTargetId{}.family() == TargetFamily::None);
    CHECK(AttackTargetId{}.id() == -1);
    STATIC_CHECK(AttackTargetId::fromId(-1).family() == TargetFamily::None);
    STATIC_CHECK(AttackTargetId::fromId(kPastTheSpans).family() == TargetFamily::None);
}

TEST_CASE("each family's first and last ids come back as its family and index",
          "[game][party][target-id]") {
    for (const AttackTargetId::Span& span : AttackTargetId::kSpans) {
        CAPTURE(span.base);
        for (const s32 index : {0, span.end - span.base - 1}) {
            CAPTURE(index);
            const AttackTargetId target = AttackTargetId::of(span.family, index);
            CHECK(target.id() == span.base + index);
            CHECK(AttackTargetId::fromId(span.base + index).family() == span.family);
            CHECK(AttackTargetId::fromId(span.base + index).index() == index);
        }
    }
    STATIC_CHECK(AttackTargetId::of(TargetFamily::Statue, 3).id() == 9503);
    STATIC_CHECK(AttackTargetId::fromId(52767).index() == kImportedTargetCount - 1);
}

TEST_CASE("only the enemies, the great ones and the boss are creatures",
          "[game][party][target-id]") {
    for (const AttackTargetId::Span& span : AttackTargetId::kSpans) {
        CAPTURE(span.base);
        const bool creature = span.family == TargetFamily::Enemy ||
                              span.family == TargetFamily::Critter ||
                              span.family == TargetFamily::Boss;
        CHECK(AttackTargetId::fromId(span.base).creature() == creature);
        CHECK(AttackTargetId::fromId(span.end - 1).creature() == creature);
    }
}

TEST_CASE("a target's key is its id as the charge ledger spells it", "[game][party][target-id]") {
    for (const s32 id : {0, 999, 1000, 5000, 6003, 19999, 20000, 52767, -1}) {
        CAPTURE(id);
        CHECK(AttackTargetId::fromId(id).key() == static_cast<usize>(id));
    }
}

TEST_CASE("naming a target neither checks nor clamps its index", "[game][party][target-id]") {
    // The inert stops are numbered on past their span unchecked: the five hundredth is a potion.
    const AttackTargetId stop = AttackTargetId::of(TargetFamily::ItemStop, 500);
    CHECK(stop.id() == 8000);
    CHECK(stop.family() == TargetFamily::Potion);
    CHECK(stop.index() == 0);
    CHECK(AttackTargetId::of(TargetFamily::Enemy, -1).family() == TargetFamily::Barrel);
    CHECK(AttackTargetId::of(TargetFamily::Barrel, -1).family() == TargetFamily::None);
    CHECK(AttackTargetId::of(TargetFamily::None, 3).id() == -1);
}

} // namespace
