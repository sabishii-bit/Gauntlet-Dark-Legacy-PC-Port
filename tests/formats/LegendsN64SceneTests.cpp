#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/WorldCamera.h"

#include "TestSupport.h"
#include "formats/LegendsMap.h"
#include "formats/LegendsN64Scene.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
void text(std::vector<u8>& bytes, usize at, std::string_view value) {
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

struct NativeSequence {
    static constexpr usize kBody = 8;
    static constexpr usize kSequence = 40;
    static constexpr usize kList = 56;
    static constexpr usize kGroup = 64;
    std::vector<u8> bytes = std::vector<u8>(88 + 4 * 12);
    std::vector<LegendsN64Model> models = std::vector<LegendsN64Model>(2);
    NativeSequence() {
        models[0].name = "BASE";
        models[1].name = "LID";
        put32(bytes, 0, 88);
        put32(bytes, 4, 4);
        const std::array<std::string_view, 4> tags{"BODY", "SEQS", "SLST", "GRPS"};
        const std::array<u32, 4> offsets{kBody, kSequence, kList, kGroup};
        const std::array<u32, 4> counts{32, 16, 1, 1};
        for (usize i = 0; i < tags.size(); ++i) {
            text(bytes, 88 + i * 12, tags[i]);
            put32(bytes, 92 + i * 12, offsets[i]);
            put32(bytes, 96 + i * 12, counts[i]);
        }
        put16(bytes, kBody, 0x2000); // model zero, following position block
        put16(bytes, kBody + 2, 0xffff);
        put16(bytes, kBody + 4, 2); // child skips position block
        put16(bytes, kBody + 8, 128);
        put16(bytes, kBody + 10, 64);
        put16(bytes, kBody + 12, 32);
        put16(bytes, kBody + 16, 1);
        put16(bytes, kBody + 18, 0xffff);
        put16(bytes, kBody + 20, 0xffff);
        bytes[kSequence + 1] = 16;
        text(bytes, kList, "TEST");
        put32(bytes, kList + 4, kSequence);
        text(bytes, kGroup, "CONTAINER");
        put32(bytes, kGroup + 16, 1);
        put32(bytes, kGroup + 20, kList);
    }
};

struct TransformSequence {
    std::vector<u8> bytes = std::vector<u8>(248 + 48);
    std::vector<LegendsN64Model> models = std::vector<LegendsN64Model>(1);
    std::vector<u8> program = std::vector<u8>(0x33398);
    TransformSequence(u8 mode) {
        models[0].name = "WING";
        put32(bytes, 0, 248);
        put32(bytes, 4, 4);
        const std::array<std::string_view, 4> tags{"BODY", "SEQS", "SLST", "GRPS"};
        const std::array<u32, 4> offsets{8, 16, 216, 224};
        const std::array<u32, 4> counts{8, 200, 1, 1};
        for (usize i = 0; i < 4; ++i) {
            text(bytes, 248 + 12 * i, tags[i]);
            put32(bytes, 252 + 12 * i, offsets[i]);
            put32(bytes, 256 + 12 * i, counts[i]);
        }
        put16(bytes, 10, 0xffff);
        put16(bytes, 12, 0xffff);
        bytes[16] = 1;
        bytes[17] = 16;
        put16(bytes, 22, 3);
        bytes[24] = mode | 9;
        put16(bytes, 26, 4);    // payload32
        put16(bytes, 32, 8192); // initial pi/2
        bytes[38] = 0x39;       // Y-X-Z order, animated X and Y
        usize cursor = 39;
        if (mode == 0xc0) {
            bytes[cursor++] = 0x60; // frames1,2, high bit first
        }
        for (int frame = 1; frame < 3; ++frame) {
            if (mode == 0x80) {
                bytes[cursor++] = 1;
            }
            bytes[cursor++] = 1;
            bytes[cursor++] = 2;
        }
        text(bytes, 216, "IDLE");
        put32(bytes, 220, 16);
        text(bytes, 224, "POWERUP");
        put32(bytes, 240, 1);
        put32(bytes, 244, 216);
        put32(program, 0x32b90 + 4, std::bit_cast<u32>(0.25f));
        put32(program, 0x32b90 + 8, std::bit_cast<u32>(-0.5f));
        put32(program, 0x32f98 + 4, std::bit_cast<u32>(0.5f));
    }
};

std::vector<u8> externalTransform(TransformSequence& fixture) {
    std::vector<u8> dynamic(216);
    std::copy(fixture.bytes.begin() + 16, fixture.bytes.begin() + 216, dynamic.begin() + 16);
    put32(fixture.bytes, 4, 5);
    put32(fixture.bytes, 248 + 12 + 8, 0); // no inline SEQS bytes
    put32(fixture.bytes, 220, 0x10000010);
    fixture.bytes.resize(248 + 5 * 12);
    text(fixture.bytes, 296, "DSIZ");
    put32(fixture.bytes, 300, 16);
    put32(fixture.bytes, 304, 200);
    return dynamic;
}

} // namespace

TEST_CASE("N64 transform tracks decode all native key encodings and rotation order",
          "[legends][n64][scene]") {
    for (const u8 mode : std::array<u8, 3>{0, 0x80, 0xc0}) {
        TransformSequence fixture(mode);
        const auto decoded =
            decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program);
        REQUIRE(decoded.warnings.empty());
        const auto& sequence = decoded.animation.trees[0].sequences[0];
        REQUIRE(sequence.tracks.size() == 1);
        CHECK(sequence.frameCount == 3);
        CHECK(sequence.frameRate == 30);
        CHECK(sequence.repeats);
        const auto& track = sequence.tracks[0];
        CHECK(track.flags == (NodeTrack::kLinearAngles | NodeTrack::kYawPitchRoll | 7));
        CHECK(track.frames == std::vector<u16>{0, 1, 2});
        REQUIRE(track.values.size() == 9);
        CHECK(track.values[0] == Catch::Approx(kHalfPi));
        CHECK(track.values[3] == Catch::Approx(kHalfPi + 0.25f));
        CHECK(track.values[4] == Catch::Approx(0.5f));
        CHECK(track.values[6] == Catch::Approx(kHalfPi + 0.5f));
        CHECK(track.values[7] == Catch::Approx(1.0f));
        const auto missing = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
        CHECK(missing.animation.trees[0].sequences[0].tracks.empty());
        CHECK_FALSE(missing.warnings.empty());
    }
}

