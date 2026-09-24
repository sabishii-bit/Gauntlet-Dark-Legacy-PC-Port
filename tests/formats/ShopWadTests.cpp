#include <bit>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/ShopWad.h"
namespace {
using namespace gdl;
using namespace gdl::formats;
std::vector<u8> shopWad() {
    test::ByteWriter writer;
    writer.putU32(96).putU32(1).putU32(0).putU32(0);
    std::vector<u8> names(64);
    names[0] = 'K';
    names[32] = 'K';
    writer.putBytes(names).putU32(std::bit_cast<u32>(1.0f)).putS32(1).putS32(100).putS32(1);
    writer.putFourcc("METI").putU32(16).putU32(1).putU32(
        1); // WAD tags stored in reverse byte order
    return writer.bytes();
}
TEST_CASE("shop wad decodes item fields without a console file at runtime", "[formats][shop]") {
    const auto items = parseShopWad(shopWad());
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].texture == "K");
    REQUIRE(items[0].description == "K");
    REQUIRE(items[0].type == 1);
    REQUIRE(items[0].price == 100);
    REQUIRE(items[0].amount == 1);
    REQUIRE(items[0].scale == 1);
}
TEST_CASE("shop wad rejects truncated or corrupt item sections", "[formats][shop]") {
    auto bytes = shopWad();
    bytes[104] = 2;
    REQUIRE_THROWS_AS(parseShopWad(bytes), FormatError);
    bytes = shopWad();
    bytes[96] = 'X';
    REQUIRE_THROWS_AS(parseShopWad(bytes), FormatError);
    bytes = shopWad();
    bytes[87] = 0xFF;
    REQUIRE_THROWS_AS(parseShopWad(bytes), FormatError);
    bytes = shopWad();
    bytes.resize(15);
    REQUIRE_THROWS_AS(parseShopWad(bytes), FormatError);
}
TEST_CASE("retail shop wad contains thirty four entries", "[formats][shop][assets]") {
    const auto file = test::assetOrSkip("SHPDATA/SHOP.WAD");
    const auto items = parseShopWad(readFile(file));
    REQUIRE(items.size() == 34);
    REQUIRE(items.front().type == 0);
    REQUIRE(items.back().price == 1000);
}
} // namespace
