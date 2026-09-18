#include <algorithm>
#include <bit>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/AnimationTree.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;
using test::ByteWriter;

ByteWriter& putName(ByteWriter& w, std::string_view name, usize width) {
    w.putText(name);
    return w.putZeros(width - name.size());
}

ByteWriter& putF32(ByteWriter& w, f32 value) {
    return w.putU32(std::bit_cast<u32>(value));
}

constexpr u32 kInfosAt = 24;
constexpr u32 kTreeAt = kInfosAt + 36;
constexpr u32 kSequencesAt = 56;
constexpr u32 kNodesAt = kSequencesAt + 48;
constexpr u32 kKeysAt = kNodesAt + 2 * 60;   ///< the key header, relative to the tree
constexpr u32 kEntriesAt = 28;               ///< the nodes' entries, relative to the header
constexpr u32 kTableAt = kEntriesAt + 2 * 8; ///< the angle delta table
constexpr u32 kBlocksAt = kTableAt + 256 * 4;

/** One tree "ARROW" with a skeletal root and a chrome mesh child, plus one sequence of
 * twelve frames: the root keys pitch and height as plain floats at frames 0, 4 and 11, the
 * child keys yaw in compressed form at frames 0 and 6. */
std::vector<u8> sampleFile() {
    ByteWriter w;
    w.putU16(1).putU16(8).putU32(kInfosAt).putU32(0).putU32(0).putU32(0).putU32(0);
    putName(w, "ARROW", 32).putU32(kTreeAt);
    w.putU32(kSequencesAt).putU32(kKeysAt).putU32(0).putU32(kNodesAt).putU32(2).putU32(1);
    putName(w, "ICON_ARROW", 30).putU16(0);
    putName(w, "ACTIVE", 32)
        .putU16(12)
        .putU16(30)
        .putU16(1)
        .putU16(0)
        .putU16(0)
        .putU16(1)
        .putU32(0);
    putName(w, "L1", 32).putU32(0xBFF00000).putU32(0).putU32(0);
    w.putU16(1).putU16(1).putU32(0).putU32(kEntriesAt).putU32(0xFFFFFFFF);
    putName(w, "FRT_SPIKE", 32).putU32(0).putU32(0x3F000000).putU32(0);
    w.putU16(1).putU16(0).putU32(0x8000).putU32(kEntriesAt + 8).putU32(0);
    // The key header: an angle table, no position or scale tables, then the blocks.
    w.putU32(kTableAt).putU32(0).putU32(0).putU32(kBlocksAt).putU32(kEntriesAt).putU32(1).putU32(0);
    w.putU16(0x21).putU16(2).putU32(0);    // L1: rotation x and position y, two floats a key
    w.putU16(0xA002).putU16(1).putU32(28); // FRT_SPIKE: rotation y, compressed, pitch-yaw-roll
    for (int i = 0; i < 256; ++i) {
        putF32(w, i == 3 ? 0.5f : 0.0f);
    }
    w.putU32(0x811); // keys at frames 0, 4 and 11
    for (const f32 value : {0.5f, 1.0f, 1.0f, 2.0f, -0.5f, 3.0f}) {
        putF32(w, value);
    }
    w.putU32(0x41); // keys at frames 0 and 6: the first as a float, the next a table step
    putF32(w, 0.25f);
    w.putU8(3);
    return w.bytes();
}

TEST_CASE("animation trees list their nodes with resolved object names", "[formats][animation]") {
    const AnimationFile file = AnimationFile::parse(sampleFile());
    REQUIRE(file.trees.size() == 1);
    const TreeDefinition& tree = file.trees[0];
    REQUIRE(tree.name == "ARROW");
    REQUIRE(tree.prefix == "ICON_ARROW");
    REQUIRE(tree.sequences.size() == 1);
    REQUIRE(tree.sequences[0].name == "ACTIVE");
    REQUIRE(tree.sequences[0].frameCount == 12);
    REQUIRE(tree.sequences[0].frameRate == 30);
    REQUIRE(tree.sequences[0].repeats);
    REQUIRE(tree.sequences[0].flags == 1);

    REQUIRE(tree.nodes.size() == 2);
    REQUIRE(tree.nodes[0].name == "L1");
    REQUIRE(tree.nodes[0].object.empty());
    REQUIRE(tree.nodes[0].type == TreeNodeType::Skeletal);
    REQUIRE(tree.nodes[0].parent == -1);
    REQUIRE(tree.nodes[0].position.x == Approx(-1.875f));
    REQUIRE(tree.nodes[1].name == "FRT_SPIKE");
    REQUIRE(tree.nodes[1].object == "ICON_ARROWFRT_S");
    REQUIRE(tree.nodes[1].objectFlags == AnimationFile::kChromeFlag);
    REQUIRE(tree.nodes[1].parent == 0);
    REQUIRE(tree.nodes[1].position.y == Approx(0.5f));
    REQUIRE(file.find("arrow") == 0U);
    REQUIRE_FALSE(file.find("other").has_value());
}

