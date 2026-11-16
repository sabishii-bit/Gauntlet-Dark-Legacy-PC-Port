#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "FakeGameServices.h"
#include "FakeHudStyles.h"
#include "game/online/OnlineLinks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_aggregate_v<OnlineLinks>);

TEST_CASE("online links hold no owner by default and carry the owners they are given",
          "[game][online][online-links]") {
    const OnlineLinks none;
    CHECK(none.haptics == nullptr);
    CHECK(none.unlocks == nullptr);
    CHECK(none.store == nullptr);
    CHECK(none.hudStyles == nullptr);
    test::RecordingHaptics haptics;
    test::RecordingUnlocks unlocks;
    test::RecordingPartyStore store;
    test::FakeHudStyles styles;
    const OnlineLinks links{&haptics, &unlocks, &store, &styles};
    CHECK(links.haptics == &haptics);
    CHECK(links.unlocks == &unlocks);
    CHECK(links.store == &store);
    CHECK(links.hudStyles == &styles);
    // The owners given before the HUD's looks keep their places.
    const OnlineLinks withoutLooks{&haptics, &unlocks, &store};
    CHECK(withoutLooks.store == &store);
    CHECK(withoutLooks.hudStyles == nullptr);
    const OnlineLinks rumbleOnly{.haptics = &haptics};
    CHECK(rumbleOnly.haptics == &haptics);
    CHECK(rumbleOnly.unlocks == nullptr);
    CHECK(rumbleOnly.store == nullptr);
    CHECK(rumbleOnly.hudStyles == nullptr);
}
} // namespace
