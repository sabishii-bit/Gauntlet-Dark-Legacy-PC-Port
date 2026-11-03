#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "formats/CritterWad.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("every native Legends boss stock is a boss in each of its parts",
          "[legends][native-boss-assets][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("N64 native banks not installed");
    }
    const auto gc = test::assetOrSkip("CRITTER").parent_path();
    const auto program = readFile(root / "_program/game.bin");
    test::FakeRenderDevice device;
    constexpr s32 kFirstBossKind = 34; ///< the dragon
    constexpr s32 kLastBossKind = 44;  ///< the garm
    usize stocks = 0;
    usize parts = 0;
    for (s32 kind = kFirstBossKind; kind <= kLastBossKind; ++kind) {
        CAPTURE(kind);
        std::vector<std::string> warnings;
        const auto stock = loadLegendsBossAssets(device, root, gc, kind, program, {}, warnings);
        if (!stock) {
            continue; // no native bank stands this boss
        }
        ++stocks;
        CHECK(stock->definition.kind == CombatantKind::Boss);
        CHECK(stock->data.kind() == CombatantKind::Boss);
        CHECK(stock->data.rules().pressureReferenceDamage > 0);
        for (const CritterData& child : stock->children) {
            CHECK(child.kind() == CombatantKind::Boss);
            CHECK(child.rules().pressureReferenceDamage > 0);
            ++parts;
        }
    }
    CHECK(stocks == 6);
    CHECK(parts == 3); // the chimera's heads
}

TEST_CASE("the great ones a Legends level stands beside its boss carry no pressure",
          "[legends][native-boss-assets][assets]") {
    // Its golems come from the GameCube's critter data, the only stocks it holds besides
    // the boss's own.
    const auto directory = test::assetOrSkip("CRITTER");
    usize greatOnes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (toLowerAscii(entry.path().extension().string()) != ".wad") {
            continue;
        }
        CAPTURE(entry.path().filename().string());
        const auto source = formats::parseCritterWad(readFile(entry.path()));
        for (usize i = 0; i < source.types.size(); ++i) {
            CritterData part;
            REQUIRE(part.load(entry.path(), i));
            if (part.kind() == CombatantKind::Boss) {
                continue;
            }
            CHECK(part.rules().pressureReferenceDamage == 0);
            ++greatOnes;
        }
    }
    CHECK(greatOnes == 7); // three golems, the general and three gargoyles
}

} // namespace