TEST_CASE("animation trees decode each node's keys, plain or compressed", "[formats][animation]") {
    const AnimationFile file = AnimationFile::parse(sampleFile());
    const TreeSequence& sequence = file.trees[0].sequences[0];
    REQUIRE_FALSE(sequence.fixesPosition);
    REQUIRE(sequence.tracks.size() == 2);
    const NodeTrack& root = sequence.tracks[0];
    REQUIRE(root.node == 0);
    REQUIRE(root.flags == (NodeTrack::kRotationX | NodeTrack::kPositionY));
    REQUIRE(root.channelCount() == 2);
    REQUIRE(root.has(0));
    REQUIRE(root.has(4));
    REQUIRE_FALSE(root.has(3));
    REQUIRE(root.frames == std::vector<u16>{0, 4, 11});
    REQUIRE(root.values == std::vector<f32>{0.5f, 1.0f, 1.0f, 2.0f, -0.5f, 3.0f});
    const NodeTrack& spike = sequence.tracks[1];
    REQUIRE(spike.node == 1);
    REQUIRE(spike.flags == (NodeTrack::kRotationY | NodeTrack::kPitchYawRoll));
    REQUIRE(spike.frames == std::vector<u16>{0, 6});
    REQUIRE(spike.values.size() == 2);
    REQUIRE(spike.values[0] == 0.25f);
    REQUIRE(spike.values[1] == Approx(0.75f)); // the first key plus the table's step

    // A record too short for its channels is rejected.
    std::vector<u8> bad = sampleFile();
    bad[kTreeAt + kKeysAt + kEntriesAt + 2] = 1;
    REQUIRE_THROWS_AS(AnimationFile::parse(bad), FormatError);
}

/** A level's file: no trees, two texture animations in the header's list. */
std::vector<u8> sampleLevelFile() {
    ByteWriter w;
    w.putU16(0).putU16(0).putU32(24).putU32(2).putU32(24).putU32(0).putU32(0);
    // A torch: fifteen frames found by name, every second frame, starting five in.
    w.putU16(0xFFFF).putU16(0xFFFF);
    putName(w, "TORCHB", 32);
    putName(w, "TORCH00", 32);
    w.putU32(423).putU32(0xFFFFFFFF).putU16(15).putU16(0).putU32(2).putU32(5);
    // Sunbeams: the coordinates scroll along u the other way over 350 frames.
    w.putU16(0xFFFF).putU16(0xFFFF);
    putName(w, "SUNBEAMS", 32);
    putName(w, "", 32);
    w.putU32(413).putU32(0xFFFFFFFE).putU16(static_cast<u16>(-350)).putU16(0).putU32(0).putU32(0);
    return w.bytes();
}

TEST_CASE("a level's file lists its texture animations", "[formats][animation]") {
    const AnimationFile file = AnimationFile::parse(sampleLevelFile());
    REQUIRE(file.trees.empty());
    REQUIRE(file.textureAnimations.size() == 2);
    const TextureAnimation& torch = file.textureAnimations[0];
    REQUIRE(torch.name == "TORCHB");
    REQUIRE(torch.frameName == "TORCH00");
    REQUIRE(torch.texture == 423);
    REQUIRE(torch.source == TextureAnimation::kByName);
    REQUIRE(torch.frames == 15);
    REQUIRE(torch.rate == 2);
    REQUIRE(torch.start == 5);
    REQUIRE(torch.cycles());
    REQUIRE_FALSE(torch.scrolls());
    const TextureAnimation& beams = file.textureAnimations[1];
    REQUIRE(beams.texture == 413);
    REQUIRE(beams.source == TextureAnimation::kScrollU);
    REQUIRE(beams.frames == -350);
    REQUIRE(beams.scrolls());
    // The tree sample names none.
    REQUIRE(AnimationFile::parse(sampleFile()).textureAnimations.empty());
}