TEST_CASE("Indexed N64 BODY nodes expose native model suffix attachment names",
          "[legends][n64][scene][native-traps]") {
    NativeSequence fixture;
    fixture.models[0].name = "TENT_MOAT_RDYHITNODE";
    fixture.models[0].partName = "HITNODE";
    const auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    REQUIRE(decoded.animation.trees.size() == 1);
    const auto& nodes = decoded.animation.trees.front().nodes;
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].name == "HITNODE");
    CHECK(nodes[0].object == "TENT_MOAT_RDYHITNODE");
    CHECK(nodes[1].name == "LID");
    CHECK(nodes[1].parent == 0);
    CHECK(nodes[0].position == Vec3{-2, 1, .5f});
}

TEST_CASE("N64 dynamic sequences use the native stream without rebinding their BODY",
          "[legends][n64][scene][dynamic-sequence]") {
    TransformSequence fixture(0xc0);
    const auto inlineResult =
        decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program);
    const auto dynamic = externalTransform(fixture);
    const auto& before = dynamic;
    const auto missing = decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program);
    CHECK(missing.animation.trees.empty());
    REQUIRE(missing.warnings.size() == 1);
    CHECK(missing.warnings[0].find("externally streamed") != std::string::npos);
    const auto decoded =
        decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program, dynamic);
    REQUIRE(decoded.warnings.empty());
    REQUIRE(decoded.animation.trees.size() == 1);
    const auto& tree = decoded.animation.trees[0];
    CHECK(tree.name == "POWERUP:IDLE");
    CHECK(tree.nodes[0].object == "WING");
    const auto& sequence = tree.sequences[0];
    REQUIRE(sequence.tracks.size() == 1);
    CHECK(sequence.tracks[0].values ==
          inlineResult.animation.trees[0].sequences[0].tracks[0].values);
    CHECK(sequence.tracks[0].frames ==
          inlineResult.animation.trees[0].sequences[0].tracks[0].frames);
    CHECK(dynamic == before);
}

TEST_CASE("N64 dynamic sequences reject unavailable stream extents and escaped tracks",
          "[legends][n64][scene][dynamic-sequence]") {
    TransformSequence fixture(0xc0);
    auto dynamic = externalTransform(fixture);
    SECTION("missing DSIZ") {
        text(fixture.bytes, 296, "NONE");
    }
    SECTION("too-small scratch buffer") {
        put32(fixture.bytes, 304, 7);
    }
    SECTION("oversized scratch buffer") {
        put32(fixture.bytes, 304, 0xFFFFFFFF);
    }
    SECTION("stream offset past end") {
        put32(fixture.bytes, 220, 0x13FFFFFF);
    }
    SECTION("truncated stream header") {
        dynamic.resize(20);
    }
    SECTION("truncated track list") {
        dynamic.resize(27);
    }
    SECTION("transform outside streamed buffer") {
        put32(fixture.bytes, 304, 16);
    }
    SECTION("payload outside stream") {
        put16(dynamic, 26, 0xFFFF);
    }
    CHECK_THROWS_AS(
        decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program, dynamic),
        FormatError);
}

TEST_CASE("native Sumner sequences retain their stream transforms and native clocks",
          "[legends][n64][scene][native-assets][dynamic-sequence]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native N64 extraction is not installed");
    }
    const auto models = decodeLegendsN64Models(readFile(root / "sum/objects.wad"));
    const auto decoded = decodeLegendsN64Sequences(readFile(root / "wiz/anim/seq.wad"), models,
                                                   readFile(root / "_program/game.bin"),
                                                   readFile(root / "wiz/anim/dynseq.rom"));
    REQUIRE(decoded.animation.trees.size() == 117);
    // The unused throwing moves have native clock20 (period37.5), outside
    // the common integer-period format. Tower's INIT/idle moves all use16.
    const std::set<std::string> unusedThrowingMoves{"WIZMOVES:TH1",  "WIZMOVES:TH2",
                                                    "WIZMOVES:TH1R", "WIZMOVES:TH1S",
                                                    "WIZMOVES:TH2R", "WIZMOVES:TH2S"};
    REQUIRE(decoded.warnings.size() == unusedThrowingMoves.size());
    for (const auto& warning : decoded.warnings) {
        INFO(warning);
        const auto prefix = warning.substr(0, warning.find(": "));
        CHECK(unusedThrowingMoves.contains(prefix));
        CHECK(warning.find("27 native animation tracks") != std::string::npos);
    }
    for (const std::string_view name : {"INIT", "LKDN", "READ", "PAGE", "CNJ1", "CNJ2", "CNJ3"}) {
        CAPTURE(name);
        const auto expected = "WIZMOVES:" + std::string(name);
        CHECK(std::none_of(
            decoded.warnings.begin(), decoded.warnings.end(),
            [&](const auto& warning) { return warning.starts_with(expected + ": "); }));
        const auto tree =
            std::find_if(decoded.animation.trees.begin(), decoded.animation.trees.end(),
                         [&](const auto& entry) { return entry.name == expected; });
        REQUIRE(tree != decoded.animation.trees.end());
        CHECK(tree->nodes.size() >= 20);
        CHECK(std::count_if(tree->nodes.begin(), tree->nodes.end(),
                            [](const auto& node) { return !node.object.empty(); }) == 19);
        CHECK(std::any_of(tree->nodes.begin(), tree->nodes.end(), [](const auto& node) {
            return node.name == "HEAD" && node.object == "HEAD";
        }));
        CHECK(std::any_of(tree->nodes.begin(), tree->nodes.end(), [](const auto& node) {
            return node.name == "LEGL3" && node.object.empty();
        }));
        REQUIRE(tree->sequences.size() == 1);
        const auto& sequence = tree->sequences[0];
        CHECK(sequence.frameRate == 30);
        CHECK_FALSE(sequence.tracks.empty());
        if (name == "INIT") {
            CHECK(sequence.frameCount == 1);
        } else {
            CHECK(sequence.frameCount > 1);
            bool changes = false;
            for (const auto& track : sequence.tracks) {
                const auto channels = track.channelCount();
                for (usize frame = 1; frame < track.frames.size(); ++frame) {
                    for (usize channel = 0; channel < channels; ++channel) {
                        changes |=
                            track.values[channel] != track.values[frame * channels + channel];
                    }
                }
            }
            CHECK(changes);
        }
    }
}

