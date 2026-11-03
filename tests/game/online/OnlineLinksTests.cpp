#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "FakeGameServices.h"
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
    test::RecordingHaptics haptics;
    test::RecordingUnlocks unlocks;
    test::RecordingPartyStore store;
    const OnlineLinks links{&haptics, &unlocks, &store};
    CHECK(links.haptics == &haptics);
    CHECK(links.unlocks == &unlocks);
    CHECK(links.store == &store);
    const OnlineLinks rumbleOnly{.haptics = &haptics};
    CHECK(rumbleOnly.haptics == &haptics);
    CHECK(rumbleOnly.unlocks == nullptr);
    CHECK(rumbleOnly.store == nullptr);
}
} // namespace
