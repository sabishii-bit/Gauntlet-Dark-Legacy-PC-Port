#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/level/LegendsPortalAppearance.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Locked native portal hides the centre mesh but retains its descendants",
          "[legends][portal][unlocks]") {
    TreeInfo tree;
    tree.nodes.resize(7);
    tree.nodes[0].name = "CENTEREXIT";
    tree.nodes[0].object = "EXIT_ACTIVE1CENTEREXIT";
    tree.nodes[1].object = "F1";
    tree.nodes[1].parent = 0;
    tree.nodes[2].name = "XCOANIM";
    tree.nodes[2].object = "TRAP:EXNR:TEX0:EXIT_ACTIVE1XCOANIM";
    tree.nodes[2].parent = 0;
    tree.nodes[3].object = "F2";
    tree.nodes[3].parent = 2;
    tree.nodes[4].object = "F3";
    tree.nodes[4].parent = 3;
    tree.nodes[5].name = "CENTER1";
    tree.nodes[5].object = "ANOTHER_PREFIX_CENTER1";
    tree.nodes[6].name = "PORTALRIM";
    tree.nodes[6].object = "CENTER_PREFIX_PORTALRIM"; // Full-name prefix is not the native alias.
    const auto before = tree.nodes[0].object;
    CHECK(legendsLockedPortalMeshes(tree) == std::vector<usize>{0, 2, 3, 4, 5});
    CHECK(tree.nodes[0].object == before);
}

TEST_CASE("Native portal lock masks resolve the actual indexed model part aliases",
          "[legends][portal][unlocks][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto archive = root / "itemanim/hires";
    if (!std::filesystem::is_regular_file(archive / "seq.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto models = formats::decodeLegendsN64Models(readFile(archive / "objects.wad"));
    auto decoded = formats::decodeLegendsN64Sequences(readFile(archive / "seq.wad"), models,
                                                      readFile(root / "_program/game.bin"));
    auto animations = AnimationSet::fromDecoded(std::move(decoded.animation));
    const auto index = animations.find(formats::legendsN64TreeName("TRAP", "EXNR"));
    REQUIRE(index);
    const auto& tree = animations.tree(*index);
    const auto hidden = legendsLockedPortalMeshes(tree);
    REQUIRE_FALSE(hidden.empty());
    bool centre = false;
    bool animation = false;
    for (const auto node : hidden) {
        REQUIRE(node < tree.nodes.size());
        centre = centre || tree.nodes[node].name == "CENTEREXIT";
        animation = animation || tree.nodes[node].name == "XCOANIM";
    }
    CHECK(centre);
    CHECK(animation);
}

TEST_CASE("Native portal presentation bounds malformed hierarchy walks",
          "[legends][portal][unlocks]") {
    TreeInfo tree;
    tree.nodes.resize(3);
    tree.nodes[0].parent = 1;
    tree.nodes[1].parent = 0;
    tree.nodes[2].parent = 99;
    CHECK(legendsLockedPortalMeshes(tree).empty());
}
