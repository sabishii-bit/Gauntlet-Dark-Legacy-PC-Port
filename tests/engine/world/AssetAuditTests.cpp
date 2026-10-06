#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/world/AssetAudit.h"

#include "SampleLevel.h"

namespace {
using namespace gdl;

void writeCycle(const std::filesystem::path& directory, u16 frames) {
    test::ByteWriter bytes;
    bytes.putU16(0).putU16(8).putU32(0).putU32(1).putU32(24).putZeros(8);
    bytes.putU16(0xFFFF).putU16(0);
    bytes.putText("TORCHB").putZeros(26).putText("TORCH00").putZeros(25);
    bytes.putU32(3).putS32(-1).putU16(frames).putU16(0).putU32(1).putU32(0);
    writeFile(directory / "ANIM.PS2", bytes.bytes());
}
TEST_CASE("asset audits distinguish successful decoding from a complete binding context",
          "[asset-conformance][asset-audit]") {
    const auto directory = test::sampleLevel("audit-context");
    const auto decoded = auditAssets(directory, {}, false);
    CHECK(decoded.passed());
    CHECK_FALSE(decoded.dependenciesChecked);
    CHECK(decoded.models == 7);
    CHECK(decoded.images == 3);
    CHECK(decoded.externalReferences == 1);
    const auto incomplete = auditAssets(directory);
    CHECK_FALSE(incomplete.passed());
    REQUIRE(incomplete.issues.size() == 1);
    CHECK(incomplete.issues[0].record == "bitmap[3] TORCHB");
    TextureSet lender;
    REQUIRE(lender.load(test::sampleLender("audit-lender")));
    const std::array lenders{&lender};
    const auto complete = auditAssets(directory, lenders);
    CHECK(complete.dependenciesChecked);
    CHECK(complete.passed());
}

TEST_CASE("asset audits reject absent native inputs and corrupt lazy geometry",
          "[asset-conformance][asset-audit]") {
    const auto missing = auditAssets(test::scratchDirectory("audit-missing"));
    REQUIRE_FALSE(missing.passed());
    const auto directory = test::sampleLevel("audit-corrupt");
    auto bytes = readFile(directory / "objects.ngc");
    const usize firstSub = readU32LE(bytes, 84) + 16;
    // Keep all tables valid, but make the first geometry payload absent. Loading
    // the directory succeeds; only forcing the lazy mesh decoder exposes it.
    bytes[firstSub] = 0;
    bytes[firstSub + 1] = 0;
    writeFile(directory / "objects.ngc", bytes);
    ModelSet models;
    REQUIRE(models.load(directory));
    CHECK_FALSE(auditAssets(directory).passed());
}

TEST_CASE("empty native animation and model tables are valid rather than loader failures",
          "[asset-conformance][asset-audit]") {
    const auto directory = test::sampleLender("audit-empty-tables");
    test::ByteWriter bytes;
    bytes.putZeros(16);
    writeFile(directory / "ANIM.PS2", bytes.bytes());
    const auto result = auditAssets(directory);
    CHECK(result.passed());
    CHECK(result.models == 0);
    CHECK(result.trees == 0);
    CHECK(result.images == 4);
}

TEST_CASE("a native cycle must bind every frame rather than silently truncate",
          "[asset-conformance][asset-audit]") {
    const auto directory = test::sampleLevel("audit-cycle");
    TextureSet lender;
    REQUIRE(lender.load(test::sampleLender("audit-cycle-lender")));
    const std::array lenders{&lender};
    writeCycle(directory, 2);
    CHECK(auditAssets(directory, lenders).passed());
    writeCycle(directory, 4);
    const auto truncated = auditAssets(directory, lenders);
    CHECK_FALSE(truncated.passed());
    REQUIRE(truncated.issues.size() == 1);
    CHECK(truncated.issues[0].record == "texmod[0] TORCHB");
    CHECK(truncated.issues[0].detail.find("requests 4 frames") != std::string::npos);
    CHECK(auditAssets(directory, {}, false).passed());
}

TEST_CASE("dependency reviews remain visible without becoming decode failures",
          "[asset-conformance][asset-audit]") {
    AssetAuditResult result;
    result.issues.push_back({"tree WEAP node ROOT", "unresolved model", true});
    CHECK(result.passed());
    result.dependenciesChecked = true;
    CHECK_FALSE(result.passed());
    result.dependenciesChecked = false;
    result.issues.push_back({"model[1]", "corrupt geometry", false});
    CHECK_FALSE(result.passed());
}
} // namespace
