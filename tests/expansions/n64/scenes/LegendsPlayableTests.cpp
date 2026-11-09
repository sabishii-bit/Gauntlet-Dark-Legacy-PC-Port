#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsPreview.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::LegendsMap playableMap(const std::filesystem::path& directory, LegendsPlatform platform) {
    const bool n64 = platform == LegendsPlatform::N64;
    const auto worlds = readFile(directory / (n64 ? "worlds.wad" : "worlds.rom"));
    const auto models = readFile(directory / (n64 ? "objects.wad" : "objects.rom"));
    auto map = n64 ? formats::LegendsMap::parseN64(worlds, models, readFile(directory / "ipr.rom"))
                   : formats::LegendsMap::parseDreamcast(worlds, models);
    if (n64) {
        const auto dynamic = directory / "texdyn.rom";
        const auto dynamicBytes =
            std::filesystem::is_regular_file(dynamic) ? readFile(dynamic) : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamicBytes)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
    } else {
        map.textures =
            formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    }
    return map;
}

void startNearGold(formats::LegendsMap& map, std::span<const LegendsPickup> pickups,
                   f32 playerReach) {
    // Only relocate the test entrance, never the item or native floor/wall data.
    // The real traversal opener still rejects obstructed candidates. Gold can sit
    // against a wall, so include nearby points inside the ordinary pickup reach.
    // Containers prebind their possible loot while inactive; those positions
    // are not collectible entrances until the opening animation has finished.
    map.starts.clear();
    for (const auto& pickup : pickups) {
        if (!pickup.active || pickup.collected ||
            pickup.offer.kind != static_cast<s32>(ItemKind::Gold)) {
            continue;
        }
        map.starts.push_back({.position = pickup.position});
        for (const f32 radius : std::array{playerReach * .5f, playerReach * .9f}) {
            for (s32 direction = 0; direction < 8; ++direction) {
                const f32 angle = static_cast<f32>(direction) * kPi / 4;
                map.starts.push_back(
                    {.position = pickup.position +
                                 Vec3{std::cos(angle) * radius, 0, std::sin(angle) * radius}});
            }
        }
    }
    REQUIRE_FALSE(map.starts.empty());
}

TEST_CASE("Playable pickup test entrances exclude contained and collected gold",
          "[legends][playable][items]") {
    formats::LegendsMap map;
    std::array<LegendsPickup, 4> pickups;
    for (usize index = 0; index < pickups.size(); ++index) {
        pickups[index].offer.kind = static_cast<s32>(ItemKind::Gold);
        pickups[index].position = {static_cast<f32>(index) * 10, 2, 0};
    }
    pickups[0].active = false; // Closed-chest outcome precedes the loose gold.
    pickups[1].collected = true;
    pickups[3].offer.kind = static_cast<s32>(ItemKind::Food);
    startNearGold(map, pickups, 1);
    REQUIRE(map.starts.size() == 17);
    CHECK(map.starts.front().position == pickups[2].position);
    for (const auto& marker : map.starts)
        CHECK(glm::distance(marker.position, pickups[2].position) <= 1);
}