TEST_CASE("N64 static transforms, scale and translation retain native units and clocks",
          "[legends][n64][scene]") {
    TransformSequence fixture(0x40);
    fixture.bytes[24] = 0x4b; // static translation
    put16(fixture.bytes, 32, 64);
    put16(fixture.bytes, 34, 128);
    put16(fixture.bytes, 36, static_cast<u16>(-64));
    fixture.bytes[17] = 32;
    fixture.bytes[18] = 1;
    auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    const auto& sequence = decoded.animation.trees[0].sequences[0];
    REQUIRE(sequence.tracks.size() == 1);
    CHECK(sequence.frameRate == 60);
    CHECK_FALSE(sequence.repeats);
    CHECK(sequence.tracks[0].values == std::vector<f32>{-1, 2, -1, -1, 2, -1, -1, 2, -1});
    fixture.bytes[24] = 13;
    put16(fixture.bytes, 32, 64);
    put16(fixture.bytes, 34, 64);
    put16(fixture.bytes, 36, 64);
    fixture.bytes[38] = 0x13; // uniform scale animated by X
    fixture.bytes[39] = 1;
    fixture.bytes[40] = 1;
    decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program);
    CHECK(decoded.animation.trees[0].sequences[0].tracks[0].values ==
          std::vector<f32>{1, 1, 1, 1.5f, 1.5f, 1.5f, 2, 2, 2});
}

TEST_CASE("N64 transform readers reject malformed intervals, final keys and codebooks",
          "[legends][n64][scene]") {
    TransformSequence fixture(0x80);
    SECTION("zero interval") {
        fixture.bytes[39] = 0;
    }
    SECTION("interval outside clock") {
        fixture.bytes[39] = 255;
    }
    SECTION("missing final bitmap key") {
        fixture.bytes[24] = 0xc9;
        fixture.bytes[39] = 0x40;
    }
    SECTION("truncated table") {
        fixture.program.resize(20);
    }
    SECTION("nonfinite table") {
        put32(fixture.program, 0x32b90, 0x7f800000);
    }
    SECTION("escaping payload") {
        put16(fixture.bytes, 26, 0xffff);
    }
    CHECK_THROWS_AS(decodeLegendsN64Sequences(fixture.bytes, fixture.models, fixture.program),
                    FormatError);
}

TEST_CASE("N64 native speed bytes retain their periods when bound to AnimationPlayer",
          "[legends][n64][scene][animation]") {
    for (const u8 period : std::array<u8, 4>{8, 16, 24, 32}) {
        TransformSequence fixture(0x40);
        fixture.bytes[17] = period;
        put16(fixture.bytes, 22, 120);
        auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
        const auto animation = AnimationSet::fromDecoded(std::move(decoded.animation));
        const auto& sequence = animation.tree(0).sequences[0];
        AnimationPlayer player;
        player.start(sequence, 0);
        CHECK(player.secondsPerFrame() == Catch::Approx(static_cast<f32>(period) / 480.0f));
        player.advance(0.5f, true);
        CHECK(player.presentationFrame() == Catch::Approx(240.0f / static_cast<f32>(period)));
    }
}

TEST_CASE("N64 body facing and model sorting map only verified material policies",
          "[legends][n64][scene]") {
    NativeSequence fixture;
    put16(fixture.bytes, NativeSequence::kBody, 0x6000); // position + full camera facing
    fixture.models[0].flags = 0x20;
    fixture.models[1].flags = 0x11;
    auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    CHECK(decoded.animation.trees[0].nodes[0].objectFlags ==
          ((4U << object_flags::kFacingShift) | object_flags::kSorted));
    CHECK(decoded.animation.trees[0].nodes[1].objectFlags == object_flags::kSorted);
    fixture.models[0].flags = 1; // Alpha alone does not disable writes or force sorting.
    decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    CHECK(decoded.animation.trees[0].nodes[0].objectFlags == (4U << object_flags::kFacingShift));
}

TEST_CASE("N64 fixtures preserve native placement and template identity", "[legends][n64][scene]") {
    std::vector<u8> placement(108);
    std::vector<u8> container(48);
    put32(placement, 24, 92);
    put16(placement, 50, 1);
    put16(placement, 92, static_cast<u16>(-12));
    put16(placement, 94, 20);
    put16(placement, 96, 28);
    put16(placement, 98, 48);
    put16(placement, 100, 0x1234);
    placement[102] = 9;
    placement[103] = 3;
    put16(placement, 104, 0x2111);
    placement[106] = 4;
    put32(container, 0, 1);
    put16(container, 4, 0x1234);
    put32(container, 8, 0x102);
    text(container, 12, "CBRC");
    text(container, 16, "CBRA");
    text(container, 20, "CBRD");
    LegendsN64Templates templates;
    templates.containers = container;
    const auto fixtures = decodeLegendsN64Fixtures(placement, templates);
    REQUIRE(fixtures.size() == 1);
    const auto& item = fixtures.front();
    CHECK(item.kind == LegendsN64FixtureKind::Container);
    CHECK(item.position == Vec3{-3.0f, 5.0f, 7.0f});
    CHECK(item.facingRadians == 1.5f);
    CHECK(item.minimumPlayers == 3);
    CHECK(item.placementFlags == 9);
    CHECK(item.templateFlags == 0x102);
    CHECK(item.sequence == "CBRC");
    CHECK(item.activeSequence == "CBRA");
    CHECK(item.deathSequence == "CBRD");
    CHECK(item.containedPowerup == 0x2111);
    CHECK(item.quantity == 4);
    put16(placement, 100, 0xffff);
    CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
    put32(placement, 24, 104);
    CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
}

