#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "animations.json", R"({
  "trees": [
    {"name": "ARROW", "prefix": "ICON_ARROW",
     "sequences": [{"name": "ACTIVE", "frames": 0, "frameRate": 30, "repeats": false, "flags": 0}],
     "nodes": [
       {"name": "L1", "object": "", "type": 1, "flags": 1, "objectFlags": 0, "parent": -1,
        "position": [-1.0, 0.0, 0.0]},
       {"name": "G", "object": "", "type": 1, "flags": 1, "objectFlags": 0, "parent": 0,
        "position": [0.5, 0.0, 0.25]},
       {"name": "FR", "object": "ICON_ARROWFR", "type": 1, "flags": 0, "objectFlags": 32768,
        "parent": 1, "position": [0.0, 1.0, 0.0]}
     ]}
  ]
})");
    return dir;
}

TEST_CASE("animation sets expose trees, nodes and world offsets", "[assets][animation]") {
    AnimationSet set;
    REQUIRE(set.load(sampleSet("animation-set")));
    REQUIRE(set.size() == 1);
    REQUIRE(set.find("arrow") == 0U);
    const TreeInfo& tree = set.tree(0);
    REQUIRE(tree.prefix == "ICON_ARROW");
    REQUIRE(tree.sequences.size() == 1);
    REQUIRE(tree.sequences[0].frameRate == 30);
    REQUIRE(tree.nodes.size() == 3);
    REQUIRE(tree.nodes[2].object == "ICON_ARROWFR");
    REQUIRE(tree.nodes[2].chrome());
    REQUIRE_FALSE(tree.nodes[0].chrome());
    const Vec3 world = tree.worldPosition(2);
    REQUIRE(world.x == Approx(-0.5f));
    REQUIRE(world.y == Approx(1.0f));
    REQUIRE(world.z == Approx(0.25f));
}

TEST_CASE("a missing or malformed animation manifest fails to load", "[assets][animation]") {
    AnimationSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("animation-set-empty")));
    const auto dir = test::scratchDirectory("animation-set-bad");
    writeTextFile(dir / "animations.json",
                  R"({"trees": [{"name": "X", "nodes": [{"parent": 5, "position": [0, 0, 0]}]}]})");
    REQUIRE_FALSE(set.load(dir));
}

TEST_CASE("the unpacked powerups define the menu arrow tree", "[assets][animation][unpacked]") {
    const auto dir = test::unpackedOrSkip("POWERUPS/animations.json").parent_path();
    AnimationSet set;
    REQUIRE(set.load(dir));
    const auto index = set.find("ICON_ARROW");
    REQUIRE(index.has_value());
    REQUIRE(set.tree(*index).nodes.size() == 9);
}

} // namespace