TEST_CASE("Native Legends playable visits collect on fixed ticks and return durable isolated saves",
          "[legends][playable][assets][items]") {
    const auto characterRoot = test::assetOrSkip("PLAYERS").parent_path();
    const auto assets = test::dataDirectory().parent_path() / "assets";
    LegendsPlatform platform = LegendsPlatform::N64;
    std::filesystem::path directory;
    SECTION("Nintendo64 mount4") {
        // mount1's three loose gold placements require two, three or four
        // players. mount4 has two initial single-player gold placements.
        directory = assets / "n64/extracted/levels/mount4";
    }
    SECTION("Dreamcast levela1") {
        platform = LegendsPlatform::Dreamcast;
        directory = assets / "dreamcast/extracted/native/levels/levela1";
    }
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Native Legends extraction is not installed");
    }

    auto map = playableMap(directory, platform);
    const usize nativeFaces = map.collision.size();
    REQUIRE(nativeFaces > 100);
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, platform, directory, map);
    auto* scene = adapter.get(); // Borrowed only until preview.close().
    REQUIRE(scene != nullptr);
    INFO(directory.string());
    INFO("Native pickup count: " << scene->pickups().size());
    for (const auto& pickup : scene->pickups()) {
        UNSCOPED_INFO(pickup.name << " kind " << pickup.offer.kind << " amount "
                                  << pickup.offer.amount);
    }
    for (const auto& warning : scene->warnings()) {
        UNSCOPED_INFO(warning);
    }
    REQUIRE_FALSE(scene->pickups().empty());
    CHECK(std::ranges::none_of(scene->pickups(), &LegendsPickup::collected));

    PartyMember member;
    member.save.name = "TEST";
    member.save.gold = 10;
    member.save.progress().health = 500;
    member.save.progress().relics.addRune(1);
    member.save.progress().legendsProgress(LegendsPlatform::N64).add(LegendsRelic::Runestone, 2);
    member.save.progress().legendsProgress(LegendsPlatform::Dreamcast).add(LegendsRelic::Shard, 1);
    const CharacterSave original = member.save;
    ClassDataSet classes;
    classes.load(characterRoot / "pdata");
    PlayerActor dimensions;
    dimensions.spawn(member.player, member.save, classes.stats(member.save.character), Vec3{0}, 0);
    startNearGold(map, scene->pickups(), dimensions.reach());
    CHECK(map.collision.size() == nativeFaces);

    LegendsPreview preview;
    preview.openPlayable(device, map, characterRoot, member, std::move(adapter));
    REQUIRE(preview.playable());
    REQUIRE(preview.traversal());
    REQUIRE(preview.traversal()->authoredStart());
    const auto& actor = preview.traversal()->actor();
    const auto pickups = scene->pickups();
    const auto selected = std::ranges::find_if(pickups, [&](const auto& pickup) {
        return pickup.offer.kind == static_cast<s32>(ItemKind::Gold) &&
               touchesLegendsPickup(pickup, actor.followPoint(), actor.reach(),
                                    actor.height() * .5f);
    });
    INFO("Actor feet " << actor.position().x << ',' << actor.position().y << ','
                       << actor.position().z << " centre " << actor.followPoint().x << ','
                       << actor.followPoint().y << ',' << actor.followPoint().z);
    for (const auto& pickup : pickups) {
        if (pickup.offer.kind != static_cast<s32>(ItemKind::Gold))
            continue;
        const Vec3 difference = pickup.position - actor.position();
        if (glm::length(Vec2{difference.x, difference.z}) < actor.reach() + pickup.radius + 1)
            UNSCOPED_INFO("Nearby gold "
                          << pickup.nativeIndex << ' ' << pickup.name << " active=" << pickup.active
                          << " collected=" << pickup.collected << " at " << pickup.position.x << ','
                          << pickup.position.y << ',' << pickup.position.z);
    }
    REQUIRE(selected != pickups.end());
    const usize selectedIndex = static_cast<usize>(selected - pickups.begin());
    const s32 selectedGold = selected->offer.amount;
    INFO("Selected gold " << selected->nativeIndex << ' ' << selected->name
                          << " active=" << selected->active << " at " << selected->position.x << ','
                          << selected->position.y << ',' << selected->position.z);
    REQUIRE(selectedGold > 0);
    REQUIRE(selected->active);
    REQUIRE_FALSE(selected->collected);
    const auto parkedPosition = actor.position();

    Input input;
    input.beginPoll();
    GameConfig config;
    config.combat.autoActivateItems = false;
    preview.updatePlayable(input, .25f, true, config);
    CHECK(actor.position() == parkedPosition);
    CHECK(actor.save() == original);
    CHECK_FALSE(scene->pickups()[selectedIndex].collected);
    CHECK(member.save == original);

    // One half-frame cannot collect; completing a 60Hz tick must use the actual
    // Preview -> traversal -> adapter -> takeLegendsPickup path, not a direct grant.
    preview.updatePlayable(input, 1.0f / 120.0f, false, config);
    CHECK_FALSE(scene->pickups()[selectedIndex].collected);
    CHECK(actor.save() == original);
    preview.updatePlayable(input, 1.0f / 120.0f, false, config);
    REQUIRE(scene->pickups()[selectedIndex].collected);
    CHECK(actor.save().gold >= original.gold + selectedGold);
    CHECK(actor.save().progress().relics == original.progress().relics);
    CHECK(member.save == original);
    const auto collectedGold = actor.save().gold;
    for (s32 tick = 0; tick < 5; ++tick) {
        preview.updatePlayable(input, 1.0f / 60.0f, false, config);
    }
    CHECK(actor.save().gold == collectedGold);
    preview.render(device, Mat4{1}, 640, 448, kPi / 2);
    CHECK_FALSE(device.draws.empty());

    // A concurrent change must win over the visit; refusal must not consume the
    // pending reward or prevent a later transfer to the unchanged parked state.
    CharacterSave concurrent = original;
    ++concurrent.gold;
    const auto newer = concurrent;
    CHECK_FALSE(preview.transferProgress(concurrent));
    CHECK(concurrent == newer);
    CHECK(member.save == original);
    REQUIRE(preview.transferProgress(member.save));
    CHECK(member.save.gold == collectedGold);
    CHECK(member.save == actor.save());
    REQUIRE(preview.transferProgress(member.save));
    CHECK(member.save.gold == collectedGold);

    const auto reloaded = CharacterSave::fromJson(member.save.toJson());
    CHECK(reloaded.name == original.name);
    CHECK(reloaded.character == original.character);
    CHECK(reloaded.gold == collectedGold);
    CHECK(reloaded.progress().inventory == member.save.progress().inventory);
    CHECK(reloaded.progress().relics == original.progress().relics);
    CHECK(reloaded.progress().legends == member.save.progress().legends);
    preview.close();
    CHECK_FALSE(preview.isOpen());
    CHECK_FALSE(preview.playable());
    CHECK_FALSE(preview.transferProgress(member.save));
    CHECK(member.save.gold == collectedGold);
}

} // namespace
