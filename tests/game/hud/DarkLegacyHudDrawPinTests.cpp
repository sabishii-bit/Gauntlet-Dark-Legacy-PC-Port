#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

#include "DrawStreamDigest.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/audio/LevelSoundscape.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudView.h"
#include "game/hud/LevelMessages.h"
#include "game/hud/PartyHud.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/RuneMeter.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerControls.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** One part of the HUD as it was drawn: the SHA-256 of its draws, and how many textures the
 * device had created and updated by then, which pins when the art is uploaded. */
struct DrawnPart {
    std::string draws;
    u32 created = 0;
    u32 updated = 0;
};

/** What a part is held to. */
struct PinnedPart {
    std::string_view name;
    std::string_view draws;
    u32 created = 0;
    u32 updated = 0;
};

CharacterSave member(s32 character, s32 color, std::string_view name) {
    CharacterSave save;
    save.name = std::string(name);
    save.character = character;
    save.color = color;
    return save;
}

/** The lich's meter: two backed strips, capped 44 in and 53 short of the end. */
HealthMeterDefinition lichMeter() {
    HealthMeterDefinition meter;
    meter.pieces = 2;
    meter.advance = 256;
    meter.leftInset = 44;
    meter.rightInset = 53;
    meter.shown = true;
    meter.backed = true;
    return meter;
}

