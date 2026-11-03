#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeGameServices.h"
#include "game/scenes/ClassUnlocks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<ClassUnlocks>);
static_assert(std::has_virtual_destructor_v<ClassUnlocks>);

TEST_CASE("recorded class unlocks keep every grant in turn after the hook has seen it",
          "[game][screens][class-unlocks]") {
    test::RecordingUnlocks recorded;
    ClassUnlocks& unlocks = recorded;
    unlocks.unlockClasses(0x100);
    CHECK(recorded.masks == std::vector<u16>{0x100});
    std::vector<usize> seenBefore;
    recorded.hook = [&](u16 mask) {
        CHECK(mask == 0x2);
        seenBefore.push_back(recorded.masks.size());
    };
    unlocks.unlockClasses(0x2);
    CHECK(seenBefore == std::vector<usize>{1});
    CHECK(recorded.masks == std::vector<u16>{0x100, 0x2});
}
} // namespace
