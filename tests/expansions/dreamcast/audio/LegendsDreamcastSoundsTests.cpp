#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("Dreamcast native bank routing plays transporter and realm gate effects",
          "[dreamcast-sounds][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
        SKIP("Dreamcast extraction is not installed");
    }
    game::LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    CHECK(sounds.namedToken("S_EXITFLAME") == 10);
    CHECK(sounds.namedToken("S_GATEB1") == game::dreamcastGateSound(2, 0));
    CHECK(sounds.namedToken("S_TROLL2HIT1CLOSE") == 0x21000f);
    CHECK(sounds.namedToken("S_TROLL2HIT2CLOSE") == 0x210010);
    CHECK(sounds.namedToken("S_TROLL2DIECLOSE") == 0x210011);
    const std::array<u32, 2> stone{0x210033, 0x210034};
    for (s32 slot = 0; slot < 5; ++slot) {
        CHECK(sounds.motionTokens('B', slot) == stone);
    }
    CHECK(sounds.motionTokens('B', 5) == std::array<u32, 2>{0x210002, 0x210003});
    CHECK(sounds.motionTokens('G', 0) == std::array<u32, 2>{0x22001f, 0x220020});
    CHECK(sounds.motionTokens('G', 1) == std::array<u32, 2>{0x220021, 0x220022});
    CHECK(sounds.motionTokens('G', 2) == std::array<u32, 2>{0x220025, 0x220026});
    CHECK(sounds.motionTokens('G', 3) == std::array<u32, 2>{0x220027, 0x220028});
    CHECK(sounds.motionTokens('G', 4) == std::array<u32, 2>{0x220023, 0x220024});
    CHECK_FALSE(sounds.motionTokens('B', -1));
    CHECK_FALSE(sounds.motionTokens('B', 6));
    CHECK_FALSE(sounds.motionTokens('\0', 0));
    CHECK(game::dreamcastGateSound(-1, 0) == 0);
    CHECK(game::dreamcastGateSound(2, 4) == 0);
    AudioMixer mixer{48000};
    SoundPlayer player{mixer};
    const auto flame = sounds.play(player, 10);
    REQUIRE(flame != kNoSound);
    CHECK(player.isPlaying(flame));
    const auto gate = sounds.play(player, game::dreamcastGateSound(2, 0));
    CHECK(gate != kNoSound);
    CHECK_FALSE(sounds.play(player, 0xffffffff));
    player.stop(flame);
    CHECK_FALSE(player.isPlaying(flame));
    player.stopAll();
}

TEST_CASE("Dreamcast generator cues follow the native realm table", "[dreamcast-sounds]") {
    constexpr std::array<std::array<u32, 2>, 8> kExpected{{
        {0x230031, 0x230032},
        {0x21002f, 0x210030},
        {0x24003d, 0x24003e},
        {0x250037, 0x250038},
        {0x2b0018, 0x2b0019},
        {0x2d001b, 0x2d001c},
        {0x220016, 0x220017},
        {0x260016, 0x260017},
    }};
    for (usize realm = 0; realm < kExpected.size(); ++realm) {
        CAPTURE(realm);
        CHECK(game::dreamcastGeneratorSound(static_cast<s32>(realm + 1), false) ==
              kExpected[realm][0]);
        CHECK(game::dreamcastGeneratorSound(static_cast<s32>(realm + 1), true) ==
              kExpected[realm][1]);
    }
    for (const s32 realm : {-1, 0, 9, 100}) {
        CHECK_FALSE(game::dreamcastGeneratorSound(realm, false));
    }
}

TEST_CASE("Missing Dreamcast audio leaves optional playback unavailable", "[dreamcast-sounds]") {
    game::LegendsDreamcastSounds sounds;
    CHECK_FALSE(sounds.open(test::scratchDirectory("no-dreamcast-audio") / "native"));
    CHECK_FALSE(sounds.namedToken("S_EXITFLAME"));
    AudioMixer mixer{48000};
    SoundPlayer player{mixer};
    CHECK(sounds.play(player, 10) == kNoSound);
}

