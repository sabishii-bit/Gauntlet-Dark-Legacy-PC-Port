#include <cstddef>
#include <cstdint>
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
    REQUIRE(track->frames == std::vector<std::uint16_t>{0, 4, 11});
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

TEST_CASE("an animation set keeps an object node's runs of frames", "[assets][animation]") {
    const auto dir = test::scratchDirectory("animation-set-object-frames");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FLAME", "prefix": "FX", "sequences": [
     {"name": "ACTIVE", "frames": 30, "frameRate": 60, "repeats": false, "fixesPosition": false,
      "flags": 0, "tracks": []}],
   "nodes": [
    {"name": "ROOT", "object": "", "type": 1, "flags": 1, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]},
    {"name": "GLOW", "object": "", "type": 2, "flags": 1, "objectFlags": 128, "parent": 0,
     "position": [0.0, 1.0, 0.0],
     "objectFrames": [{"object": "fx0f01", "start": 2, "frames": 13}]}]}]})");
    AnimationSet set;
    REQUIRE(set.load(dir));
    const TreeInfo& tree = set.tree(0);
    REQUIRE(tree.nodes[0].objectFrames.empty());
    REQUIRE(tree.nodes[1].type == TreeNodeInfo::kObjectType);
    REQUIRE(tree.nodes[1].objectFrames.size() == 1);
    REQUIRE(tree.nodes[1].objectFrames[0].object == "FX0F01");
    REQUIRE(tree.nodes[1].objectFrames[0].start == 2);
    REQUIRE(tree.nodes[1].objectFrames[0].frames == 13);
    REQUIRE_FALSE(tree.nodes[1].writesDepth());
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

TEST_CASE("the unpacked lich's effects key their texture nodes to their sequences' frames",
          "[assets][animation][unpacked]") {
    const auto dir = test::unpackedOrSkip("MONSTERS/LICH/animations.json").parent_path();
    AnimationSet set;
    REQUIRE(set.load(dir));
    // The axe's glow: one texture node scrolling the caustic texture from the sequence's
    // twenty-ninth frame, easing over eleven, sixteen long; none of the archive's clocked
    // animations, which are flagged free-running.
    const auto glow = set.find("ATK01FX");
    REQUIRE(glow.has_value());
    const TreeInfo& tree = set.tree(*glow);
    REQUIRE(tree.nodes.size() == 2);
    REQUIRE(tree.nodes[0].type == 3);
    REQUIRE(tree.nodes[0].textureAnimation >= 0);
    const TextureAnimationInfo& scroll =
        set.textureAnimations()[static_cast<std::size_t>(tree.nodes[0].textureAnimation)];
    REQUIRE(scroll.name == "EXECAUSTICSM");
    REQUIRE(scroll.source == TextureAnimationInfo::kScrollU);
    REQUIRE(scroll.offset == 29);
    REQUIRE(scroll.rate == 11);
    REQUIRE(scroll.frames == 16);
    REQUIRE_FALSE(scroll.freeRunning());
    REQUIRE(tree.nodes[1].textureAnimation == -1);
    REQUIRE(set.textureAnimations()[0].freeRunning());
    REQUIRE(tree.sequences[0].textureAnimationCount == 0);
    // The stomp's ring: planes cycling the stomp's texture from the thirty-second frame.
    const auto ring = set.find("ATK09FX");
    REQUIRE(ring.has_value());
    std::size_t keyed = 0;
    for (const TreeNodeInfo& node : set.tree(*ring).nodes) {
        if (node.textureAnimation >= 0) {
            const TextureAnimationInfo& cycle =
                set.textureAnimations()[static_cast<std::size_t>(node.textureAnimation)];
            keyed += cycle.name == "EXEATCK09TEX" && cycle.offset == 32 ? 1U : 0U;
        }
    }
    REQUIRE(keyed >= 10);
}

} // namespace
