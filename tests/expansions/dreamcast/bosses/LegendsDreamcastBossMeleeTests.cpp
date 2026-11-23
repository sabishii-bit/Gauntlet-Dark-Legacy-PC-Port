#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "game/critters/BlowPushTestSupport.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/BossKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f32 kTolerance = 1e-3f;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
}

/** Lands every blow of `part` (0 for the body, else a head's id) of a Dreamcast boss standing at
 * its home, and checks that each pushes as 1ST_READ.BIN's CritterNodePlayerCollide (8c08d4c0)
 * builds the push: along the node's travel since MoveSetup (8c08a2c0) last noted it and away
 * from the body's origin (c+0x4C, ProcessCritter 8c08b460), each scaled by NormalVector
 * (8c0ec8e0). Returns how many it landed. */
usize checkSwingPushes(CombatantAssets& stock, s32 part) {
    Combatant actor;
    test::spawnAtHome(actor, stock, 'A');
    const Combatant* fighter = part == 0 ? &actor : actor.child(part);
    REQUIRE(fighter);
    const CritterData& data = *fighter->data();
    usize tested = 0;
    for (usize index = 0; index < data.moves().size(); ++index) {
        const auto& move = data.moves()[index];
        if (test::blowOf(data, move) == nullptr) {
            continue;
        }
        ++tested;
        CAPTURE(move.name, move.colnode, move.frameStart, move.frameEnd);
        const test::LandedBlow landed = test::landBlow(actor, part, index);
        const Vec3 expected = test::swingPush(landed.before, landed.node, landed.body, landed.feet);
        const Vec3& direction = landed.blows[0].direction;
        CAPTURE(direction.x, direction.y, direction.z, expected.x, expected.y, expected.z);
        CHECK(glm::distance(direction, expected) < kTolerance);
    }
    return tested;
}

TEST_CASE("a Dreamcast boss's blow pushes along its striking part's swing and away from its body",
          "[legends][bosses][native-boss-melee][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "critters/dragon.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    struct Bank {
        s32 kind;
        usize blows;
    };
    // The bodies' blows: the dragon's two claws, the djinn's seven, the drider's two charges and
    // each Skorne's left claw. The chimera's are its heads', below.
    constexpr std::array kBanks{Bank{boss_type::kDragon, 2}, Bank{boss_type::kDjinn, 7},
                                Bank{boss_type::kDrider, 2}, Bank{boss_type::kSkorne1, 1},
                                Bank{boss_type::kSkorne2, 1}};
    for (const auto& bank : kBanks) {
        DYNAMIC_SECTION("Dreamcast boss " << bank.kind) {
            test::FakeRenderDevice device;
            std::vector<std::string> warnings;
            auto stock = loadLegendsDreamcastBossAssets(device, root, {}, bank.kind, {}, warnings);
            REQUIRE(stock);
            CHECK(checkSwingPushes(*stock, 0) == bank.blows);
        }
    }
}

TEST_CASE("a Dreamcast Chimera head's bite pushes along its head's swing and away from the body",
          "[legends][bosses][native-boss-melee][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "critters/chimera.wad")) {
        SKIP("Dreamcast boss data not installed");
    }
    test::FakeRenderDevice device;
    std::vector<std::string> warnings;
    auto stock =
        loadLegendsDreamcastBossAssets(device, root, {}, boss_type::kChimera, {}, warnings);
    REQUIRE(stock);
    REQUIRE(stock->children.size() == 3);
    // The eagle's bite, the lion's two and the snake's.
    constexpr std::array<usize, 3> kBites{1, 2, 1};
    for (s32 id = 1; id <= static_cast<s32>(kBites.size()); ++id) {
        DYNAMIC_SECTION("head " << id) {
            CHECK(checkSwingPushes(*stock, id) == kBites[static_cast<usize>(id - 1)]);
        }
    }
}
} // namespace