TEST_CASE("N64 generator placements select the authored tier and flag actor replacements",
          "[legends][n64][scene]") {
    std::vector<u8> placement(108);
    std::vector<u8> generators(116);
    put32(placement, 8, 92);
    put16(placement, 40, 1);
    put16(placement, 100, 0x1112);
    placement[102] = 2;
    placement[105] = 2;
    placement[106] = 4;
    put32(generators, 0, 2);
    for (usize i = 0; i < 2; ++i) {
        put16(generators, 4 + i * 56, 0x1112);
        generators[14 + i * 56] = static_cast<u8>(i + 1);
        text(generators, 16 + i * 56, i == 0 ? "GEN1" : "GEN2");
    }
    LegendsN64Templates templates;
    templates.generators = generators;
    const auto fixtures = decodeLegendsN64Fixtures(placement, templates);
    REQUIRE(fixtures.size() == 1);
    CHECK(fixtures[0].sequence == "GEN2");
    CHECK(fixtures[0].generatorTier == 2);
    CHECK(fixtures[0].minimumPlayers == 4);
    CHECK(fixtures[0].actorInsteadOfGenerator);
}

TEST_CASE("N64 initial pickup selectors use native cursors and skip inactive placements",
          "[legends][n64][scene]") {
    std::vector<u8> placement(92 + 4 * 12);
    put32(placement, 0, 92);
    put16(placement, 36, 4);
    for (usize i = 0; i < 4; ++i) {
        put16(placement, 92 + i * 12 + 6, 0x2201);
    }
    placement[100] = 2; // Hidden at threshold1; must not consume a selector step.
    std::vector<u8> powerups(4 + 3 * 28);
    put32(powerups, 0, 3);
    put16(powerups, 4, 0x2201);
    text(powerups, 12, "POTR");
    put16(powerups, 32, 0x0101);
    text(powerups, 40, "PRED");
    put16(powerups, 60, 0x0102);
    put16(powerups, 62, 3);
    put32(powerups, 64, 0x12008);
    text(powerups, 68, "PGRN");
    put32(powerups, 80, 9);
    put32(powerups, 84, 0x4010);
    std::vector<u8> selectors(4 + 3 * 8);
    put32(selectors, 0, 3);
    text(selectors, 4, "POTR");
    put16(selectors, 8, 1);
    put16(selectors, 10, 1);
    put16(selectors, 16, 1);
    put16(selectors, 18, 2);
    text(selectors, 20, "STOP");
    LegendsN64Templates templates;
    templates.powerups = powerups;
    templates.randoms = selectors;
    const auto original = powerups;
    const auto fixtures = decodeLegendsN64Fixtures(placement, templates);
    REQUIRE(fixtures.size() == 4);
    CHECK(fixtures[0].sequence == "POTR");
    CHECK(fixtures[1].sequence == "PGRN");
    CHECK(fixtures[2].sequence == "PRED");
    CHECK(fixtures[3].sequence == "PGRN");
    CHECK(fixtures[1].templateKey == 0x2201);
    CHECK(fixtures[1].resolvedTemplateKey == 0x0102);
    CHECK(fixtures[1].resolvedTemplateFlags == 0x12008);
    CHECK(fixtures[1].templateAmount == 3);
    CHECK(fixtures[1].pickupLimit == 9);
    CHECK(fixtures[1].pickupSound == 0x4010);
    CHECK(fixtures[1].pickupRadius == 0.5f);
    CHECK(fixtures[1].pickupHeight == 3.0f);
    CHECK(powerups == original);
    CHECK(decodeLegendsN64Fixtures(placement, templates, 2)[1].sequence == "PRED");
    SECTION("unknown selected template") {
        put16(selectors, 18, 99);
        CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
    }
    SECTION("missing STOP") {
        put32(selectors, 20, 0);
        CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
    }
    SECTION("recursive selector") {
        put16(selectors, 16, 0x22);
        put16(selectors, 18, 1);
        CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
    }
    SECTION("truncated PRND table") {
        templates.randoms = templates.randoms.first(12);
        CHECK_THROWS_AS(decodeLegendsN64Fixtures(placement, templates), FormatError);
    }
}

TEST_CASE("N64 specialized pickup libraries follow native kind subtype and flag dispatch",
          "[legends][n64][scene]") {
    struct Case {
        u16 key;
        u32 flags;
        std::string_view archive;
    };
    const std::array cases{
        Case{0x150d, 0x8210, "items/s/r13"},  Case{0x2707, 0x1a4, "items/s/ctig"},
        Case{0x2705, 0x1a2, "items/s/cfal"},  Case{0x2704, 0x1a1, "items/s/cmin"},
        Case{0x2706, 0x1a3, "items/s/cjac"},  Case{0x2708, 0x1a5, "items/s/csum"},
        Case{0x2901, 0x8201c0, "items/s/li"}, Case{0x2902, 0x8201d0, "items/s/lf"},
        Case{0x2903, 0x8201e0, "items/s/ls"}, Case{0x2904, 0x8201f0, "items/s/lj"},
        Case{0x2905, 0x20200, "items/s/lss"}, Case{0x2a01, 0x220, "items/s/bossm"},
        Case{0x2b01, 0, "items/s/sm"},        Case{0x2b04, 0, "items/s/st"}};
    for (const auto& item : cases) {
        INFO(item.archive);
        std::vector<u8> placement(104);
        put32(placement, 0, 92);
        put16(placement, 36, 1);
        put16(placement, 98, item.key);
        std::vector<u8> powerups(32);
        put32(powerups, 0, 1);
        put16(powerups, 4, item.key);
        put32(powerups, 8, item.flags);
        text(powerups, 12, "TEST");
        LegendsN64Templates templates;
        templates.powerups = powerups;
        auto fixtures = decodeLegendsN64Fixtures(placement, templates);
        REQUIRE(fixtures.size() == 1);
        CHECK(fixtures[0].archive == item.archive);
        const auto archives = legendsN64FixtureArchives("mount1", fixtures);
        CHECK(archives.back() == item.archive);
        fixtures.push_back(fixtures[0]);
        CHECK(legendsN64FixtureArchives("mount1", fixtures) == archives);
        for (auto& fixture : fixtures) {
            fixture.minimumPlayers = 2;
        }
        CHECK(legendsN64FixtureArchives("mount1", fixtures) == legendsN64FixtureArchives("mount1"));
    }
}

