#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

std::filesystem::path makeTree() {
    const auto root = test::scratchDirectory("asset-locator");
    std::filesystem::create_directories(root / "VQMOVIES");
    writeFile(root / "VQMOVIES" / "OPENING.avi", std::vector<u8>{1});
    writeFile(root / "VQMOVIES" / "midway.avi", std::vector<u8>{2});
    return root;
}

TEST_CASE("assets are found regardless of case", "[io][assets]") {
    const AssetLocator locator(makeTree());
    const auto opening = locator.find("vqmovies/opening.avi");
    REQUIRE(opening.has_value());
    REQUIRE(opening->filename() == "OPENING.avi");
    REQUIRE(std::filesystem::exists(*opening));

    const auto midway = locator.find("VQMovies\\MIDWAY.AVI");
    REQUIRE(midway.has_value());
    REQUIRE(midway->filename() == "midway.avi");

    const auto directory = locator.find("vqmovies/");
    REQUIRE(directory.has_value());
    REQUIRE(std::filesystem::is_directory(*directory));
}

TEST_CASE("missing assets are reported", "[io][assets]") {
    const AssetLocator locator(makeTree());
    REQUIRE_FALSE(locator.find("vqmovies/nothing.avi").has_value());
    REQUIRE_FALSE(locator.find("nowhere/opening.avi").has_value());
    REQUIRE_THROWS_AS(locator.require("vqmovies/nothing.avi"), FileError);
    REQUIRE(locator.require("VQMOVIES/midway.avi").filename() == "midway.avi");
}

} // namespace
