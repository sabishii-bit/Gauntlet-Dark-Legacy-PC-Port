#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Critter.h"
#include "expansions/n64/formats/LegendsN64SequenceGroups.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/critters/CritterData.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "dragon/moves.wad")) {
        SKIP("N64 boss data not installed");
    }
    return root;
}

void put32(std::vector<u8>& b, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        b.at(at + i) = static_cast<u8>(value >> (24U - static_cast<u32>(i) * 8U));
    }
}

TEST_CASE("Native boss record decoder rejects incomplete and nonfinite data",
          "[legends][n64-critter]") {
    CHECK_THROWS_AS(decodeLegendsN64Critter({}), FormatError);
    const std::array<u8, 8> empty{};
    CHECK_THROWS_AS(decodeLegendsN64Critter(empty), FormatError);
}

TEST_CASE("Native boss bone lookup uses BNAM indices and fixed-width authored order",
          "[legends][n64-critter][native-boss-melee]") {
    const std::array names{
        LegendsN64BoneName{"RESETBODY1_L_WRIST", 12}, LegendsN64BoneName{"RESETBODY1_R_WRIST", 26},
        LegendsN64BoneName{"RESETBODY1_HEAD01", 6}, LegendsN64BoneName{"RESETBODY1_HEAD02", 7}};
    CHECK(findLegendsN64Bone(names, "RESETBODY1_L_WR") == 12);
    CHECK(findLegendsN64Bone(names, "resetbody1_r_wr") == 26);
    CHECK(findLegendsN64Bone(names, "RESETBODY1_HEAD") == 6);
    CHECK_FALSE(findLegendsN64Bone(names, "RESETBODY1_L_W")); // NUL before character15 differs.
    CHECK_FALSE(findLegendsN64Bone(names, "BODY1_L_WRIST"));  // No guessed prefix stripping.
    CHECK_FALSE(findLegendsN64Bone(names, ""));

    std::vector<u8> bytes(72);
    put32(bytes, 0, 48);
    put32(bytes, 4, 2);
    const std::string bone = "RESETBODY1_L_WRIST";
    std::copy(bone.begin(), bone.end(), bytes.begin() + 8);
    put32(bytes, 32, 8);
    put32(bytes, 36, 12);
    put32(bytes, 48, 0x42444154); // BDAT: byte extent includes the name terminator.
    put32(bytes, 52, 8);
    put32(bytes, 56, 24);
    put32(bytes, 60, 0x424e414d); // BNAM: one pair of native name offset / node index.
    put32(bytes, 64, 32);
    put32(bytes, 68, 1);
    const auto decoded = decodeLegendsN64BoneNames(bytes);
    REQUIRE(decoded.size() == 1);
    CHECK(decoded.front().name == bone);
    CHECK(decoded.front().node == 12);
    CHECK(findLegendsN64Bone(decoded, "RESETBODY1_L_WR") == 12);

    auto malformed = bytes;
    put32(malformed, 32, 32); // A name cannot point into BNAM itself.
    CHECK_THROWS_AS(decodeLegendsN64BoneNames(malformed), FormatError);
    malformed = bytes;
    put32(malformed, 36, 4096);
    CHECK_THROWS_AS(decodeLegendsN64BoneNames(malformed), FormatError);
    malformed = bytes;
    std::fill(malformed.begin() + 8, malformed.begin() + 32, 'X');
    CHECK_THROWS_AS(decodeLegendsN64BoneNames(malformed), FormatError);
    CHECK_THROWS_AS(decodeLegendsN64BoneNames(std::span{bytes}.first(68)), FormatError);
}