TEST_CASE("N64 fixtures project onto native floors within their initializer search window",
          "[legends][n64][scene]") {
    LegendsMapCollisionTriangle floor;
    floor.vertices = {Vec3{-10, 2, -10}, Vec3{0, 2, 10}, Vec3{10, 2, -10}};
    floor.floor = true;
    std::vector<LegendsMapCollisionTriangle> collision{floor};
    LegendsN64Fixture fixture;
    fixture.kind = LegendsN64FixtureKind::Container;
    fixture.position = {0, 2.5f, 0};
    CHECK(legendsN64FixturePosition(fixture, collision).y == Catch::Approx(2.2f));
    fixture.groundProjected = false;
    CHECK(legendsN64FixturePosition(fixture, collision) == fixture.position);
    fixture.groundProjected = true;
    fixture.position.y = -10;
    CHECK(legendsN64FixturePosition(fixture, collision).y == Catch::Approx(2.2f));
    fixture.kind = LegendsN64FixtureKind::Generator;
    CHECK(legendsN64FixturePosition(fixture, collision) == fixture.position);
    fixture.position.y = 20;
    CHECK(legendsN64FixturePosition(fixture, collision) == fixture.position);
    fixture.position.y = 2.5f;
    collision[0].floor = false;
    CHECK(legendsN64FixturePosition(fixture, collision) == fixture.position);
    collision[0].floor = true;
    collision[0].vertices[1].y = 4;
    CHECK(legendsN64FixturePosition(fixture, collision).y == Catch::Approx(3.2f));
    fixture.position.x = 30;
    CHECK(legendsN64FixturePosition(fixture, collision) == fixture.position);
}

TEST_CASE("N64 sequence BODY hierarchies decode their own model slots and signed translations",
          "[legends][n64][scene]") {
    NativeSequence fixture;
    const auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    REQUIRE(decoded.animation.trees.size() == 1);
    const auto& tree = decoded.animation.trees[0];
    CHECK(tree.name == "CONTAINER:TEST");
    REQUIRE(tree.nodes.size() == 2);
    CHECK(tree.nodes[0].object == "BASE");
    CHECK(tree.nodes[0].position == Vec3{-2.0f, 1.0f, 0.5f});
    CHECK(tree.nodes[0].parent == -1);
    CHECK(tree.nodes[1].object == "LID");
    CHECK(tree.nodes[1].parent == 0);
    CHECK(decoded.warnings.empty());
    put16(fixture.bytes, NativeSequence::kBody + 18, 0);
    CHECK_THROWS_AS(decodeLegendsN64Sequences(fixture.bytes, fixture.models), FormatError);
}

TEST_CASE("N64 BODY projected-yaw flags use the explicit native camera mode",
          "[legends][n64][scene][facing]") {
    NativeSequence fixture;
    put16(fixture.bytes, NativeSequence::kBody, 0xa000);
    const auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    REQUIRE(decoded.warnings.empty());
    REQUIRE(decoded.animation.trees.size() == 1);
    CHECK((decoded.animation.trees[0].nodes[0].objectFlags >> 24U) ==
          CameraFrame::kFacingProjectedYaw);
    // Native invisible group mode2 has different orientation and stays explicit
    // until it is decoded; do not silently alias it to mode1.
    put16(fixture.bytes, NativeSequence::kBody, 0xa800);
    const auto group = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    REQUIRE(group.warnings.size() == 1);
    CHECK(group.warnings[0].starts_with("CONTAINER:TEST: "));
    CHECK((group.animation.trees[0].nodes[0].objectFlags >> 24U) == 0);
}

TEST_CASE("N64 named BODY models resolve only inside the supplied appearance archive",
          "[legends][n64][scene][named-body]") {
    NativeSequence fixture;
    std::vector<u8> bytes(156);
    std::copy_n(fixture.bytes.begin(), 88, bytes.begin());
    std::copy(fixture.bytes.begin() + 88, fixture.bytes.end(), bytes.begin() + 96);
    fixture.bytes = std::move(bytes);
    put32(fixture.bytes, 0, 96);
    put32(fixture.bytes, 4, 5);
    text(fixture.bytes, 144, "BDAT");
    put32(fixture.bytes, 148, 88);
    put32(fixture.bytes, 152, 8);
    text(fixture.bytes, 88, "LID");
    put16(fixture.bytes, NativeSequence::kBody, 0x21ff);
    put16(fixture.bytes, NativeSequence::kBody + 6, 22);
    const auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    REQUIRE(decoded.warnings.empty());
    REQUIRE(decoded.animation.trees.size() == 1);
    CHECK(decoded.animation.trees[0].nodes[0].name == "LID");
    CHECK(decoded.animation.trees[0].nodes[0].object == "LID");
    CHECK(decoded.animation.trees[0].nodes[0].position == Vec3{-2, 1, 0.5f});
    fixture.models[1].name = "OTHER";
    const auto missing = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    CHECK(missing.animation.trees[0].nodes[0].object.empty());
    CHECK(missing.animation.trees[0].nodes[1].object == "OTHER");
    CHECK(missing.animation.trees[0].nodes[1].parent == 0);
    SECTION("name outside BDAT") {
        put16(fixture.bytes, NativeSequence::kBody + 6, 21);
    }
    SECTION("unterminated name") {
        std::fill(fixture.bytes.begin() + 88, fixture.bytes.begin() + 96, static_cast<u8>('X'));
    }
    CHECK_THROWS_AS(decodeLegendsN64Sequences(fixture.bytes, fixture.models), FormatError);
}

