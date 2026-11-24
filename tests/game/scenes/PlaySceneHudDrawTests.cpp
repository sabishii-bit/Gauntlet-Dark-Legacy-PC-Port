#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "DrawStreamDigest.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudView.h"
#include "game/hud/PartyHud.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"
#include "game/scenes/PlayScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

bool awaitingEntrance(const PlayScene& scene) {
    if (scene.spawning()) {
        return true;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (scene.animator(player) != nullptr && scene.animator(player)->entering()) {
            return true;
        }
    }
    return false;
}

TEST_CASE("a G1 scene's HUD overlay stays byte for byte what it was",
          "[play-scene][hud-draw-pin][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    GameConfig config;
    // Bloom runs after the world and before the HUD, so where it ran the overlay begins.
    config.display.bloom = true;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &levels;
    context.unpackedRoot = root;

    // Four players: seat 0 holds the temple's shard, which shows the runestone finder, and
    // seat 1 carries a powerup it can name.
    constexpr std::array<std::string_view, 4> kNames{"AB", "CD", "EF", "GH"};
    std::vector<PartyMember> party;
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        CharacterSave save;
        save.name = std::string(kNames[static_cast<usize>(seat)]);
        save.character = seat + 2;
        save.color = (seat + 1) % PlayScene::kPlayerCount;
        save.progress().health = 4000;
        party.push_back({seat, save});
    }
    REQUIRE(party[0].save.progress().relics.addShard(9));
    party[1].save.progress().inventory.powerups[0] = PowerupSlot{-1, powerup::kSpeed, 5, 0, false};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    const auto play = [&](s32 ticks, const PlayScene::Inputs& inputs) {
        for (s32 tick = 0; tick < ticks; ++tick) {
            scene.update(1.0 / 60.0, inputs);
        }
    };
    for (s32 tick = 0; tick < 600 && awaitingEntrance(scene); ++tick) {
        play(1, still);
    }
    REQUIRE_FALSE(awaitingEntrance(scene));

    // Seat 3 falls and is asked to wait in the tower or quit.
    REQUIRE(scene.kill(3));
    for (s32 tick = 0; tick < 600 && !scene.runtime(3)->towerPrompt; ++tick) {
        play(1, still);
    }
    REQUIRE(scene.runtime(3)->towerPrompt);
    // Seat 1 names its powerup over its box.
    PlayScene::Inputs up{};
    up[1].selector.up = true;
    play(1, up);
    play(40, still);
    REQUIRE(scene.hud().selector(1).showing());
    // A gargoyle's piece underfoot: a card over its taker's box, and each standing player's
    // count of the eagle's pieces.
    REQUIRE(world.placeItem(device, "GARGEAGL", scene.actor(0)->position()));
    for (s32 tick = 0; tick < 120 && scene.pickups().cards().empty(); ++tick) {
        play(1, still);
    }
    REQUIRE(scene.pickups().cards().size() == 1);
    play(20, still);
    // Seat 2 reaches for a potion it has not got, and is told so over its head.
    PlayScene::Inputs reach{};
    reach[2].usePotion = true;
    play(1, reach);
    play(5, still);
    REQUIRE(scene.help().showing());
    REQUIRE(scene.help().player() == 2);
    REQUIRE(scene.pickups().cards().size() == 1);
    REQUIRE(scene.pickups().counts(0)[0].showing());
    REQUIRE(scene.runeMeter().visible());

    device.draws.clear();
    device.bloomDrawOffsets.clear();
    scene.render(device, makeScreenProjection(640, 448), 640, 448);
    REQUIRE(device.bloomDrawOffsets.size() == 1);
    const auto overlay = std::span{device.draws}.subspan(device.bloomDrawOffsets.front());
    REQUIRE_FALSE(overlay.empty());
    // SHA-256 of the overlay's draws, which the scene keeps byte for byte while the HUD's
    // code moves: a literal moves only when what the HUD draws changes on purpose. The seats
    // stand their whole widths apart, and seat 2 a whole width off the chest beside the start,
    // where its help is drawn over it.
    CHECK(test::hexBytes(test::drawStreamDigest(overlay)) ==
          "861ce8c0b41c538dea9fd714397b1b39184e15cdfeae011bec617fe8dc64b36d");
    scene.close();
}