TEST_CASE("the tower's level file animates its torches and clouds", "[formats][animation][assets]") {
    const auto path = test::assetOrSkip("LEVELS/levelL1/ANIM.PS2");
    const AnimationFile file = AnimationFile::parse(readFile(path));
    REQUIRE(file.trees.empty());
    REQUIRE(file.textureAnimations.size() == 55);
    REQUIRE(file.textureAnimations[0].name == "TORCHA");
    REQUIRE(file.textureAnimations[0].frameName == "TORCH00");
    REQUIRE(file.textureAnimations[0].source == TextureAnimation::kByName);
    REQUIRE(file.textureAnimations[2].start == 10);
    REQUIRE(file.textureAnimations[3].name == "LAMPS00");
    REQUIRE(file.textureAnimations[3].source == 252);
    REQUIRE(file.textureAnimations[18].name == "SUNBEAMS");
    REQUIRE(file.textureAnimations[18].scrolls());
}

TEST_CASE("an archive's file lists its particle templates and the nodes that emit them",
          "[formats][animation][assets]") {
    const auto path = test::assetOrSkip("POWERUPS/ANIM.PS2");
    const AnimationFile file = AnimationFile::parse(readFile(path));
    REQUIRE(file.particles.size() == 19);
    REQUIRE(file.particles[3].id == 'C');
    REQUIRE(file.particles[3].texture == "ORAN03");
    REQUIRE(file.particles[3].preset == 5);
    const auto tree = file.find("GETGEMORANGE");
    REQUIRE(tree.has_value());
    const TreeDefinition& burst = file.trees[*tree];
    REQUIRE(burst.nodes.size() == 7);
    REQUIRE(static_cast<u16>(burst.nodes[3].type) == 4);
    REQUIRE(burst.nodes[3].particle == 3); // the orange sparks
    REQUIRE(burst.nodes[3].direction == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(burst.nodes[0].particle == -1);
    // A level's older file lists none.
    const AnimationFile level = AnimationFile::parse(readFile(test::assetOrSkip("LEVELS/levelL1/ANIM.PS2")));
    REQUIRE(level.particles.empty());
}

TEST_CASE("damaged animation files are rejected", "[formats][animation]") {
    REQUIRE_THROWS_AS(AnimationFile::parse(std::vector<u8>(4, 0)), FormatError);
    std::vector<u8> bad = sampleFile();
    bad[24 + 36 + 16] = 200; // node count past the file
    REQUIRE_THROWS_AS(AnimationFile::parse(bad), FormatError);
}

TEST_CASE("the powerups file defines the menu arrow", "[formats][animation][assets]") {
    const auto path = test::assetOrSkip("POWERUPS/ANIM.PS2");
    const AnimationFile file = AnimationFile::parse(readFile(path));
    REQUIRE(file.trees.size() == 99);
    const auto index = file.find("ICON_ARROW");
    REQUIRE(index.has_value());
    const TreeDefinition& tree = file.trees[*index];
    REQUIRE(tree.nodes.size() == 9);
    REQUIRE(tree.sequences.size() == 1);
    REQUIRE(tree.nodes[0].name == "L1");
    REQUIRE(tree.nodes[0].object.empty());
    REQUIRE(tree.nodes[2].object == "ICON_ARROWFRT_S");
    REQUIRE((tree.nodes[2].objectFlags & AnimationFile::kChromeFlag) != 0);
    REQUIRE(tree.nodes[6].object == "ICON_ARROWFR#0");
    REQUIRE(tree.nodes[6].parent == 5);
    REQUIRE(tree.nodes[8].object == "ICON_ARROWGR#1");
}

TEST_CASE("the warrior's class file carries every sequence's keys", "[formats][animation][assets]") {
    const auto path = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2");
    const AnimationFile file = AnimationFile::parse(readFile(path));
    const auto index = file.find("WAR");
    REQUIRE(index.has_value());
    const TreeDefinition& tree = file.trees[*index];
    REQUIRE(tree.nodes.size() == 26);
    REQUIRE(tree.sequences.size() == 140);
    const TreeSequence& ready = tree.sequences[0];
    REQUIRE(ready.name == "READY");
    REQUIRE(ready.frameCount == 60);
    REQUIRE(ready.frameRate == 30);
    REQUIRE(ready.repeats);
    // The root keys nothing; the pelvis keys rotation and position at 57 of the 60 frames.
    REQUIRE(std::ranges::none_of(ready.tracks, [](const NodeTrack& t) { return t.node == 0; }));
    const auto pelvis =
        std::ranges::find_if(ready.tracks, [](const NodeTrack& t) { return t.node == 1; });
    REQUIRE(pelvis != ready.tracks.end());
    REQUIRE(pelvis->flags == 0x77);
    REQUIRE(pelvis->channelCount() == 6);
    REQUIRE(pelvis->frames.size() == 57);
    REQUIRE(pelvis->frames[1] == 1);
    REQUIRE(pelvis->frames.back() == 59);
    REQUIRE(pelvis->values.size() == usize{57} * 6);
    REQUIRE(pelvis->values[0] == Approx(-0.223f).margin(0.001f));
    REQUIRE(pelvis->values[1] == Approx(0.565f).margin(0.001f));
    REQUIRE(pelvis->values[4] == Approx(0.024f).margin(0.001f));
    // The keys are steps through the shared tables, summed: the last one lands here.
    REQUIRE(pelvis->values[56 * 6 + 1] == Approx(0.595f).margin(0.002f));
    REQUIRE(pelvis->values[56 * 6 + 5] == Approx(0.034f).margin(0.002f));
    REQUIRE(tree.sequences[1].name == "IDLE1");
    REQUIRE(tree.sequences[1].frameRate == 45);
    REQUIRE(tree.sequences[4].name == "START");
    REQUIRE_FALSE(tree.sequences[4].repeats);
}

/** One tree "FLAME" of a root and an object node whose one sequence flips through thirteen
 * objects from FX0F01, starting at frame 2. */
std::vector<u8> objectFrameFile() {
    constexpr u32 kTree = kInfosAt + 36;
    constexpr u32 kNodes = 56 + 48;
    constexpr u32 kRuns = kNodes + 2 * 60;
    ByteWriter w;
    w.putU16(1).putU16(8).putU32(kInfosAt).putU32(0).putU32(0).putU32(0).putU32(0);
    putName(w, "FLAME", 32).putU32(kTree);
    w.putU32(56).putU32(0).putU32(kRuns).putU32(kNodes).putU32(2).putU32(1);
    putName(w, "FX", 30).putU16(0);
    putName(w, "ACTIVE", 32)
        .putU16(30)
        .putU16(60)
        .putU16(0)
        .putU16(0)
        .putU16(0)
        .putU16(0)
        .putU32(0);
    putName(w, "ROOT", 32).putU32(0).putU32(0).putU32(0);
    w.putU16(1).putU16(1).putU32(0).putU32(0xFFFFFFFF).putU32(0xFFFFFFFF);
    putName(w, "GLOW", 32).putU32(0).putU32(0x3F800000).putU32(0);
    w.putU16(2).putU16(1).putU32(0x80).putU32(8).putU32(0);
    w.putU32(8).putU32(1); // the run table: its runs start eight bytes in, one of them
    putName(w, "FX0F01", 32).putU32(0xFFFFFFFF).putU16(13).putU16(2);
    return w.bytes();
}

TEST_CASE("an object node lists the run of objects each sequence flips through",
          "[formats][animation]") {
    const AnimationFile file = AnimationFile::parse(objectFrameFile());
    REQUIRE(file.trees.size() == 1);
    const TreeDefinition& tree = file.trees[0];
    REQUIRE(tree.nodes.size() == 2);
    REQUIRE(tree.nodes[0].objectFrames.empty());
    const TreeNode& glow = tree.nodes[1];
    REQUIRE(glow.type == TreeNodeType::Object);
    REQUIRE(glow.object.empty()); // it draws nothing of its own
    REQUIRE(glow.position.y == 1.0f);
    REQUIRE(glow.objectFlags == 0x80);
    REQUIRE(glow.objectFrames.size() == 1);
    REQUIRE(glow.objectFrames[0].object == "FX0F01");
    REQUIRE(glow.objectFrames[0].start == 2);
    REQUIRE(glow.objectFrames[0].frames == 13);
}

} // namespace
