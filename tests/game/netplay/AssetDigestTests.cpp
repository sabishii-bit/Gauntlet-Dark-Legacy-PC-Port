#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/netplay/AssetDigest.h"

using namespace gdl;
using namespace gdl::game;
TEST_CASE("native compatibility hashes paths and bytes rather than timestamps",
          "[netplay][asset-digest]") {
    const auto a = test::scratchDirectory("asset-digest-a");
    const auto b = test::scratchDirectory("asset-digest-b");
    writeTextFile(a / "TEST.WAD", "abc");
    writeTextFile(b / "test.wad", "abc");
    // Independently produced by hashlib using the launcher's path/size/content framing.
    CHECK(assetDigest(a) == "c587ed1caf07f402b431b28e92840f2c97d56cbab6f8e5ea33e39165ccdf4ca7");
    CHECK(assetDigest(a) == assetDigest(b));
    writeTextFile(b / "test.wad", "abd");
    CHECK(assetDigest(a) != assetDigest(b));
    std::filesystem::rename(b / "test.wad", b / "other.wad");
    writeTextFile(b / "other.wad", "abc");
    CHECK(assetDigest(a) != assetDigest(b));
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS(assetDigest(a, stop.get_token()));
    const auto empty = test::scratchDirectory("asset-digest-empty");
    CHECK_THROWS(assetDigest(empty));
}