TEST_CASE("the Dark Legacy HUD draws stay byte for byte what they were",
          "[game][hud][hud-draw-pin][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    const auto powerupsDirectory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    const auto lichDirectory = test::assetOrSkip("MONSTERS/LICH/textures.ngc").parent_path();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;

    // Everything is taken as the scene takes it: the boxes and help first, then the scene's
    // STATIC set with the messages' FONT32, the glow and the finder borrowed from that set,
    // and the hourglasses' art from POWERUPS.
    DarkLegacyHudPainter look;
    REQUIRE(look.load(device, root, &strings));
    PartyHud hud;
    hud.load(root, &strings);
    TextureSet sceneStatic;
    LevelMessages messages;
    messages.load(device, sceneStatic, root, &strings);
    REQUIRE(messages.text().ready());
    const auto staticArt = [&](std::string_view name) -> const Texture& {
        const auto index = sceneStatic.find(name);
        REQUIRE(index);
        return sceneStatic.texture(device, *index);
    };
    look.setGlow(&staticArt("FONT32_GLOW"));
    look.setHelpScroll(&sceneStatic);
    ItemArchive powerups;
    REQUIRE(powerups.load(powerupsDirectory));
    REQUIRE(hud.bindHourglass(device, powerups));
    ChallengeHud challenge;
    REQUIRE(challenge.bind(device, powerups));
    const Texture& finderFrame = staticArt("THERMBASE");
    const Texture& finderColumn = staticArt("THERMCOL");
    look.setRuneMeter(&finderFrame, &finderColumn);
    ItemArchive lich;
    REQUIRE(lich.load(lichDirectory));
    const u32 loadedTextures = device.texturesCreated;
    const u32 loadedUpdates = device.textureUpdates;

    const Mat4 canvasProjection =
        makeVirtualScreenTransform(makeScreenProjection(640, 448), 512, 384, 640, 448);
    // The view's clip transform is the canvas's own here, so a head lies where its x and y say.
    const Mat4& clip = canvasProjection;
    Canvas canvas;
    HudFrame frame{.canvas = canvas, .device = device, .darkLegacy = look};
    std::vector<DrawnPart> drawn;
    const auto draw = [&](const auto& part) {
        device.draws.clear();
        canvas.begin(device, canvasProjection);
        part();
        canvas.end();
        REQUIRE_FALSE(device.draws.empty());
        drawn.push_back({test::hexBytes(test::drawStreamDigest(device.draws)),
                         device.texturesCreated, device.textureUpdates});
    };
    // The seats the scene hands its HUD look for `party`, each from that seat's own player.
    const ClassDataSet classes;
    const auto seatsOf = [&](HudView& view, std::span<const PlayerRuntime> party,
                             bool quitOffered) {
        view.players = party;
        view.relicsShown = hud.relicsShown();
        view.quitOffered = quitOffered;
        view.pickups = &hud.pickups();
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            const auto index = static_cast<s32>(seat);
            view.seats[seat] = hudSeatOf(
                index, party, hud.selector(index), classes,
                {.keysShown = view.relicsShown, .quitOffered = quitOffered, .local = true});
        }
    };

    // An online party of two: seat 1 fallen and asked only to wait, seat 3 gone from the
    // game, seats 0 and 2 never joined.
    std::array<PlayerRuntime, 2> online;
    online[0].actor.spawn(1, member(5, 1, "ODO"), nullptr, Vec3{0}, 0);
    online[0].life = PlayerLife::InTower;
    online[0].towerPrompt = true;
    online[1].actor.spawn(3, member(2, 3, "IVO"), nullptr, Vec3{0}, 0);
    online[1].departed = true;
    draw([&] {
        HudView view;
        seatsOf(view, online, false);
        look.drawStatus(frame, view);
    });

    // Four players, out of seat order: seat 2 standing with all a box shows, seat 0 fallen
    // and asked to wait in the tower or quit, seat 3 waiting in the tower, and seat 1
    // standing with little.
    LevelSoundscape audio;
    std::array<PlayerRuntime, 4> players;
    {
        CharacterSave save = member(1, 3, "ROWENA");
        save.gold = 1234;
        save.progress().experience = 25000;
        save.progress().health = 760;
        Inventory& carried = save.progress().inventory;
        carried.addKeys(3);
        carried.addPotions(2, 1);
        carried.addPotions(4, 1);
        carried.powerups[0] = PowerupSlot{60, powerup::kSpecial, 0, powerup::kStopTime, true};
        carried.powerups[1] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, true};
        // Runes 1, 5, 11, 12 and the thirteenth; keys 1 and 5 to 8, whose colours repeat.
        save.progress().relics.runes = 0x1C11;
        save.progress().relics.shards = 0xF1;
        players[0].actor.spawn(2, save, nullptr, Vec3{0}, 0);
    }
    players[1].actor.spawn(0, member(4, 0, "ULRIC"), nullptr, Vec3{0}, 0);
    players[1].life = PlayerLife::InTower;
    players[1].towerPrompt = true;
    players[2].actor.spawn(3, member(6, 2, "MAGDA"), nullptr, Vec3{0}, 0);
    players[2].life = PlayerLife::InTower;
    {
        CharacterSave save = member(0, 1, "THOR");
        save.gold = 50;
        save.progress().health = 230;
        save.progress().inventory.addPotions(1, 1);
        save.progress().inventory.powerups[0] = PowerupSlot{12.5f, powerup::kSpeed, 5, 0, false};
        save.progress().relics.runes = 0x0007;
        players[3].actor.spawn(1, save, nullptr, Vec3{192, 220, 0}, 0);
    }
    players[0].turbo.add(100);
    players[0].turbo.step(110);
    players[3].turbo.add(45);
    players[3].turbo.step(20);
    // Stop Time is taken first, which sets the shared total, then invisibility is named;
    // seat 1's speed is named switched off.
    hud.focusPickup(players[0].actor, powerup::kSpecial, powerup::kStopTime);
    hud.focusPickup(players[0].actor, powerup::kSpecial, powerup::kInvisible);
    hud.focusPickup(players[3].actor, powerup::kSpeed, 0);
    for (const usize opening : {usize{0}, usize{3}}) {
        PlayerActor& actor = players[opening].actor;
        hud.stepSelector(actor, SelectorInput{.up = true},
                         PowerupSelector::kSlide / PowerupSelector::kSlidePerTick, audio);
        hud.stepSelector(actor, {}, 1, audio);
        REQUIRE(hud.selector(actor.player()).showing());
    }
    players[0].actor.save().progress().inventory.advance(15);
    hud.stepHourglass(0.1f, players);
    challenge.step(0.2f);
    hud.showRelics();
    hud.pickups().addCard(1, PickupHud::kCrystalCard);
    hud.pickups().showCount(2, PickupHud::crystalIcon(1), 3, 15);
    hud.pickups().step(30, 0.5f);

    std::array<PartyMember, 1> finders{};
    REQUIRE(finders[0].save.progress().relics.addShard(9));
    RuneMeter finder;
    finder.begin(0, Vec3{0}, Vec3{100, 0, 0}, 7, finders);
    finder.update(Vec3{30, 0, 0}, true);
    REQUIRE(finder.visible());

    BossMeters meters;
    std::array<HealthMeterReading, 1> readings;
    readings[0].definition = lichMeter();
    readings[0].health = 2100;
    readings[0].maxHealth = 3000;
    meters.bind(readings, &lich.textures);

    draw([&] {
        HudView view;
        seatsOf(view, players, true);
        look.drawStatus(frame, view);
    });
    draw([&] {
        HudView view;
        view.runeFill = finder.shownFill();
        look.drawRuneMeter(frame, view);
    });
    draw([&] {
        HudView view;
        view.hourglass = hud.hourglassLook(players);
        view.hourglassArt = &hud.hourglass();
        REQUIRE(view.hourglass.has_value());
        look.drawHourglass(frame, view);
    });
    draw([&] {
        HudView view;
        view.hourglass = challenge.look(40, 90, true);
        view.hourglassArt = &challenge;
        look.drawHourglass(frame, view);
    });
    draw([&] {
        HudView view;
        view.bossMeters = &meters;
        look.drawBossMeters(frame, view);
    });
    draw([&] {
        HudView view;
        seatsOf(view, players, true);
        view.strings = &strings;
        view.largeText = &messages.text();
        look.drawSelectors(frame, view);
    });
    REQUIRE(hud.postHelp(HelpMessages::kChestNeedsKey, 3, players, audio));
    REQUIRE(hud.help().player() == 1);
    REQUIRE(hud.helpAnchor(players).has_value());
    const auto drawHelp = [&] {
        HudView view;
        view.help = hudHelpOf(hud.help(), hud.helpAnchor(players), players, clip, canvasProjection,
                              512, 384);
        look.drawHelp(frame, view);
    };
    draw(drawHelp);
    // A message for nobody in particular has no anchor, and sits at the canvas's centre.
    hud.help().clear();
    REQUIRE(hud.help().post(HelpMessages::kLevelUp, -1, {}, 12) != nullptr);
    REQUIRE_FALSE(hud.helpAnchor(players).has_value());
    draw(drawHelp);

    // SHA-256 of each part's draws, with the textures the device had created and updated by
    // then. Dark Legacy's look keeps every one while its code moves: a literal moves only when
    // what is drawn, or when its art is uploaded, changes on purpose.
    CHECK(loadedTextures == 36);
    CHECK(loadedUpdates == 0);
    constexpr std::array kPins{
        PinnedPart{"an online party's empty, waiting and departed boxes",
                   "62c8b795bee839045d55ec213fab09dcd776cafd87fd578ef918bd726c12c405", 41, 0},
        PinnedPart{"four boxes with their relic strip, a card and a ticker",
                   "5dbe64b3d8dbbde8c0138c2ff75f089f4d60fab322a4855de3d566fa3144dc3c", 66, 0},
        PinnedPart{"the runestone finder",
                   "aacee0ad1d34a9a6afbd24be90bcc40b422a15ec6eb58be6197fa59945383943", 66, 0},
        PinnedPart{"Stop Time's hourglass",
                   "1ab3c585c320206c0396cedd741112407979c46fd1358fc2e356569d06b99e04", 66, 0},
        PinnedPart{"the secret level's hourglass",
                   "cd0d77379a4f897f61e6dfe8f593d242b858bc97116e71484870d2f20c88a6a1", 66, 0},
        PinnedPart{"a two-piece boss meter",
                   "557224741e0f2a4cfaac554c30028f9e77064a82ff6c93dca148e7c4365e8018", 70, 0},
        PinnedPart{"two selector labels, one worn",
                   "7926dea73c88887d5b4daf7b2efa5e3a8d49ed91e31c088fc0564954642438f0", 70, 0},
        PinnedPart{"help over a player's head",
                   "d187318adb7c55ed64f6ed9cef886891f3b5af027b6aa57cfc82bea54db2423d", 70, 0},
        PinnedPart{"help at the canvas's centre",
                   "e17bd1bbc37afa14dc64df39e8347efc88968420c8e2a5d7021bc801a9aaf844", 70, 0},
    };
    REQUIRE(drawn.size() == kPins.size());
    for (usize part = 0; part < kPins.size(); ++part) {
        CAPTURE(kPins[part].name);
        CHECK(drawn[part].draws == kPins[part].draws);
        CHECK(drawn[part].created == kPins[part].created);
        CHECK(drawn[part].updated == kPins[part].updated);
    }
    meters.clear();
    challenge.clear();
    hud.clear();
    look.clear();
}

} // namespace