TEST_CASE("N64 sequence parser rejects escaping groups, meshes and truncated position blocks",
          "[legends][n64][scene]") {
    NativeSequence fixture;
    SECTION("escaping SLST") {
        put32(fixture.bytes, NativeSequence::kGroup + 20, 4);
    }
    SECTION("missing model") {
        put16(fixture.bytes, NativeSequence::kBody, 42);
    }
    SECTION("missing position") {
        put16(fixture.bytes, NativeSequence::kSequence + 4, 3);
        put16(fixture.bytes, NativeSequence::kBody + 24, 0x2000);
    }
    CHECK_THROWS_AS(decodeLegendsN64Sequences(fixture.bytes, fixture.models), FormatError);
}

TEST_CASE("N64 shared texture sequences retain native 30Hz fractional timing",
          "[legends][n64][scene]") {
    std::vector<u8> bytes(120);
    put32(bytes, 0, 72);
    put32(bytes, 4, 4);
    const std::array<std::string_view, 4> tags{"BODY", "SEQS", "SLST", "GRPS"};
    const std::array<u32, 4> offsets{8, 16, 40, 48};
    const std::array<u32, 4> counts{8, 24, 1, 1};
    for (usize i = 0; i < 4; ++i) {
        text(bytes, 72 + i * 12, tags[i]);
        put32(bytes, 76 + i * 12, offsets[i]);
        put32(bytes, 80 + i * 12, counts[i]);
    }
    bytes[16] = 1;
    bytes[17] = 16;
    put16(bytes, 20, 0xffff);
    put16(bytes, 22, 4);
    bytes[24] = 7;
    bytes[25] = 0xff;
    put16(bytes, 26, 3);
    put16(bytes, 28, 2);
    put16(bytes, 30, 3);
    bytes[32] = 0xff;
    bytes[33] = 4;
    bytes[34] = 1;
    bytes[35] = 32;
    text(bytes, 40, "TEST");
    put32(bytes, 44, 16);
    text(bytes, 48, "!");
    put32(bytes, 64, 1);
    put32(bytes, 68, 40);
    const auto decoded = decodeLegendsN64Sequences(bytes, {});
    REQUIRE(decoded.animation.textureAnimations.size() == 1);
    const auto& animation = decoded.animation.textureAnimations[0];
    CHECK(animation.texture == 2);
    CHECK(animation.source == 3);
    CHECK(animation.frames == 4);
    CHECK(animation.start == 1);
    CHECK(animation.rate == 2); // Native denominator32 = 15 frames per second.
    CHECK(animation.flag == -1);
    CHECK(decoded.warnings.empty());
    // '!+' is started at archive load even outside group '!'. Ordinary named
    // sequences in the same group must not become free-running implicitly.
    text(bytes, 48, "POWERUP");
    CHECK(decodeLegendsN64Sequences(bytes, {}).animation.textureAnimations.empty());
    put32(bytes, 40, 0x212b);
    const auto startup = decodeLegendsN64Sequences(bytes, {});
    REQUIRE(startup.animation.textureAnimations.size() == 1);
    CHECK(startup.animation.textureAnimations[0].flag == -1);
    CHECK(startup.animation.textureAnimations[0].texture == 2);
    CHECK(startup.warnings.empty());
    bytes[35] = 15;
    CHECK(decodeLegendsN64Sequences(bytes, {}).animation.textureAnimations.empty());
    bytes[35] = 0;
    CHECK_THROWS_AS(decodeLegendsN64Sequences(bytes, {}), FormatError);
}

TEST_CASE("N64 item archive routes follow native realm scope and stable order",
          "[legends][n64][scene]") {
    const auto mount = legendsN64FixtureArchives("MoUnT1");
    REQUIRE(mount.size() == 13);
    CHECK(mount.front() == "itemanim/generic");
    CHECK(mount[1] == "itemanim/hires");
    CHECK(mount[2] == "levels/effects");
    CHECK(mount[3] == "items/c/redbar");
    CHECK(mount.back() == "items/mount");
    CHECK(std::find(mount.begin(), mount.end(), "items/c/saw") == mount.end());
    const auto ice = legendsN64FixtureArchives("ice1");
    CHECK(std::find(ice.begin(), ice.end(), "items/c/saw") != ice.end());
    CHECK(std::find(ice.begin(), ice.end(), "items/c/gchest") == ice.end());
    CHECK(legendsN64FixtureArchives("hell1").back() == "items/boss");
    CHECK(legendsN64FixtureArchives("secret1").back() == "items/castle");
    CHECK(legendsN64FixtureArchives("../mount1").empty());
    CHECK(legendsN64FixtureArchives("unknown1").empty());
    CHECK(legendsN64FixtureArchives("mount").empty());
}

TEST_CASE("N64 Castle tentacle archives follow native ordinals rather than directory suffixes",
          "[legends][n64][scene][native-traps]") {
    for (const auto* level : {"castle1", "castle6", "CASTLE6"}) {
        CAPTURE(level);
        const auto archives = legendsN64FixtureArchives(level);
        REQUIRE(archives.size() > 4);
        CHECK(archives[3] == "items/castle/tent");
        CHECK(archives[4] == "items/c/redbar");
        CHECK(std::ranges::count(archives, std::string_view("items/castle/tent")) == 1);
    }
    for (const auto* level : {"castle3", "castle4", "castle5"}) {
        CAPTURE(level);
        const auto archives = legendsN64FixtureArchives(level);
        CHECK(std::ranges::find(archives, std::string_view("items/castle/tent")) == archives.end());
    }
}

