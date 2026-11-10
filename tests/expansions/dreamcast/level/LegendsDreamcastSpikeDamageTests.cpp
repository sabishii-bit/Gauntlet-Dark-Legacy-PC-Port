#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerHealth.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast native spikes deliver piercing damage to independent party seats",
          "[legends][dreamcast-traps][dreamcast-spike-damage][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levela1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto common = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    const auto bytes = readFile(directory / "worlds.rom");
    const auto native = formats::LegendsDreamcastScene::parse(bytes);
    const auto placed = std::ranges::find_if(native.itemInstances, [&](const auto& item) {
        if (item.info < 0 || item.minPlayers > 1) {
            return false;
        }
        const auto& info = native.itemInfos.at(static_cast<usize>(item.info));
        return info.type == 8 && info.subtype == 0;
    });
    REQUIRE(placed != native.itemInstances.end());
    const auto index = static_cast<s32>(placed - native.itemInstances.begin());
    const auto& info = native.itemInfos.at(static_cast<usize>(placed->info));
    REQUIRE(info.name == "SPIKES");
    REQUIRE(info.value == 20);
    REQUIRE(info.properties == 0x2000);
    CAPTURE(index, placed->position.x, placed->position.y, placed->position.z);

    test::FakeRenderDevice device;
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 4);
    REQUIRE(adapter);
    // Console identities index placed figures, not native item records: a
    // container/generator can contribute several figures before this trap.
    std::optional<ImportedWorldObject> object;
    for (usize figure = 0; figure < adapter->fixtureCount(); ++figure) {
        const auto candidate = adapter->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase +
                                                      static_cast<s32>(figure));
        if (candidate && candidate->name == info.name &&
            glm::length(Vec2{candidate->position.x - placed->position.x,
                             candidate->position.z - placed->position.z}) < .001f) {
            REQUIRE_FALSE(object);
            object = candidate;
        }
    }
    REQUIRE(object);
    REQUIRE(object->visible);
    // Use the runtime's floor-snapped fixture, not the raw authored item height.
    const Vec3 position = object->position;
    CAPTURE(object->id, object->name, position.x, position.y, position.z);
    LegendsLevel level{device, map, std::move(adapter), directory, common};

    std::array<PlayerRuntime, 4> players;
    for (usize seat = 0; seat < players.size(); ++seat) {
        players[seat].actor.spawn(static_cast<s32>(seat), {}, nullptr, position, 0);
        players[seat].actor.save().progress().health = 1000;
    }
    std::array<s32, 4> expectedHealth{980, 980, 980, 980};
    std::array<usize, 4> expectedContacts{1, 1, 1, 1};
    std::array<bool, 4> injured{true, true, true, true};
    SECTION("all four seats receive the native physical spike hit") {}
    SECTION("shared combat preserves levitation, invulnerability and elemental distinctions") {
        // ItemTouch 8c0af400 excludes Levitation before damage, sound and the
        // per-player cooldown. Other shields intentionally keep Dark Legacy
        // combat on imported maps rather than skipping native contact admission.
        players[0].actor.save().progress().inventory.addPowerup(powerup::kSpecial,
                                                                powerup::kLevitation, 0, -1);
        players[1].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                                armor::kInvulnerable, 0, -1);
        players[2].actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                                armor::kGoldInvulnerable, 0, -1);
        players[3].actor.save().progress().inventory.addPowerup(powerup::kArmor, armor::kImmuneFire,
                                                                0, -1);
        expectedHealth = {1000, 1000, 1002, 980};
        expectedContacts = {0, 1, 1, 1};
        injured = {false, false, false, true};
    }
    // Party-vector order is not the input seat used by trap hit admission.
    std::array<PlayerActor*, 4> party{&players[3].actor, &players[1].actor, &players[0].actor,
                                      &players[2].actor};
    std::array<usize, 4> contacts{};
    std::array<std::vector<std::string>, 4> cries;
    PlayerHealth health;
    const auto apply = [&] {
        for (const auto& hit : level.takeHazardHits()) {
            REQUIRE(hit.player >= 0);
            REQUIRE(hit.player < 4);
            const auto seat = static_cast<usize>(hit.player);
            REQUIRE(hit.pierces);
            CHECK_FALSE(hit.poison);
            CHECK_FALSE(hit.armorAbsorbed);
            CHECK(hit.damage == info.value);
            CHECK((hit.impact.flags & damage::kSpike) != 0);
            CHECK((hit.impact.flags & damage::kLow) != 0);
            CHECK((hit.impact.flags & damage::kElementMask) == 0);
            CHECK(hurtSourceOf(hit.impact.flags) == HurtSource::Undirected);
            const PlayerHealth::Events feedback{
                .block = [](f32, f32) {},
                .sound = [](std::string_view) {},
                .cry = [&](std::string_view cue) { cries[seat].emplace_back(cue); },
                .named = [](std::string_view, f32) {},
            };
            health.hurt(players[seat], hit.damage, HurtKind::Pierce, hurtSourceOf(hit.impact.flags),
                        {}, feedback, hit.impact);
            ++contacts[seat];
        }
    };
    bool reached = false;
    for (s32 tick = 0; tick < 1200; ++tick) {
        level.update(1.f / 60, false);
        level.updateParty(party, 1.f / 60, false);
        apply();
        if (std::ranges::any_of(contacts, [](usize count) { return count != 0; })) {
            reached = true;
            break;
        }
    }
    REQUIRE(reached);
    CHECK(contacts == expectedContacts);
    for (usize seat = 0; seat < players.size(); ++seat) {
        CAPTURE(seat);
        CHECK(players[seat].actor.save().health() == expectedHealth[seat]);
        CHECK(players[seat].reaction == (injured[seat] ? PlayerDeed::Spike : PlayerDeed::None));
        CHECK(cries[seat] ==
              (injured[seat] ? std::vector<std::string>{"DIE1"} : std::vector<std::string>{}));
    }

    // A second contact query in the same native frame must not spend health
    // again, nor use another seat's cooldown merely because the party is reordered.
    level.updateParty(party, 0, false);
    CHECK(level.takeHazardHits().empty());
    for (s32 tick = 0; tick < 120; ++tick) {
        level.update(1.f / 60, true);
        level.updateParty(party, 1.f / 60, false);
        CHECK(level.takeHazardHits().empty());
    }
    // Stop Time restores the resting action; its released cycle must remain
    // live rather than leaving a permanently consumed per-player hit mask.
    std::array<usize, 4> resumed{};
    for (s32 tick = 0; tick < 1200; ++tick) {
        level.update(1.f / 60, false);
        level.updateParty(party, 1.f / 60, false);
        for (const auto& hit : level.takeHazardHits()) {
            REQUIRE(hit.player >= 0);
            REQUIRE(hit.player < 4);
            ++resumed[static_cast<usize>(hit.player)];
        }
        if (std::ranges::any_of(resumed, [](usize count) { return count != 0; })) {
            break;
        }
    }
    CHECK(resumed == expectedContacts);
}
} // namespace
