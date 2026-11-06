#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/level/InstanceNames.h"
#include "game/level/SyntheticLayout.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("an item instance goes by its own name, else its record's",
          "[dev-console][commands][level]") {
    const WorldLayout layout = test::syntheticLayout();
    CHECK(instanceName(layout, 0) == "BRIDGEPAD");
    CHECK(instanceName(layout, 1) == "LIFTPAD");
    CHECK(instanceName(layout, 2) == "TARGET");
    CHECK(instanceName(layout, 3) == "GRU");
    CHECK(instanceName(layout, 11) == "KEY");
    for (const s32 instance : {12, -1, 13}) {
        CAPTURE(instance);
        CHECK(instanceName(layout, instance).empty());
    }
}

TEST_CASE("an item instance's record is the layout's by its index, or none",
          "[dev-console][commands][level]") {
    const WorldLayout layout = test::syntheticLayout();
    const auto& instances = layout.itemInstances();
    REQUIRE(instances.size() == 13);
    const ItemInfo* pad = instanceRecord(layout, instances[1]);
    REQUIRE(pad != nullptr);
    CHECK(pad == layout.itemInfos().data());
    CHECK(pad->name == "BRIDGEPAD");
    const ItemInfo* rat = instanceRecord(layout, instances[8]);
    REQUIRE(rat != nullptr);
    CHECK(rat == &layout.itemInfos()[11]);
    CHECK(instanceRecord(layout, instances[12]) == nullptr);
    ItemInstance beyond = instances[0];
    beyond.info = static_cast<s32>(layout.itemInfos().size());
    CHECK(instanceRecord(layout, beyond) == nullptr);
}

TEST_CASE("a pickup record's subtype is named for what it is", "[dev-console][commands][level]") {
    CHECK(pickupKindName(2) == "keys");
    CHECK(pickupKindName(99) == "pickup");
}

} // namespace
