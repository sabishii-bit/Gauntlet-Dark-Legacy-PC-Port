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

/** One tree "ARROW" with a group root and a chrome mesh child, plus one sequence. */
std::vector<u8> sampleFile() {
    constexpr u32 kInfosAt = 24;
    constexpr u32 kTreeAt = kInfosAt + 36;
    constexpr u32 kSequencesAt = 56;
    constexpr u32 kNodesAt = kSequencesAt + 48;
    ByteWriter w;
    w.putU16(1).putU16(8).putU32(kInfosAt).putU32(0).putU32(0).putU32(0).putU32(0);
    putName(w, "ARROW", 32).putU32(kTreeAt);
    w.putU32(kSequencesAt).putU32(0).putU32(0).putU32(kNodesAt).putU32(2).putU32(1);
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
    w.putU16(1).putU16(1).putU32(0).putU32(0).putU32(0xFFFFFFFF);
    putName(w, "FRT_SPIKE", 32).putU32(0).putU32(0x3F000000).putU32(0);
    w.putU16(1).putU16(0).putU32(0x8000).putU32(28).putU32(0);
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

} // namespace
