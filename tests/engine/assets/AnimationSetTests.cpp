#include <filesystem>
#include <vector>

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
     "sequences": [{"name": "ACTIVE", "frames": 12, "frameRate": 30, "repeats": false, "flags": 0,
                    "tracks": [{"node": 1, "flags": 33, "frames": [0, 4, 11],
                                "values": [0.5, 1, 1, 2, -0.5, 3]}]}],
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
    REQUIRE(tree.findSequence("ACTIVE") == 0U);
    REQUIRE_FALSE(tree.findSequence("IDLE").has_value());
    REQUIRE(tree.findNode("FR") == 2U);
    REQUIRE_FALSE(tree.findNode("XX").has_value());

    // The sequence's keys hang off the node they move; the others rest.
    const TreeSequenceInfo& active = tree.sequences[0];
    REQUIRE(active.frames == 12);
    REQUIRE(active.tracks.size() == 1);
    REQUIRE(active.track(0) == nullptr);
    REQUIRE(active.track(9) == nullptr);
    const TrackInfo* track = active.track(1);
    REQUIRE(track != nullptr);
    REQUIRE(track->channelCount() == 2);
    REQUIRE(track->has(0));
    REQUIRE(track->has(4));
    REQUIRE_FALSE(track->has(1));
    REQUIRE_FALSE(track->pitchYawRoll());
    REQUIRE(track->frames == std::vector<u16>{0, 4, 11});
    REQUIRE(track->values[5] == 3.0f);
}

TEST_CASE("an animation set lists the archive's texture animations", "[assets][animation]") {
    const auto dir = test::scratchDirectory("animation-set-texmods");
    writeTextFile(dir / "animations.json", R"({
  "textureAnimations": [
    {"name": "TORCHB", "frameName": "TORCH00", "texture": 423, "source": -1, "frames": 15,
     "start": 5, "rate": 2, "offset": 0, "flag": -1, "scrollIndex": -1},
    {"name": "SUNBEAMS", "frameName": "", "texture": 413, "source": -2, "frames": -350}
  ],
  "trees": []
})");
    AnimationSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.size() == 0);
    REQUIRE(set.textureAnimations().size() == 2);
    const TextureAnimationInfo& torch = set.textureAnimations()[0];
    REQUIRE(torch.texture == 423);
    REQUIRE(torch.frameName == "TORCH00");
    REQUIRE(torch.source == TextureAnimationInfo::kByName);
    REQUIRE(torch.frames == 15);
    REQUIRE(torch.start == 5);
    REQUIRE(torch.rate == 2);
    REQUIRE(torch.cycles());
    REQUIRE(set.textureAnimations()[1].scrolls());
    REQUIRE(set.textureAnimations()[1].frames == -350);
}

TEST_CASE("an animation set lists the archive's particle templates and particle nodes",
          "[assets][animation]") {
    const auto dir = test::scratchDirectory("animation-set-particles");
    writeTextFile(dir / "animations.json", R"({
  "textureAnimations": [],
  "particles": [
    {"id": "C", "preset": 5, "flags": 520, "flagMask": 520, "enables": 420449,
     "particleLife": [0.35, 0.75], "texture": "oran03", "rate": [25, 25, 25, 25],
     "width": [0.8, 0.3, 0.3, 0.3]}
  ],
  "trees": [
    {"name": "GETGEM", "prefix": "", "sequences": [
       {"name": "ACTIVE", "frames": 45, "frameRate": 30, "repeats": false, "tracks": []}],
     "nodes": [
      {"name": "ROOT", "object": "", "type": 1, "flags": 1, "objectFlags": 0, "parent": -1,
       "position": [0, 0, 0]},
      {"name": "", "object": "", "type": 4, "flags": 1, "objectFlags": 0, "parent": 0,
       "position": [0, 1, 0], "particle": 0, "direction": [0, 1, 0]}]}
  ]
})");
    AnimationSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.particleTemplates().size() == 1);
    REQUIRE(set.particleTemplates()[0].id == 'C');
    REQUIRE(set.particleTemplates()[0].texture == "ORAN03");
    REQUIRE(set.particleTemplates()[0].width[0] == 0.8f);
    const TreeInfo& tree = set.tree(0);
    REQUIRE(tree.nodes[0].particle == -1);
    REQUIRE(tree.nodes[1].type == TreeNodeInfo::kParticleType);
    REQUIRE(tree.nodes[1].particle == 0);
    REQUIRE(tree.nodes[1].direction == Vec3{0.0f, 1.0f, 0.0f});
}

TEST_CASE("a missing or malformed animation manifest fails to load", "[assets][animation]") {
    AnimationSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("animation-set-empty")));
    const auto dir = test::scratchDirectory("animation-set-bad");
    writeTextFile(dir / "animations.json",
                  R"({"trees": [{"name": "X", "nodes": [{"parent": 5, "position": [0, 0, 0]}]}]})");
    REQUIRE_FALSE(set.load(dir));
    // Tracks must match their keys and name a node of the tree.
    writeTextFile(dir / "animations.json",
                  R"({"trees": [{"name": "X", "sequences": [{"name": "A", "frames": 2,
                  "tracks": [{"node": 0, "flags": 1, "frames": [0, 1], "values": [1]}]}],
                  "nodes": [{"name": "N", "position": [0, 0, 0]}]}]})");
    REQUIRE_FALSE(set.load(dir));
    writeTextFile(dir / "animations.json",
                  R"({"trees": [{"name": "X", "sequences": [{"name": "A", "frames": 2,
                  "tracks": [{"node": 3, "flags": 1, "frames": [0], "values": [1]}]}],
                  "nodes": [{"name": "N", "position": [0, 0, 0]}]}]})");
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
