#include <array>
#include <limits>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsTowerPortals.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Kind = LegendsTowerPortalKind;

TEST_CASE("Cathedral fog advances through room depth before a completed white handoff",
          "[legends][tower][portal][tower-fog]") {
    const auto opening = legendsCathedralFog(0);
    CHECK(opening.color == Vec3{0});
    CHECK(opening.atDepth(20) == 0);
    const auto whitening = legendsCathedralFog(.2f);
    CHECK(whitening.color == Vec3{1});
    CHECK(whitening.atDepth(20) == 0);
    const auto middle = legendsCathedralFog(1);
    CHECK(middle.atDepth(10) == 0);
    CHECK(middle.atDepth(50) > 0);
    CHECK(middle.atDepth(50) < 1);
    CHECK(middle.atDepth(500) > middle.atDepth(50));
    const auto final = legendsCathedralFog(2);
    CHECK(final.atDepth(2) == 1);
    CHECK(final.atDepth(100) == 1);
    CHECK(final == legendsCathedralFog(100));
    CHECK(opening == legendsCathedralFog(-1));
    CHECK(opening == legendsCathedralFog(std::numeric_limits<f32>::quiet_NaN()));
}

TEST_CASE("Tower completion portals retain native locations and destinations",
          "[legends][tower][portal]") {
    const LegendsTowerPortalMotion shards{Kind::Shards};
    CHECK(shards.definition().position == Vec3{0, 1, -30});
    CHECK(shards.definition().radius == 3);
    CHECK(shards.definition().map == "boss1");
    const LegendsTowerPortalMotion runes{Kind::Runestones};
    CHECK(runes.definition().position == Vec3{1.5f, 0, 14});
    CHECK(runes.definition().radius == 4);
    CHECK(runes.definition().map == "hell1");
    CHECK_THROWS_AS(LegendsTowerPortalMotion(static_cast<Kind>(-1)), std::invalid_argument);
}

TEST_CASE("Tower shard portal charges for one player but waits for the whole party",
          "[legends][tower][portal]") {
    LegendsTowerPortalMotion portal{Kind::Shards};
    const auto centre = portal.definition().position;
    std::array visitors{LegendsPortalVisitor{0, centre},
                        LegendsPortalVisitor{3, centre + Vec3{10, 0, 0}}};
    REQUIRE_FALSE(portal.advance(2, visitors, true));
    CHECK(portal.charge() == 1);
    CHECK(portal.occupied() == 1);
    REQUIRE_FALSE(portal.advance(2, visitors, true));
    CHECK(portal.dwell() == 0);
    visitors[1].position = centre;
    REQUIRE_FALSE(portal.advance(1.5f, visitors, true));
    CHECK(portal.dwell() == 1.5f);
    CHECK(portal.occupied() == 9);
    REQUIRE(portal.advance(.001f, visitors, true));
    CHECK(portal.departed());
    CHECK_FALSE(portal.advance(10, visitors, true));
    CHECK_FALSE(portal.advance(1, visitors, false));
    CHECK(portal.departed());
}

TEST_CASE("Tower runestone portal requires everyone for charging and resets interrupted dwell",
          "[legends][tower][portal]") {
    LegendsTowerPortalMotion portal{Kind::Runestones};
    const auto centre = portal.definition().position;
    std::array visitors{LegendsPortalVisitor{0, centre},
                        LegendsPortalVisitor{1, centre + Vec3{10, 0, 0}}};
    REQUIRE_FALSE(portal.advance(2, visitors, true));
    CHECK(portal.charge() == 0);
    visitors[1].position = centre;
    REQUIRE_FALSE(portal.advance(1, visitors, true));
    CHECK(portal.charge() == .5f);
    visitors[1].position.x += 10;
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.charge() == .25f);
    visitors[1].position = centre;
    REQUIRE_FALSE(portal.advance(1.5f, visitors, true));
    CHECK(portal.charge() == 1);
    CHECK(portal.dwell() == 0);
    REQUIRE_FALSE(portal.advance(1, visitors, true));
    visitors[1].position.x += 10;
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.charge() == .75f);
    CHECK(portal.dwell() == 0);
    visitors[1].position = centre;
    REQUIRE_FALSE(portal.advance(.5f, visitors, true));
    REQUIRE_FALSE(portal.advance(1.5f, visitors, true));
    REQUIRE(portal.advance(.01f, visitors, true));
}

TEST_CASE("Tower portal contact is strict horizontal centre distance rather than body contact",
          "[legends][tower][portal]") {
    for (const auto kind : {Kind::Shards, Kind::Runestones}) {
        LegendsTowerPortalMotion portal{kind};
        const auto centre = portal.definition().position;
        std::array visitors{LegendsPortalVisitor{2, centre}};
        visitors[0].position.x += portal.definition().radius;
        visitors[0].radius = 10;
        REQUIRE_FALSE(portal.advance(2, visitors, true));
        CHECK(portal.occupied() == 0);
        CHECK(portal.charge() == 0);
        visitors[0].position.x -= .001f;
        visitors[0].position.y += 100;
        REQUIRE_FALSE(portal.advance(2, visitors, true));
        CHECK(portal.occupied() == 4);
        CHECK(portal.charge() == 1);
        visitors[0].alive = false;
        REQUIRE_FALSE(portal.advance(2, visitors, true));
        CHECK(portal.occupied() == 0);
        CHECK(portal.charge() == 0);
        CHECK(portal.dwell() == 0);
    }
}

