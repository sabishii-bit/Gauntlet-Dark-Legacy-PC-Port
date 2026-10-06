#include <algorithm>
#include <array>
#include <bit>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/NativeDataAudit.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"

namespace {
using namespace gdl;

/** Standalone action tree with two translation keys, no model archive. */
std::vector<u8> actionFile(f32 lastPosition = 4) {
    test::ByteWriter bytes;
    bytes.putU16(1).putU16(8).putU32(24).putZeros(16);
    bytes.putText("ACTOR").putZeros(27).putU32(60);
    bytes.putU32(56).putU32(164).putU32(0).putU32(104).putU32(1).putU32(1);
    bytes.putText("ACTOR").putZeros(25).putU16(0);
    bytes.putText("WALK").putZeros(28).putU16(5).putU16(30);
    bytes.putU16(1).putU16(0).putU16(0).putU16(1).putU32(0);
    bytes.putText("ROOT").putZeros(28).putZeros(12);
    bytes.putU16(1).putU16(1).putU32(0).putU32(28).putS32(-1);
    bytes.putZeros(12).putU32(36).putU32(28).putU32(1).putU32(0);
    bytes.putU16(0x10).putU16(1).putU32(0);
    bytes.putU32(0x11).putU32(0).putU32(std::bit_cast<u32>(lastPosition));
    return bytes.bytes();
}

TEST_CASE("data audit exercises standalone action keys and their interpolated poses",
          "[asset-conformance][data-audit]") {
    const auto directory = test::scratchDirectory("audit-action-poses");
    const auto file = directory / "ANIM.PS2";
    writeFile(file, actionFile());
    const auto valid = auditNativeData(file);
    for (const auto& issue : valid.issues) {
        INFO(issue.record << ": " << issue.detail);
    }
    REQUIRE(valid.passed());
    CHECK(valid.kind == "animation");
    CHECK(valid.counts.at("tracks") == 1);
    CHECK(valid.counts.at("keys") == 2);
    CHECK(valid.counts.at("pose_samples") == 3);
    CHECK_FALSE(std::filesystem::exists(directory / "objects.ngc"));
    writeFile(file, actionFile(std::numeric_limits<f32>::infinity()));
    const auto invalid = auditNativeData(file);
    REQUIRE_FALSE(invalid.passed());
    CHECK(invalid.issues[0].record == "tree ACTOR sequence WALK");
    CHECK(invalid.issues[0].detail == "non-finite track values");
}

TEST_CASE("data audit does not mistake valid empty animations for malformed ones",
          "[asset-conformance][data-audit]") {
    const auto directory = test::scratchDirectory("audit-data-empty");
    const auto file = directory / "ANIM.PS2";
    CHECK_FALSE(auditNativeData(file).passed());
    writeFile(file, std::array<u8, 16>{});
    const auto empty = auditNativeData(file);
    REQUIRE(empty.passed());
    CHECK(empty.counts.at("trees") == 0);
    writeFile(file, std::array<u8, 1>{});
    CHECK_FALSE(auditNativeData(file).passed());
    CHECK_FALSE(auditNativeData(directory / "unknown.WAD").passed());
}

TEST_CASE("data audit decodes unreferenced sound samples and catches short PCM",
          "[asset-conformance][data-audit]") {
    const auto directory = test::scratchDirectory("audit-data-sound");
    const auto file = directory / "TEST.VBK";
    const std::array samples{test::NativeSoundSample{12000, std::vector<s16>(14, 0)},
                             test::NativeSoundSample{12000, std::vector<s16>(14, 0)}};
    test::writeNativeSoundBank(
        file, R"({"sounds":[{"name":"ONLY_FIRST","sequence":[{"sample":0}]}]})", samples);
    const auto valid = auditNativeData(file);
    REQUIRE(valid.passed());
    CHECK(valid.counts.at("samples") == 2);
    CHECK(valid.counts.at("pcm_samples") == 28);
    auto bytes = readFile(file);
    // Second DSP header's big-endian sample count says 15, but its only
    // 8-byte ADPCM frame can supply 14. The call table never uses this sample.
    constexpr usize kSecondCount = 20 + 8 + 48 + 96 + 8 + 48;
    bytes[kSecondCount + 3] = 15;
    writeFile(file, bytes);
    const auto invalid = auditNativeData(file);
    REQUIRE_FALSE(invalid.passed());
    CHECK(invalid.issues[0].record == "sample[1] SAMPLE");
}

TEST_CASE("data audit catches named calls with no bank sequence",
          "[asset-conformance][data-audit]") {
    const auto directory = test::scratchDirectory("audit-data-call");
    const auto file = directory / "TEST.VBK";
    const std::array samples{test::NativeSoundSample{12000, std::vector<s16>(14, 0)}};
    test::writeNativeSoundBank(file, R"({"sounds":[{"name":"CALL","sequence":[{"sample":0}]}]})",
                               samples);
    auto bytes = readFile(file);
    bytes[15] = 0; // zero calls; the directory still advertises one
    writeFile(file, bytes);
    const auto result = auditNativeData(file);
    REQUIRE_FALSE(result.passed());
    CHECK(result.issues[0].record == "call[0] CALL");
}

TEST_CASE("data audit reports missing WAD tables instead of accepting a partial player",
          "[asset-conformance][data-audit]") {
    const auto directory = test::scratchDirectory("audit-data-wad") / "PDATA";
    std::filesystem::create_directories(directory);
    test::ByteWriter bytes;
    bytes.putU32(16 + 0x180).putU32(1).putZeros(8);
    bytes.putU16(1).putU16(0).putZeros(8); // one effect, but no SFXX section
    for (usize i = 0; i < 12; ++i) {
        bytes.putU16(0xFFFF); // no moves
    }
    bytes.putZeros(16 + 0x180 - bytes.size());
    bytes.putText("TADP").putU32(16).putU32(1).putU32(1);
    const auto file = directory / "TEST.WAD";
    writeFile(file, bytes.bytes());
    const auto result = auditNativeData(file);
    REQUIRE_FALSE(result.passed());
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues[0].record == "PDAT");
}

TEST_CASE("each standalone native player action archive builds finite animation poses",
          "[asset-conformance][data-audit][assets]") {
    const auto root =
        test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2").parent_path().parent_path().parent_path();
    std::vector<std::string> classes;
    for (const auto& directory : std::filesystem::directory_iterator(root)) {
        const auto file = AssetLocator(directory.path()).find("ANIM/ANIM.PS2");
        if (!file) {
            continue;
        }
        CAPTURE(file->string());
        const auto result = auditNativeData(*file);
        for (const auto& issue : result.issues) {
            INFO(issue.record << ": " << issue.detail);
        }
        REQUIRE(result.passed());
        CHECK(result.counts.at("sequences") > 0);
        CHECK(result.counts.at("pose_samples") > result.counts.at("sequences"));
        classes.push_back(directory.path().filename().string());
    }
    std::ranges::sort(classes);
    CHECK(classes ==
          std::vector<std::string>{"ARC", "DWF", "JES", "KNI", "SOR", "VAL", "WAR", "WIZ"});
}
} // namespace