/** The vertices of `draws`, in the order they were drawn. */
std::vector<ImmediateVertex> verticesOf(std::span<const test::RecordedDraw> draws) {
    std::vector<ImmediateVertex> vertices;
    for (const test::RecordedDraw& draw : draws) {
        vertices.insert(vertices.end(), draw.vertices.begin(), draw.vertices.end());
    }
    return vertices;
}

TEST_CASE("a G1 scene's boxes show the bosses' keys while the relics show, and then no more",
          "[play-scene][hud-draw-pin][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    GameConfig config;
    config.display.bloom = true; // where the bloom ran, the overlay begins
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &levels;
    context.unpackedRoot = root;

    // One player carrying two of the bosses' keys.
    std::vector<PartyMember> party;
    {
        CharacterSave save;
        save.name = "AB";
        save.character = 2;
        save.color = 1;
        save.progress().health = 4000;
        REQUIRE(save.progress().relics.addShard(1));
        REQUIRE(save.progress().relics.addShard(2));
        party.push_back({0, save});
    }
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));

    // The boxes as Dark Legacy's look draws them for the scene's party, with the keys or
    // without, to be found among the scene's own draws.
    test::FakeRenderDevice expectedDevice;
    DarkLegacyHudPainter expected;
    REQUIRE(expected.load(expectedDevice, root, &strings));
    const PowerupSelector closed;
    const PickupHud noPickups;
    const ClassDataSet classes;
    const Mat4 frameProjection = makeScreenProjection(640, 448);
    const Mat4 canvasProjection = makeVirtualScreenTransform(frameProjection, 512, 384, 640, 448);
    const auto boxes = [&](bool keysShown) {
        HudView view;
        view.players = scene.participants();
        view.pickups = &noPickups;
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            view.seats[seat] = hudSeatOf(static_cast<s32>(seat), scene.participants(), closed,
                                         classes, {.keysShown = keysShown});
        }
        expectedDevice.draws.clear();
        Canvas canvas;
        HudFrame frame{.canvas = canvas, .device = expectedDevice, .darkLegacy = expected};
        canvas.begin(expectedDevice, canvasProjection);
        expected.drawStatus(frame, view);
        canvas.end();
        return verticesOf(expectedDevice.draws);
    };
    const auto overlay = [&] {
        device.draws.clear();
        device.bloomDrawOffsets.clear();
        scene.render(device, frameProjection, 640, 448);
        REQUIRE(device.bloomDrawOffsets.size() == 1);
        return verticesOf(std::span{device.draws}.subspan(device.bloomDrawOffsets.front()));
    };
    const auto holds = [](const std::vector<ImmediateVertex>& drawn,
                          const std::vector<ImmediateVertex>& boxes) {
        return std::ranges::search(drawn, boxes).begin() != drawn.end();
    };

    // As the level opens the keys show in the box,
    scene.update(1.0 / 60.0, PlayScene::Inputs{});
    REQUIRE(scene.hud().relicsShown());
    const auto opening = overlay();
    const auto keyed = boxes(true);
    const auto keyless = boxes(false);
    REQUIRE_FALSE(keyless.empty());
    REQUIRE(keyed != keyless);
    CHECK(holds(opening, keyed));
    CHECK_FALSE(holds(opening, keyless));
    // and once the relics' while is over they go.
    for (s32 tick = 0; tick <= PartyHud::kRelicTicks && scene.hud().relicsShown(); ++tick) {
        scene.update(1.0 / 60.0, PlayScene::Inputs{});
    }
    REQUIRE_FALSE(scene.hud().relicsShown());
    const auto later = overlay();
    CHECK(holds(later, boxes(false)));
    CHECK_FALSE(holds(later, boxes(true)));
    expected.clear();
    scene.close();
}

} // namespace