TEST_CASE("The rune fire circle rises for any visitor independently of unanimous readiness",
          "[legends][tower][portal][tower-special-portals]") {
    LegendsTowerPortalMotion portal{Kind::Runestones};
    const auto centre = portal.definition().position;
    std::array visitors{LegendsPortalVisitor{0, centre},
                        LegendsPortalVisitor{3, centre + Vec3{10, 0, 0}}};
    CHECK_FALSE(portal.fireCreated());
    CHECK(portal.fireHeight() == -40);
    portal.capturePresentation();
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.fireCreated());
    CHECK(portal.fireHeight() == -20);
    CHECK(portal.fireHeight(0) == -40);
    CHECK(portal.fireHeight(.5f) == -30);
    CHECK(portal.charge() == 0);
    portal.capturePresentation();
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.fireHeight() == 0);
    visitors[0].position.x += 10;
    portal.capturePresentation();
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.fireHeight() == -20);
    REQUIRE_FALSE(portal.advance(.5f, visitors, true));
    CHECK(portal.fireHeight() == -40);
    CHECK(portal.fireCreated());
    visitors[0].position = centre;
    REQUIRE_FALSE(portal.advance(.5f, visitors, true));
    CHECK(portal.fireHeight() == 0);
    REQUIRE_FALSE(portal.advance(.1f, visitors, false));
    CHECK_FALSE(portal.fireCreated());
    CHECK(portal.fireHeight() == -40);
}

TEST_CASE("Cathedral contact illumination interpolates without advancing the native readiness",
          "[legends][tower][portal][tower-special-portals]") {
    LegendsTowerPortalMotion portal{Kind::Shards};
    std::array visitors{LegendsPortalVisitor{0, portal.definition().position}};
    portal.capturePresentation();
    REQUIRE_FALSE(portal.advance(1, visitors, true));
    CHECK(portal.charge(0) == 0);
    CHECK(portal.charge(.5f) == .25f);
    CHECK(portal.charge(1) == .5f);
    CHECK(portal.charge() == .5f);
    CHECK(portal.dwell() == 0);
    visitors[0].position.x += 10;
    portal.capturePresentation();
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.charge(.5f) == .375f);
    CHECK(portal.charge() == .25f);
    REQUIRE_FALSE(portal.advance(.25f, visitors, true));
    CHECK(portal.charge() == 0);
}

TEST_CASE("Tower completion portal charge and dwell run on elapsed simulation time",
          "[legends][tower][portal]") {
    for (const auto kind : {Kind::Shards, Kind::Runestones}) {
        for (const s32 frequency : {30, 60, 144}) {
            CAPTURE(kind, frequency);
            LegendsTowerPortalMotion portal{kind};
            std::array visitors{LegendsPortalVisitor{0, portal.definition().position},
                                LegendsPortalVisitor{1, portal.definition().position},
                                LegendsPortalVisitor{2, portal.definition().position},
                                LegendsPortalVisitor{3, portal.definition().position}};
            s32 requests = 0;
            f32 requestTime = 0;
            for (s32 tick = 0; tick < frequency * 5; ++tick) {
                if (portal.advance(1.0f / static_cast<f32>(frequency), visitors, true)) {
                    ++requests;
                    requestTime = static_cast<f32>(tick + 1) / static_cast<f32>(frequency);
                }
            }
            CHECK(requests == 1);
            CHECK(requestTime >= 3.5f - .0001f);
            CHECK(requestTime <= 3.5f + 2.0f / static_cast<f32>(frequency) + .0001f);
        }
    }
}

TEST_CASE("Tower portals reject malformed rosters and cannot depart when disabled or vacant",
          "[legends][tower][portal]") {
    LegendsTowerPortalMotion portal{Kind::Shards};
    std::array visitors{LegendsPortalVisitor{0, portal.definition().position},
                        LegendsPortalVisitor{1, portal.definition().position}};
    for (const f32 invalid : {0.0f, -1.0f, 11.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(portal.advance(invalid, visitors, true));
        CHECK(portal.charge() == 0);
    }
    CHECK_FALSE(portal.advance(2, visitors, false));
    CHECK(portal.charge() == 0);
    REQUIRE_FALSE(portal.advance(2, visitors, true));
    REQUIRE_FALSE(portal.advance(1, visitors, true));
    REQUIRE_FALSE(portal.advance(.1f, visitors, false));
    CHECK(portal.charge() == 0);
    CHECK(portal.dwell() == 0);
    CHECK(portal.occupied() == 0);
    REQUIRE_FALSE(portal.advance(10, {}, true));
    CHECK(portal.charge() == 0);
    visitors[1].player = 0;
    REQUIRE_FALSE(portal.advance(10, visitors, true));
    CHECK(portal.occupied() == 0);
    visitors[1].player = 4;
    REQUIRE_FALSE(portal.advance(10, visitors, true));
    CHECK(portal.occupied() == 0);
    visitors[1].player = 1;
    visitors[1].position.x = std::numeric_limits<f32>::quiet_NaN();
    REQUIRE_FALSE(portal.advance(10, visitors, true));
    CHECK(portal.occupied() == 0);
    CHECK_FALSE(portal.departed());
}

} // namespace
