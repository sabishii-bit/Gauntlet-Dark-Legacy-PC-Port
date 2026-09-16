#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"

namespace {

using namespace gdl;

TEST_CASE("asset names upper-case and stop at whitespace", "[core][strings]") {
    REQUIRE(normalizeAssetName("  title00 extra") == "TITLE00");
    REQUIRE(normalizeAssetName("Glow_Crop") == "GLOW_CROP");
    REQUIRE(normalizeAssetName("font32\t") == "FONT32");
    REQUIRE(normalizeAssetName("").empty());
    REQUIRE(normalizeAssetName("   ").empty());
}

TEST_CASE("lower-casing touches ASCII letters only", "[core][strings]") {
    REQUIRE(toLowerAscii("GAUNT.FNT") == "gaunt.fnt");
    REQUIRE(toLowerAscii("mixed Case 123") == "mixed case 123");
}

} // namespace