TEST_CASE("N64 repeated archive-load IDs retain every independent texture cycle",
          "[legends][n64][scene][shared-texture-sequence]") {
    std::vector<u8> bytes(144);
    put32(bytes, 0, 96);
    put32(bytes, 4, 4);
    const std::array<std::string_view, 4> tags{"BODY", "SEQS", "SLST", "GRPS"};
    const std::array<u32, 4> offsets{8, 8, 56, 72};
    const std::array<u32, 4> counts{0, 48, 2, 1};
    for (usize i = 0; i < 4; ++i) {
        text(bytes, 96 + i * 12, tags[i]);
        put32(bytes, 100 + i * 12, offsets[i]);
        put32(bytes, 104 + i * 12, counts[i]);
    }
    for (usize i = 0; i < 2; ++i) {
        const usize at = 8 + i * 24;
        bytes[at] = 1;
        bytes[at + 1] = 16;
        put16(bytes, at + 4, 0xffff);
        put16(bytes, at + 6, 4);
        bytes[at + 8] = 7;
        bytes[at + 9] = 0xff;
        put16(bytes, at + 10, 3);
        put16(bytes, at + 12, static_cast<u16>(2 + i * 7));
        put16(bytes, at + 14, static_cast<u16>(3 + i * 7));
        bytes[at + 16] = 0xff;
        bytes[at + 17] = 4;
        bytes[at + 18] = static_cast<u8>(i);
        bytes[at + 19] = 32;
        put32(bytes, 56 + i * 8, 0x212b);
        put32(bytes, 60 + i * 8, static_cast<u32>(at));
    }
    text(bytes, 72, "AUTO");
    put32(bytes, 88, 2);
    put32(bytes, 92, 56);
    const auto decoded = decodeLegendsN64Sequences(bytes, {});
    REQUIRE(decoded.warnings.empty());
    REQUIRE(decoded.animation.trees.size() == 2);
    CHECK(decoded.animation.trees[0].name == "AUTO:!+");
    CHECK(decoded.animation.trees[1].name == "AUTO:!+#1");
    REQUIRE(decoded.animation.textureAnimations.size() == 2);
    CHECK(decoded.animation.textureAnimations[0].texture == 2);
    CHECK(decoded.animation.textureAnimations[1].texture == 9);
    CHECK(decoded.animation.textureAnimations[1].start == 1);
    // Ordinary name-addressable sequences still cannot shadow each other.
    text(bytes, 56, "TEST");
    text(bytes, 64, "TEST");
    CHECK_THROWS_AS(decodeLegendsN64Sequences(bytes, {}), FormatError);
}

TEST_CASE("N64 node texture tracks bind segment13 model variants without changing native slots",
          "[legends][n64][scene]") {
    TransformSequence fixture(0);
    fixture.bytes[24] = 7;
    put16(fixture.bytes, 32, 0); // Not a global destination: segment13 belongs to this node.
    put16(fixture.bytes, 34, 2);
    fixture.bytes[36] = 0;
    fixture.bytes[37] = 3;
    fixture.bytes[38] = 0;
    fixture.bytes[39] = 16;
    put16(fixture.bytes, 42, 0);
    fixture.models[0].mesh.parts.push_back({LegendsN64Model::kRuntimeTexture, 0, {0, 1, 2}});
    fixture.models[0].mesh.vertices.resize(3);
    for (const u8 flags : std::array<u8, 3>{0, 1, 5}) {
        fixture.bytes[18] = flags;
        const auto decoded = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
        REQUIRE(decoded.warnings.empty());
        REQUIRE(decoded.modelVariants.size() == 1);
        REQUIRE(decoded.animation.textureAnimations.size() == 1);
        CHECK(fixture.models[0].mesh.parts[0].texture == LegendsN64Model::kRuntimeTexture);
        CHECK(decoded.modelVariants[0].mesh.parts[0].texture == 2);
        const auto& tree = decoded.animation.trees[0];
        CHECK(tree.nodes[0].object == decoded.modelVariants[0].name);
        CHECK(tree.nodes[0].textureAnimation == 0);
        const auto& animation = decoded.animation.textureAnimations[0];
        CHECK(animation.texture == 2);
        CHECK(animation.source == 2);
        CHECK(animation.frames == 3);
        CHECK(animation.rate == 1);
        CHECK(animation.flag == 0);
        const auto bound = AnimationSet::fromDecoded(decoded.animation);
        const auto& sequence = bound.tree(0).sequences[0];
        CHECK(sequence.effectFrame(0) == ((flags & 4) ? 2 : 0));
        CHECK(sequence.repeats == ((flags & 1) == 0));
    }
    fixture.bytes[39] = 15; // A clock the common keyed frame model cannot express exactly.
    const auto unsupported = decodeLegendsN64Sequences(fixture.bytes, fixture.models);
    CHECK(unsupported.modelVariants.empty());
    CHECK(unsupported.animation.trees[0].nodes[0].object.empty());
    CHECK_FALSE(unsupported.warnings.empty());
}

