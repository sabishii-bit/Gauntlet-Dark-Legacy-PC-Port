#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** B6's SAFEROCK definition. */
formats::ItemInfoRecord safeRock() {
    formats::ItemInfoRecord info;
    info.type = 10;
    info.subtype = 41;
    info.name = "SAFEROCK";
    info.collisionType = 1;
    info.radius = 2.3f;
    info.height = 5;
    info.hitPoints = 40;
    info.armor = 10;
    return info;
}

/** A placement of three standing segments. */
formats::ItemInstanceRecord placement() {
    formats::ItemInstanceRecord instance;
    instance.position = {4, 1, -9};
    instance.rotation.y = .5f;
    test::setU16LE(instance.params, 2, 3);
    return instance;
}

TEST_CASE("Dreamcast safe rocks stand their placed segments as arena cover",
          "[legends][dreamcast-arena-cover]") {
    const auto admission = legendsDreamcastArenaRock(safeRock(), placement(), 0);
    REQUIRE(admission.rock);
    CHECK(admission.diagnostic.empty());
    CHECK_FALSE(admission.ordinary);
    const auto& rock = *admission.rock;
    CHECK(rock.body.position == Vec3{4, 1, -9});
    CHECK(rock.body.yaw == .5f);
    CHECK(rock.body.radius == 2.3f);
    CHECK(rock.body.height == 5);
    CHECK(rock.body.tierHealth == 40);
    CHECK(rock.tier == 3);
    CHECK(rock.armor == 10);
    CHECK_FALSE(rock.dormant);
    auto hidden = placement();
    hidden.flags = 2;
    const auto dormant = legendsDreamcastArenaRock(safeRock(), hidden, 0).rock;
    REQUIRE(dormant);
    CHECK(dormant->dormant);
    // The roster's last place is still a place.
    CHECK(legendsDreamcastArenaRock(safeRock(), placement(), kMostDreamcastArenaRocks - 1).rock);

    SECTION("the placement's own positive subtype decides what it is") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        auto overridden = placement();
        test::setU16LE(overridden.params, 0, 41);
        CHECK(legendsDreamcastArenaRock(barrel, overridden, 0).rock);
        test::setU16LE(overridden.params, 0, 43);
        const auto other = legendsDreamcastArenaRock(safeRock(), overridden, 0);
        CHECK_FALSE(other.rock);
        CHECK(other.ordinary);
        CHECK(other.diagnostic.empty());
    }
    SECTION("what is no safe rock is left to the ordinary fixtures without a report") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        auto chest = safeRock();
        chest.type = 2;
        for (const auto& info : {barrel, chest}) {
            CAPTURE(info.type, info.subtype);
            const auto other = legendsDreamcastArenaRock(info, placement(), 0);
            CHECK_FALSE(other.rock);
            CHECK(other.ordinary);
            CHECK(other.diagnostic.empty());
        }
    }
}

TEST_CASE("Dreamcast safe rocks the arena cannot stand are reported",
          "[legends][dreamcast-arena-cover]") {
    constexpr auto kSegments = "Dreamcast arena rock has an unsupported segment count";
    constexpr auto kDurability = "Dreamcast arena rock has unsupported durability";
    constexpr auto kBody = "Dreamcast arena rock has an unsupported collision body";
    struct Case {
        const char* what;
        formats::ItemInfoRecord info;
        formats::ItemInstanceRecord instance;
        std::string diagnostic;
    };
    std::vector<Case> cases;
    const auto add = [&](const char* what, auto change, std::string diagnostic) {
        Case entry{what, safeRock(), placement(), std::move(diagnostic)};
        change(entry.info, entry.instance);
        cases.push_back(std::move(entry));
    };
    add(
        "more segments than models",
        [](auto&, auto& instance) { test::setU16LE(instance.params, 2, 4); }, kSegments);
    add(
        "a negative segment count",
        [](auto&, auto& instance) { test::setU16LE(instance.params, 2, 0xffffU); }, kSegments);
    add("no health", [](auto& info, auto&) { info.hitPoints = 0; }, kDurability);
    add("implausible health", [](auto& info, auto&) { info.hitPoints = 10001; }, kDurability);
    add("a box body", [](auto& info, auto&) { info.collisionType = 3; }, kBody);
    add("no radius", [](auto& info, auto&) { info.radius = 0; }, kBody);
    add(
        "a nonfinite height",
        [](auto& info, auto&) { info.height = std::numeric_limits<f32>::quiet_NaN(); }, kBody);
    add("an offset body", [](auto& info, auto&) { info.collisionOffset = {0, 1, 0}; }, kBody);
    for (const auto& entry : cases) {
        CAPTURE(entry.what);
        // Reported, and never left to the ordinary fixtures as scenery.
        const auto admission = legendsDreamcastArenaRock(entry.info, entry.instance, 0);
        CHECK_FALSE(admission.rock);
        CHECK_FALSE(admission.ordinary);
        CHECK(admission.diagnostic == entry.diagnostic);
    }
}

TEST_CASE("Dreamcast safe rocks past the boss's roster are reported and stay ordinary items",
          "[legends][dreamcast-arena-cover]") {
    constexpr auto kRoster = "Dreamcast arena has more safe rocks than the boss's 16";
    const auto past = legendsDreamcastArenaRock(safeRock(), placement(), kMostDreamcastArenaRocks);
    CHECK_FALSE(past.rock);
    CHECK(past.ordinary);
    CHECK(past.diagnostic == kRoster);
    // Outside the roster a rock's own fields no longer matter: the arena never stands it.
    auto spent = safeRock();
    spent.hitPoints = 0;
    const auto unsupported =
        legendsDreamcastArenaRock(spent, placement(), kMostDreamcastArenaRocks + 3);
    CHECK_FALSE(unsupported.rock);
    CHECK(unsupported.ordinary);
    CHECK(unsupported.diagnostic == kRoster);
}

} // namespace
