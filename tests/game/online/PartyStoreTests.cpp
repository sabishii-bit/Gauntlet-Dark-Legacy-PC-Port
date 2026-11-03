#include <array>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeGameServices.h"
#include "game/characters/Party.h"
#include "game/online/PartyStore.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<PartyStore>);
static_assert(std::has_virtual_destructor_v<PartyStore>);

TEST_CASE("a recorded party store counts each kind of save and answers explicit ones as told",
          "[game][online][party-store]") {
    test::RecordingPartyStore recorded;
    PartyStore& store = recorded;
    std::array party{PartyMember{2, {}}, PartyMember{0, {}}};
    std::vector<s32> seen;
    recorded.hook = [&](std::span<PartyMember> members) {
        seen.push_back(static_cast<s32>(members.size()));
        seen.push_back(members.front().player);
    };
    store.autoSave(party);
    CHECK(recorded.autoSaves == 1);
    CHECK(recorded.saves == 0);
    CHECK(store.saveParty(std::span(party).last(1)));
    recorded.saveSucceeds = false;
    CHECK_FALSE(store.saveParty(party));
    CHECK(recorded.autoSaves == 1);
    CHECK(recorded.saves == 2);
    CHECK(seen == std::vector<s32>{2, 2, 1, 0, 2, 2});
}
} // namespace
