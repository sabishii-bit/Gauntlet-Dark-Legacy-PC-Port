#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ShopCatalog.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {
using namespace gdl;

std::vector<u8> catalogWad(s32 type = 0, s32 price = 0) {
    test::ByteWriter bytes;
    bytes.putU32(96).putU32(1).putZeros(8);
    bytes.putText("EXIT").putZeros(28).putText("LEAVE").putZeros(27);
    bytes.putU32(std::bit_cast<u32>(1.0f)).putS32(type).putS32(price).putS32(0);
    bytes.putFourcc("METI").putU32(16).putU32(1).putU32(1);
    return bytes.bytes();
}

TEST_CASE("shop loads native catalog without exports and refuses stale fallback",
          "[native-shop][native-assets]") {
    const auto root = test::scratchDirectory("native-shop");
    std::filesystem::create_directories(root / "ShpData");
    std::filesystem::create_directories(root / "shop");
    const auto native = root / "ShpData/sHoP.WaD";
    writeFile(native, catalogWad());
    writeTextFile(root / "shop/catalog.json", R"({"items":[{"texture":"LEGACY","description":"",
                  "scale":1,"type":0,"price":0,"amount":0}]})");
    ShopCatalog catalog;
    REQUIRE(catalog.loadRoot(root));
    REQUIRE(catalog.items().size() == 1);
    CHECK(catalog.items()[0].texture == "EXIT");
    CHECK(catalog.items()[0].description == "LEAVE");
    SECTION("truncated native does not use exported catalog") {
        writeFile(native, std::array<u8, 3>{});
    }
    SECTION("non-exit first entry fails shared validation") {
        writeFile(native, catalogWad(1, 50));
    }
    SECTION("paid exit fails shared validation") {
        writeFile(native, catalogWad(0, 50));
    }
    CHECK_FALSE(catalog.loadRoot(root));
    CHECK(catalog.items().empty());
}

TEST_CASE("retail shop catalog retains every exported field", "[native-shop][assets]") {
    const auto native = test::assetOrSkip("SHPDATA/SHOP.WAD");
    const char* reference = std::getenv("GDL_NATIVE_REFERENCE_DIR");
    if (reference == nullptr) {
        SKIP("Set GDL_NATIVE_REFERENCE_DIR to a fresh gdlunpack export");
    }
    ShopCatalog actual;
    ShopCatalog expected;
    REQUIRE(actual.loadRoot(native.parent_path().parent_path()));
    REQUIRE(expected.loadRoot(std::filesystem::path(reference)));
    REQUIRE(actual.items().size() == 34);
    REQUIRE(actual.items().size() == expected.items().size());
    for (usize i = 0; i < actual.items().size(); ++i) {
        CAPTURE(i);
        const auto& a = actual.items()[i];
        const auto& e = expected.items()[i];
        CHECK(a.texture == e.texture);
        CHECK(a.description == e.description);
        CHECK(a.scale == e.scale);
        CHECK(a.type == e.type);
        CHECK(a.price == e.price);
        CHECK(a.amount == e.amount);
    }
}
} // namespace
