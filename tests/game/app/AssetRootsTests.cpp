#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/app/AssetRoots.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("packaged asset roots override developer paths regardless of casing",
          "[game][asset-roots]") {
    const auto root = test::scratchDirectory("asset-roots-packaged");
    std::filesystem::create_directories(root / "gAuNtLeT");
    std::filesystem::create_directories(root / "DATA");
    std::filesystem::create_directories(root / "carddemo");
    const auto roots = resolveAssetRoots(root, "developer/assets", "developer/data");
    REQUIRE(roots.assets == root / "gAuNtLeT");
    REQUIRE(roots.data == root / "DATA");
    REQUIRE(roots.assets.parent_path() / "carddemo" == root / "carddemo");
}

TEST_CASE("developer roots remain available without a packaged tree", "[game][asset-roots]") {
    const auto root = test::scratchDirectory("asset-roots-developer");
    const auto roots = resolveAssetRoots(root, "developer/assets", "developer/data");
    REQUIRE(roots.assets == "developer/assets");
    REQUIRE(roots.data == "developer/data");
    const auto missing = resolveAssetRoots(root, {}, {});
    REQUIRE(missing.assets == root / "Gauntlet");
    REQUIRE(missing.data == root / "data");
}

TEST_CASE("shipped configuration resolves independently from game assets", "[game][asset-roots]") {
    const auto root = test::scratchDirectory("asset-roots-independent");
    std::filesystem::create_directories(root / "data");
    const auto roots = resolveAssetRoots(root, "developer/assets", "developer/data");
    REQUIRE(roots.assets == "developer/assets");
    REQUIRE(roots.data == root / "data");
}

} // namespace