TEST_CASE("Native Skorne combat references select the authored BNAM wrist and head nodes",
          "[legends][n64-critter][native-boss-melee][assets]") {
    const auto root = nativeRoot();
    const auto names = decodeLegendsN64BoneNames(readFile(root / "boss2/seq.wad"));
    REQUIRE(names.size() == 48);
    const auto data = decodeLegendsN64Critter(readFile(root / "sko2/moves.wad"));
    CHECK(findLegendsN64Bone(names, data.moves[9].colnode) == 12);
    CHECK(findLegendsN64Bone(names, data.moves[10].colnode) == 26);
    CHECK(findLegendsN64Bone(names, data.moves[13].colnode) == 6);
    CHECK(findLegendsN64Bone(names, data.nodes[0].nodeName) == 4);
    CHECK(findLegendsN64Bone(names, data.nodes[1].nodeName) == 6);
}

TEST_CASE("All six native boss move tables preserve their own body and combat data",
          "[legends][n64-critter][assets]") {
    const auto root = nativeRoot();
    struct Expected {
        std::string_view bank;
        std::string_view tree;
        usize moves;
        usize types;
        f32 health;
        f32 pressure;
    };
    const std::array rows{Expected{"dragon", "DRAGON", 26, 1, 2500, 1000},
                          Expected{"yeti", "YETI", 25, 1, 6000, 2500},
                          Expected{"chi", "CHIM", 50, 4, 3750, 2000},
                          Expected{"slime", "SLIME", 24, 1, 5000, 1500},
                          Expected{"sko1", "BOSS1", 21, 1, 6000, 3000},
                          Expected{"sko2", "BOSS2", 17, 1, 10000, 5000}};
    for (const auto& row : rows) {
        CAPTURE(row.bank);
        const auto bytes = readFile(root / row.bank / "moves.wad");
        const auto source = decodeLegendsN64Critter(bytes);
        CHECK(source.moves.size() == row.moves);
        CHECK(source.types.size() == row.types);
        CHECK(source.types.front().maxHealth == row.health);
        CHECK(source.types.front().pressureReferenceDamage == row.pressure);
        for (usize i = 0; i < source.types.size(); ++i) {
            // TYPE+b8 is the pressure denominator, not max HP or awarded XP.
            // Chimera's independent heads each retain their own 1000-unit budget.
            CHECK(source.types[i].pressureReferenceDamage == (i == 0 ? row.pressure : 1000));
            const auto data = game::CritterData::fromDecoded(row.bank, source, i);
            CHECK(data.kind() == game::CombatantKind::Boss);
            if (i == 0) {
                CHECK(data.tree() == row.tree);
            }
            for (const auto& move : data.moves()) {
                CHECK_FALSE(move.anim.empty());
                CHECK(move.hold == (move.type == 17 ? 2.0f : move.type == 0x86 ? 1.5f : 0.0f));
            }
        }
        if (row.bank == "sko1") {
            const auto& attack = source.moves[10];
            CHECK(attack.name == "CLAWL");
            CHECK(attack.anim == "AT1");
            CHECK(attack.colnode == "BODY1_L_WRIST");
            CHECK(attack.frameStart == 6);
            CHECK(attack.frameEnd == 9);
            CHECK(source.damages[static_cast<usize>(attack.damage0)].damage == 100);
            CHECK(source.sounds[3].flags == 0);     // Native 0x20 is not GDL's arena callback.
            CHECK(source.moves[2].sfx2Frame == 76); // Authored but beyond the 51-frame entrance.
            CHECK(source.moves[17].name == "POUNDL");
            CHECK(source.moves[18].name == "POUNDR");
            CHECK(source.moves[17].frameStart == 4); // Raw reader does not apply port repairs.
            CHECK(source.moves[18].frameStart == 4);
        }
        if (row.bank == "slime") {
            const auto data = game::CritterData::fromDecoded(row.bank, source);
            REQUIRE(data.attachments().size() == 1);
            CHECK(data.attachments().front().tree == "POOL");
            CHECK_FALSE(data.attachments().front().follows);
            CHECK(data.attachments().front().offset == Vec3{0, -0.5f, 0});
        }
        auto malformed = bytes;
        const usize directory = readU32BE(bytes, 0);
        for (usize i = 0; i < readU32BE(bytes, 4); ++i) {
            const usize at = directory + i * 12;
            constexpr std::string_view kType = "TYPE";
            if (std::equal(kType.begin(), kType.end(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(at))) {
                put32(malformed, readU32BE(bytes, at + 4) + 0xac,
                      std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
            }
        }
        CHECK_THROWS_AS(decodeLegendsN64Critter(malformed), FormatError);
        malformed = bytes;
        for (usize i = 0; i < readU32BE(bytes, 4); ++i) {
            const usize at = directory + i * 12;
            constexpr std::string_view kType = "TYPE";
            if (std::equal(kType.begin(), kType.end(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(at))) {
                put32(malformed, readU32BE(bytes, at + 4) + 0xb8,
                      std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()));
            }
        }
        CHECK_THROWS_AS(decodeLegendsN64Critter(malformed), FormatError);
        malformed = bytes;
        put32(malformed, directory + 8, 4097);
        CHECK_THROWS_AS(decodeLegendsN64Critter(malformed), FormatError);
    }
}

TEST_CASE("Native sequence grouping preserves independent tracks and mesh runs",
          "[legends][n64-critter]") {
    AnimationFile file;
    for (const auto* name : {"BODY:INIT", "BODY:ATK1"}) {
        TreeDefinition tree;
        tree.name = name;
        TreeNode node;
        node.name = "ROOT";
        node.object = "MODEL";
        tree.nodes.push_back(node);
        TreeSequence sequence;
        sequence.name = std::string(name).substr(5);
        sequence.frameCount = 20;
        tree.sequences.push_back(sequence);
        file.trees.push_back(tree);
    }
    auto invalid = file;
    invalid.trees[1].nodes[0].position.x = 1;
    CHECK_THROWS_AS(combineLegendsN64SequenceGroups(invalid), FormatError);
    combineLegendsN64SequenceGroups(file);
    REQUIRE(file.trees.size() == 1);
    CHECK(file.trees.front().name == "BODY");
    CHECK(file.trees.front().sequences.size() == 2);
    REQUIRE(file.trees.front().nodes.front().objectFrames.size() == 2);
    CHECK(file.trees.front().nodes.front().objectFrames.back().object == "MODEL");
}

TEST_CASE("Native boss authored damage retains GDL melee and elemental defense semantics",
          "[legends][n64-critter][assets]") {
    const auto root = nativeRoot();
    const auto dragon = decodeLegendsN64Critter(readFile(root / "dragon/moves.wad"));
    const auto slime = decodeLegendsN64Critter(readFile(root / "slime/moves.wad"));
    const auto skorne = decodeLegendsN64Critter(readFile(root / "sko1/moves.wad"));
    const auto& claw = skorne.damages[static_cast<usize>(skorne.moves[10].damage0)];
    CHECK(claw.flags == game::damage::kKnockDown);
    const auto protectedClaw =
        game::Damage::modify(claw.damage, claw.flags, game::armor::kNoKnockback, 0, true);
    CHECK(protectedClaw.amount == claw.damage);
    CHECK((protectedClaw.flags & game::damage::kKnocks) == 0);
    const auto& fire = dragon.damages[2];
    CHECK(game::damage::element(fire.flags) == game::damage::kFire);
    CHECK(game::Damage::modify(fire.damage, fire.flags, game::armor::immune(game::damage::kFire), 0,
                               true)
              .amount == 0);
    const auto& acid = slime.damages[0];
    CHECK(game::damage::element(acid.flags) == game::damage::kAcid);
    CHECK(game::Damage::modify(acid.damage, acid.flags, game::armor::immune(game::damage::kAcid), 0,
                               true)
              .amount == 0);
    // N64's direct hurt consumer ignores this bit; the imported game deliberately
    // keeps GDL's gas-immunity powerup useful rather than reproducing that omission.
    const auto& gas = slime.damages[9];
    CHECK(gas.flags == game::damage::kGas);
    CHECK(game::Damage::modify(gas.damage, gas.flags, game::armor::kImmuneGas, 0, true).amount ==
          0);
}
} // namespace
