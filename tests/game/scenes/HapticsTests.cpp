#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeGameServices.h"
#include "game/config/ControlProfiles.h"
#include "game/scenes/Haptics.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Pulse = test::RecordingHaptics::Pulse;

static_assert(std::is_abstract_v<Haptics>);
static_assert(std::has_virtual_destructor_v<Haptics>);

TEST_CASE("recorded haptics keep every pulse in turn and count every stop",
          "[game][screens][haptics]") {
    test::RecordingHaptics recorded;
    Haptics& haptics = recorded;
    CHECK(recorded.pulses.empty());
    CHECK(recorded.stops == 0);
    haptics.vibrate(3, 10, ControlFeedback::Damage);
    haptics.stopRumble();
    haptics.vibrate(1, 0, ControlFeedback::MeleeHit);
    haptics.stopRumble();
    CHECK(recorded.pulses == std::vector{Pulse{3, 10, ControlFeedback::Damage},
                                         Pulse{1, 0, ControlFeedback::MeleeHit}});
    CHECK(recorded.stops == 2);
}
} // namespace
