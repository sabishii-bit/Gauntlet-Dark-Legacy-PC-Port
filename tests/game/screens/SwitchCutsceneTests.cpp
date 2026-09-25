#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/screens/SwitchCutscene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

WorldLayout cameraLayout() {
    const auto dir = test::scratchDirectory("switch-cameras");
    writeTextFile(dir / "world.json", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "locators":[
        {"type":"triggerCamera","next":7,"delay":20,
         "position":[1,2,3],"rotation":[0.4,1.2,0.9]},
        {"type":"triggerCamera","next":8,"delay":0,
         "position":[4,5,6],"rotation":[0.2,0.3,0]},
        {"type":"triggerCamera","next":198,"delay":20,
         "position":[7,8,9],"rotation":[0,0,0]},
        {"type":"triggerCamera","next":8,"delay":0,
         "position":[10,11,12],"rotation":[0.6,0.7,0.8]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    return layout;
}

TEST_CASE("switch shots use matching authored markers and ignore marker roll", "[switch-camera]") {
    const auto layout = cameraLayout();
    SwitchCutscene cut;
    CHECK_FALSE(cut.begin({6, 42}, layout, false));
    CHECK_FALSE(cut.active());
    REQUIRE(cut.begin({7, 42}, layout, false));
    CHECK(cut.active());
    CHECK_FALSE(cut.showing());
    CHECK(cut.target() == 42);
    REQUIRE(cut.camera());
    CHECK(cut.camera()->position == Vec3{1, 2, 3});
    CHECK(cut.camera()->yaw == Approx(1.2f));
    CHECK(cut.camera()->pitch == Approx(0.4f));
    CHECK(cut.camera()->roll == Approx(0));
    CHECK_FALSE(cut.begin({99, 11}, layout, false));
    CHECK(cut.target() == 42); // an unlinked switch does not cancel an existing cut
    REQUIRE(cut.begin({8, 12}, layout, false));
    CHECK(cut.camera()->position == Vec3{10, 11, 12}); // last registration wins
    CHECK(cut.target() == 12);
    cut.clear();
    CHECK_FALSE(cut.active());
    CHECK_FALSE(cut.showing());
    CHECK(cut.target() == -1);
    CHECK_FALSE(cut.begin({198, 0}, layout, true));
    CHECK(cut.begin({198, 0}, layout, false)); // reserved only in the tower
}

TEST_CASE("switch shots hold their lead-in then wait for both duration and moving target",
          "[switch-camera]") {
    const auto layout = cameraLayout();
    SwitchCutscene cut;
    REQUIRE(cut.begin({7, 42}, layout, false));
    for (int tick = 0; tick < 30; ++tick) {
        cut.update(1, true);
        CHECK(cut.active());
        CHECK_FALSE(cut.showing());
    }
    cut.update(1, true);
    CHECK(cut.showing());
    cut.update(118, true);
    CHECK(cut.showing());
    cut.update(1, false); // the 120-tick shot expired, but its wall is still moving
    CHECK(cut.showing());
    cut.update(100, false);
    CHECK(cut.showing());
    cut.update(1, true);
    CHECK_FALSE(cut.active());

    REQUIRE(cut.begin({8, -1}, layout, false));
    cut.update(30, true);
    cut.update(39, true);
    CHECK(cut.showing());
    cut.update(1, true); // zero marker duration uses forty ticks, not zero
    CHECK_FALSE(cut.active());
}
} // namespace