TEST_CASE("Dreamcast collapsing scenery retains its native realm sound table",
          "[dreamcast-ambient][dreamcast-collapse]") {
    constexpr std::array<u32, 9> kExpected{0,        0x23003b, 0x210001, 0x240029, 0x250001,
                                           0x2c0000, 0x2d001e, 0x22002a, 0};
    for (usize realm = 0; realm < kExpected.size(); ++realm) {
        CHECK(dreamcastCollapseSound(static_cast<s32>(realm)) == kExpected[realm]);
        CHECK(dreamcastCollapseSound(static_cast<s32>(realm), true) ==
              (realm == 6 ? 0x2e0000 : kExpected[realm]));
    }
    CHECK(dreamcastCollapseSound(-1) == 0);
    CHECK(dreamcastCollapseSound(9) == 0);
}

TEST_CASE("Dreamcast barrel, chest and gate cues follow their native realm tables",
          "[dreamcast-sounds]") {
    // AudioBarrelWood, AudioBarrelExplosion and AudioBarrelGas by realm, for
    // the plain, exploding and poison barrels (subtypes 43, 44 and 45).
    CHECK(dreamcastContainerSound(1, 43) == 0x230007);
    CHECK(dreamcastContainerSound(1, 44) == 0x230009);
    CHECK(dreamcastContainerSound(1, 45) == 0x230008);
    CHECK(dreamcastContainerSound(2, 43) == 0x210024);
    CHECK(dreamcastContainerSound(8, 45) == 0x26001a);
    // AudioChest 8c0e0c80 plays S_CHEST for every chest, whatever the realm.
    for (const s32 realm : {-1, 0, 1, 5, 8, 9}) {
        for (const s32 subtype : {46, 47, 48}) {
            CAPTURE(realm, subtype);
            CHECK(dreamcastContainerSound(realm, subtype) == 38);
        }
    }
    // The realms without barrel cues, and the other subtypes, are silent.
    for (const s32 realm : {-1, 0, 5, 6, 9}) {
        CAPTURE(realm);
        CHECK(dreamcastContainerSound(realm, 43) == 0);
        CHECK(dreamcastGateSound(realm, 0) == 0);
    }
    CHECK(dreamcastContainerSound(1, 42) == 0);
    CHECK(dreamcastContainerSound(1, 49) == 0);
    // AudioGate: a row of four subtypes a realm.
    constexpr std::array<u32, 4> kCastle{0x230006, 0x230004, 0x230005, 0x230003};
    for (s32 subtype = 0; subtype < 4; ++subtype) {
        CHECK(dreamcastGateSound(1, subtype) == kCastle[static_cast<usize>(subtype)]);
    }
    CHECK(dreamcastGateSound(7, 3) == 0x220031);
    CHECK(dreamcastGateSound(1, -1) == 0);
    CHECK(dreamcastGateSound(1, 4) == 0);
}

TEST_CASE("Dreamcast barrel, gate, generator and collapse cues match the native executable",
          "[dreamcast-sounds][assets]") {
    const auto path =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/1ST_READ.BIN";
    if (!std::filesystem::exists(path)) {
        SKIP("optional Dreamcast executable is not installed");
    }
    const auto image = readFile(path);
    constexpr u32 kBase = 0x8c010000;
    constexpr u32 kNone = 0xffffffffU;
    // A table word is a bank/sample token, -1 where the realm has no cue.
    const auto cue = [&](u32 address) {
        const usize offset = address - kBase;
        REQUIRE(offset + 4 <= image.size());
        const u32 token = readU32LE(image, offset);
        return token == kNone ? 0U : token;
    };
    for (s32 realm = 0; realm < 9; ++realm) {
        CAPTURE(realm);
        const auto slot = static_cast<u32>(realm) * 4;
        CHECK(dreamcastContainerSound(realm, 43) == cue(0x8c12793c + slot));
        CHECK(dreamcastContainerSound(realm, 44) == cue(0x8c1279ac + slot));
        CHECK(dreamcastContainerSound(realm, 45) == cue(0x8c127974 + slot));
        for (s32 subtype = 0; subtype < 4; ++subtype) {
            CAPTURE(subtype);
            CHECK(dreamcastGateSound(realm, subtype) ==
                  cue(0x8c127bcc + slot * 4 + static_cast<u32>(subtype) * 4));
        }
        // The final boss's realm-six collapse is AudioCollapse's own branch.
        CHECK(dreamcastCollapseSound(realm) == cue(0x8c1279e4 + slot));
        CHECK(dreamcastGeneratorSound(realm, false).value_or(0) == cue(0x8c127da8 + slot));
        CHECK(dreamcastGeneratorSound(realm, true).value_or(0) == cue(0x8c127d84 + slot));
    }
}