TEST_CASE("All extracted N64 maps bind native placement templates and scoped scene archives",
          "[legends][n64][scene][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root / "levels")) {
        SKIP("Native N64 extraction unavailable");
    }
    std::array<std::vector<u8>, 7> nativeTemplates;
    const std::array names{"ppow", "pdec", "pgen", "ptrap", "ptel", "pcon", "prnd"};
    for (usize i = 0; i < names.size(); ++i) {
        nativeTemplates[i] = readFile(root / "items/generic" / (std::string(names[i]) + ".rom"));
    }
    const LegendsN64Templates templates{nativeTemplates[0], nativeTemplates[1], nativeTemplates[2],
                                        nativeTemplates[3], nativeTemplates[4], nativeTemplates[5],
                                        nativeTemplates[6]};
    usize maps = 0;
    std::array<usize, 6> totals{};
    std::set<std::string> archives;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::exists(entry.path() / "worlds.wad")) {
            continue;
        }
        INFO(entry.path().filename().string());
        const auto fixtures =
            decodeLegendsN64Fixtures(readFile(entry.path() / "ipr.rom"), templates);
        for (const auto& item : fixtures) {
            ++totals[static_cast<usize>(item.kind)];
            if (item.kind == LegendsN64FixtureKind::Powerup && item.minimumPlayers <= 1) {
                CHECK((item.resolvedTemplateKey >> 8U) != 0x22U);
                CHECK(item.sequence != "POTR");
                CHECK(item.sequence != "FRTR");
            }
        }
        for (const auto& path :
             legendsN64FixtureArchives(entry.path().filename().string(), fixtures)) {
            archives.insert(path);
        }
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(totals == std::array<usize, 6>{1199, 0, 2816, 1425, 42, 1065});
    const auto program = readFile(root / "_program/game.bin");
    usize movingTracks = 0;
    usize runtimeVariants = 0;
    for (const auto& archive : archives) {
        INFO(archive);
        const auto models = decodeLegendsN64Models(readFile(root / archive / "objects.wad"));
        const auto sequences =
            decodeLegendsN64Sequences(readFile(root / archive / "seq.wad"), models, program);
        if (archive == "levels/effects") {
            CHECK(models.empty()); // Native effects bank supplies shared textures, not geometry.
        } else {
            REQUIRE_FALSE(models.empty());
        }
        REQUIRE_FALSE(sequences.animation.trees.empty());
        runtimeVariants += static_cast<usize>(std::count_if(
            sequences.modelVariants.begin(), sequences.modelVariants.end(),
            [](const auto& model) { return model.name.find(":MORPH") == std::string::npos; }));
        for (const auto& model : sequences.modelVariants) {
            for (const auto& part : model.mesh.parts) {
                CHECK(part.texture < 1024);
            }
        }
        if (archive == "itemanim/generic") {
            CHECK(sequences.animation.textureAnimations.size() == 29);
        } else if (archive == "itemanim/hires") {
            const auto glow =
                std::find_if(sequences.animation.textureAnimations.begin(),
                             sequences.animation.textureAnimations.end(),
                             [](const auto& animation) { return animation.flag == -1; });
            REQUIRE(glow != sequences.animation.textureAnimations.end());
            CHECK(glow->name == "!+");
            CHECK(glow->texture == 132); // Native YEL_HANDGLOW destination.
            CHECK(glow->source == 133);  // Native YEL_HANDGLOW00 first frame.
            CHECK(glow->frames == 5);
            CHECK(glow->rate == 2);
        } else if (archive == "levels/effects") {
            // e0026674 resolves the level's external FIRE/TORCH names to
            // these shared destinations; e0026924 changes every borrower.
            const std::array<s32, 4> destinations{31, 32, 33, 0};
            const std::array<s32, 4> sources{16, 16, 16, 1};
            const std::array<s32, 4> starts{0, 10, 20, 20};
            for (usize i = 0; i < destinations.size(); ++i) {
                const auto cycle = std::find_if(
                    sequences.animation.textureAnimations.begin(),
                    sequences.animation.textureAnimations.end(), [&](const auto& animation) {
                        return animation.flag == -1 && animation.texture == destinations[i];
                    });
                REQUIRE(cycle != sequences.animation.textureAnimations.end());
                CHECK(cycle->name == "!+");
                CHECK(cycle->source == sources[i]);
                CHECK(cycle->start == starts[i]);
                CHECK(cycle->frames == 15);
                CHECK(cycle->rate == 2);
            }
        }
        for (const auto& tree : sequences.animation.trees) {
            for (usize i = 0; i < tree.nodes.size(); ++i) {
                CHECK(tree.nodes[i].parent < static_cast<s32>(i));
            }
            for (const auto& sequence : tree.sequences) {
                for (const auto& track : sequence.tracks) {
                    CHECK(track.values.size() == track.frames.size() * track.channelCount());
                    CHECK(track.frames.back() < sequence.frameCount);
                    for (usize frame = 1; frame < track.frames.size(); ++frame) {
                        if (!std::equal(track.values.begin(),
                                        track.values.begin() + track.channelCount(),
                                        track.values.begin() + static_cast<std::ptrdiff_t>(
                                                                   frame * track.channelCount()))) {
                            ++movingTracks;
                            break;
                        }
                    }
                }
            }
        }
    }
    CHECK(movingTracks > 50);
    CHECK(runtimeVariants == 14); // Three entrance, three forcefield and eight steam sequences.
}

TEST_CASE("native Tower podium binds its direct vertex animation to contiguous mesh frames",
          "[legends][n64][scene][native-assets][morph]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native N64 extraction is not installed");
    }
    const auto models = decodeLegendsN64Models(readFile(root / "items/tower/objects.wad"));
    const auto decoded = decodeLegendsN64Sequences(readFile(root / "items/tower/seq.wad"), models,
                                                   readFile(root / "_program/game.bin"));
    for (const auto& warning : decoded.warnings) {
        INFO(warning);
        CHECK(decoded.warnings.empty());
    }
    for (const std::string_view sequence : {"FLIP", "REDY"}) {
        const auto name = "PODIUM:" + std::string(sequence);
        const auto tree =
            std::find_if(decoded.animation.trees.begin(), decoded.animation.trees.end(),
                         [&](const auto& value) { return value.name == name; });
        REQUIRE(tree != decoded.animation.trees.end());
        REQUIRE(tree->nodes.size() == 4);
        const auto& node = tree->nodes[3];
        REQUIRE(node.objectFrames.size() == 1);
        const auto& run = node.objectFrames.front();
        CHECK(run.frames == (sequence == "FLIP" ? 14 : 1));
        CHECK(run.start == 0);
        const auto first =
            std::find_if(decoded.modelVariants.begin(), decoded.modelVariants.end(),
                         [&](const auto& model) { return model.name == run.object; });
        REQUIRE(first != decoded.modelVariants.end());
        REQUIRE(decoded.modelVariants.end() - first >= run.frames);
        REQUIRE_FALSE(first->mesh.vertices.empty());
        const auto source = std::find(first->vertexSources.begin(), first->vertexSources.end(), 0);
        REQUIRE(source != first->vertexSources.end());
        const usize vertex = static_cast<usize>(source - first->vertexSources.begin());
        CHECK(first->mesh.vertices[vertex].position == Vec3{10, 241, 119} / 64.0f);
        if (sequence == "FLIP") {
            CHECK(first->mesh.vertices != (first + 6)->mesh.vertices);
        }
    }
}
