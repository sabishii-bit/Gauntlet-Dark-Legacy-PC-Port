#include <array>
#include <format>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/characters/NameCheats.h"
#include "game/hud/PartyHud.h"
#include "game/players/PowerupEffects.h"
namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("level announcement queues gained-level behind the color and character name",
          "[game][screens][party-hud]") {
    const auto root = test::scratchDirectory("party-hud-level-voice");
    const auto bank = [&](std::string_view folder, std::string_view name, s16 value) {
        const auto path = root / "audio" / folder;
        std::filesystem::create_directories(path);
        const std::array<s16, 4> pcm{value, value, value, value};
        const std::array<test::NativeSoundSample, 1> bankSamples{
            {{48000, {pcm.begin(), pcm.end()}}}};
        test::writeNativeSoundBank(path,
                                   std::format(R"({{"sounds":[{{"index":0,
            "name":"{}","volume":127,"sequence":[{{"sample":0,"loopStart":true,"loopBack":true}}]}}]}})",
                                               name),
                                   bankSamples);
    };
    const CharacterSave save;
    const std::string name =
        std::format("S_{}{}2", colorCode(save.color), classCode(save.character));
    bank("CHARACTER", name, 8192);
    bank("VOICE1", "S_GAINEDLEVEL", -8192);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(root, &sounds, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    players[0].figure = std::make_unique<PlayerFigure>();
    REQUIRE(players[0].figure->voice().load(root / "audio/CHARACTER"));
    PartyHud hud;
    writeTextFile(root / "messages.json",
                  R"({"messages":[{"name":"LEVELUP","lines":["LEVEL %d"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    hud.help().setTexts(&messages);
    REQUIRE(hud.postHelp(HelpMessages::kLevelUp, 0, players, audio, 2));
    REQUIRE(sounds.voiceCount() == 1); // The narrator waits, rather than speaking over the name.
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0); // Positive samples identify the color+character bank.
    audio.close();
}

TEST_CASE("world-anchored trap lessons use neutral ink and retain party history",
          "[game][screens][party-hud]") {
    const auto root = test::scratchDirectory("hud-trap-lesson");
    writeTextFile(root / "messages.json",
                  R"({"messages":[{"name":"TRAPMOVE","lines":["TRAPS MAKE SOME OBJECTS MOVE"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    PartyHud hud;
    hud.help().setTexts(&messages);
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    REQUIRE(hud.postHelp(HelpMessages::kTrapsMove, 0, players, audio, -1, Vec3{1, 2, 3}));
    REQUIRE(hud.help().player() == -1);
    REQUIRE(players[0].actor.save().helpSeen == std::vector<s32>{5});
    REQUIRE(HelpMessages::specOf(5)->voice == "S_TRAPSMAKE");
    REQUIRE_FALSE(hud.postHelp(5, 0, players, audio, -1, Vec3{1, 2, 3}));
}

TEST_CASE("party HUD status uses player identity rather than party order",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 2> players;
    CharacterSave save;
    save.name = "THREE";
    save.gold = 42;
    save.progress().health = 700;
    save.progress().inventory.addKeys(2);
    save.progress().inventory.addPotions(3, 2);
    players[0].actor.spawn(3, save, nullptr, Vec3{0}, 0);
    save.name = "ONE";
    players[1].actor.spawn(1, save, nullptr, Vec3{0}, 0);
    const auto status = PartyHud::status(3, players);
    REQUIRE(status.active);
    REQUIRE(status.name == "THREE");
    REQUIRE(status.health == 700);
    REQUIRE(status.keys == 2);
    REQUIRE(status.potions == 2);
    REQUIRE(status.potionKind == 3);
    REQUIRE(status.gold == 42);
    REQUIRE(status.turbo.has_value());
    REQUIRE(PartyHud::status(1, players).name == "ONE");
    REQUIRE_FALSE(PartyHud::status(0, players).active);
    REQUIRE_FALSE(PartyHud::status(-1, players).active);
}

TEST_CASE("party HUD hides carried goods and health while a character falls",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 1;
    players[0].actor.save().progress().inventory.addKeys(5);
    players[0].life = PlayerLife::Dying;
    const auto dying = PartyHud::status(2, players);
    REQUIRE(dying.active);
    REQUIRE(dying.health == 0);
    REQUIRE(dying.keys == 0);
    REQUIRE_FALSE(dying.inTower);
    REQUIRE_FALSE(dying.turbo.has_value());
    players[0].life = PlayerLife::InTower;
    REQUIRE(PartyHud::status(2, players).inTower);
    REQUIRE_FALSE(PartyHud::status(2, players).towerPrompt);
    // Fallen out of the tower, the box asks whether to wait there or quit; gone, it is empty.
    players[0].towerPrompt = true;
    REQUIRE(PartyHud::status(2, players).towerPrompt);
    players[0].towerPrompt = false;
    players[0].departed = true;
    REQUIRE_FALSE(PartyHud::status(2, players).active);
}

TEST_CASE("party HUD clears transient presentation without altering participants",
          "[game][screens][party-hud]") {
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 123;
    REQUIRE_FALSE(hud.postHelp(0, 9, players, audio));
    hud.pickups().addCard(3, "GOLD");
    hud.clear();
    hud.clear();
    REQUIRE_FALSE(hud.help().showing());
    REQUIRE_FALSE(hud.selector(3).showing());
    REQUIRE(players[0].actor.save().health() == 123);
}

TEST_CASE("pickup focus and usage are private to each player's controller identity",
          "[game][screens][party-hud][selector][multiplayer]") {
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].actor.spawn(i == 0 ? 3 : 1, {}, nullptr, Vec3{0}, 0);
        Inventory& inventory = players[i].actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpeed, 0, 5, 12.3f);
        inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 30);
    }
    hud.focusPickup(players[0].actor, powerup::kSpeed, 0);
    hud.focusPickup(players[1].actor, powerup::kSpecial, powerup::kGrowth);
    REQUIRE(hud.selector(3).selection() == 0);
    REQUIRE(hud.selector(1).selection() == 1);
    REQUIRE(hud.selector(0).selection() == -1);
    REQUIRE(PartyHud::status(3, players, &hud.selector(3)).powerup->strength == 12.3f);
    REQUIRE(PartyHud::status(1, players, &hud.selector(1)).powerup->strength == 30);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 32, audio);
    hud.stepSelector(players[0].actor, {}, 1, audio);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 1, audio);
    REQUIRE_FALSE(players[0].actor.save().progress().inventory.powerups[0].on);
    REQUIRE(players[1].actor.save().progress().inventory.powerups[0].on);
    REQUIRE(PartyHud::status(3, players, &hud.selector(3)).powerup->flags == powerup::kGrowth);
    players[0].life = PlayerLife::Dying;
    REQUIRE_FALSE(PartyHud::status(3, players, &hud.selector(3)).powerup);
    players[1].departed = true;
    REQUIRE_FALSE(PartyHud::status(1, players, &hud.selector(1)).powerup);
}
TEST_CASE("crystal-style item announcements use common audio and do not replay when seen",
          "[game][screens][party-hud][items]") {
    const auto root = test::scratchDirectory("party-hud-item-voice");
    const auto path = root / "audio/COMMON";
    std::filesystem::create_directories(path);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {pcm.begin(), pcm.end()}}}};
    test::writeNativeSoundBank(path, R"({"sounds":[{"name":"S_PICKUPCRYST","volume":127,
        "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}]})",
                               bankSamples);
    writeTextFile(root / "messages.json", R"({"messages":[{"name":"MIKEY","lines":["MIKEY"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(root, &sounds, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    PartyHud hud;
    hud.help().setTexts(&messages);
    REQUIRE(hud.postHelp(148, 0, players, audio));
    REQUIRE(sounds.voiceCount() == 1);
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0);
    hud.help().update(1000);
    REQUIRE_FALSE(hud.postHelp(148, 0, players, audio));
    REQUIRE(sounds.voiceCount() == 1);
    audio.close();
}

TEST_CASE("Stop Time sand follows native pickup totals and first active player identity",
          "[game][screens][party-hud][stop-time-hud][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    PartyHud hud;
    REQUIRE(hud.bindHourglass(device, archive));
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    auto& third = players[0].actor.save().progress().inventory;
    auto& first = players[1].actor.save().progress().inventory;
    third.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 120);
    hud.focusPickup(players[0].actor, powerup::kSpecial, powerup::kStopTime);
    first.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
    hud.focusPickup(players[1].actor, powerup::kSpecial, powerup::kStopTime);
    third.advance(100);
    first.advance(30);
    hud.stepHourglass(0, players);
    Canvas canvas;
    const auto draw = [&] {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        const bool shown = hud.drawHourglass(canvas, players);
        canvas.end();
        return shown;
    };
    const auto expectSand = [&](f32 remaining, f32 total) {
        REQUIRE(draw());
        REQUIRE(device.draws.size() == 3);
        REQUIRE(device.draws.back().vertices.size() == 12);
        const auto expected = ChallengeHud::sand(remaining, total);
        CHECK(device.draws.back().vertices[0].position.y == expected.upper.y);
        CHECK(device.draws.back().vertices[6].position.y == expected.lower.y);
    };
    expectSand(30, 60); // Controller 1 wins, although controller 3 is first in the vector.
    const Texture* falling = device.draws[1].texture;
    hud.stepHourglass(1.0f / 30, players);
    expectSand(30, 60);
    CHECK(device.draws[1].texture != falling);
    falling = device.draws[1].texture;
    hud.stepHourglass(0, players); // A paused update cannot advance the falling grains.
    expectSand(30, 60);
    CHECK(device.draws[1].texture == falling);

    first.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
    hud.focusPickup(players[1].actor, powerup::kSpecial, powerup::kStopTime);
    first.advance(15);
    expectSand(45, 60); // Renewed duration is 30 + half of 60, not the old total.
    third.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 40);
    hud.focusPickup(players[0].actor, powerup::kSpecial, powerup::kStopTime);
    first.advance(25);
    expectSand(20, 40); // The most recent pickup resets the shared total for every wearer.
    first.powerups[0].on = false;
    expectSand(40, 40);
    first.powerups[0].on = true;
    expectSand(20, 40); // Switching back on must not refill the sand.
    players[1].life = PlayerLife::Dying;
    expectSand(40, 40);
    players[0].departed = true;
    CHECK_FALSE(draw());
    CHECK(device.draws.empty());
    players[1].life = PlayerLife::Standing;
    first.advance(20);
    CHECK_FALSE(draw());

    hud.clear();
    REQUIRE(hud.bindHourglass(device, archive));
    first.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 10);
    hud.stepHourglass(0, players); // A carried/scenario item starts with its remaining total.
    expectSand(10, 10);
}

TEST_CASE("permanent Stop Time keeps the signed shared timer ratio",
          "[game][screens][party-hud][stop-time-hud][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    PartyHud hud;
    REQUIRE(hud.bindHourglass(device, archive));
    CharacterSave save;
    save.name = "NOVATO";
    REQUIRE(applyNameCheats(save));
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    auto& permanent = players[0].actor.save().progress().inventory;
    REQUIRE(permanent.powerups[0].strength == -1);
    hud.stepHourglass(0, players);
    Canvas canvas;
    const auto expectSand = [&](bool full) {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        REQUIRE(hud.drawHourglass(canvas, players));
        canvas.end();
        REQUIRE(device.draws.size() == 3);
        REQUIRE(device.draws.back().vertices.size() == 12);
        const auto expected = ChallengeHud::sand(full ? 1.0f : 0.0f, 1);
        CHECK(device.draws.back().vertices[0].position.y == expected.upper.y);
        CHECK(device.draws.back().vertices[6].position.y == expected.lower.y);
    };
    expectSand(true); // (-1 - -1) / -1 has zero elapsed time.
    permanent.advance(1000);
    expectSand(true);

    auto& finite = players[1].actor.save().progress().inventory;
    finite.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
    hud.focusPickup(players[1].actor, powerup::kSpecial, powerup::kStopTime);
    expectSand(false); // A finite pickup replaces the shared total, even for NOVATO.
    hud.focusPickup(players[0].actor, powerup::kSpecial, powerup::kStopTime);
    expectSand(true);
    permanent.powerups[0].on = false;
    expectSand(false); // A finite wearer with a negative total is over-empty, not full.
}

TEST_CASE("parent and secret-level hourglasses share pickup totals across visits",
          "[game][screens][party-hud][stop-time-hud][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(root));
    f32 sharedTotal = 60;
    PartyHud parent;
    PartyHud secret;
    REQUIRE(parent.bindHourglass(device, archive, &sharedTotal));
    REQUIRE(secret.bindHourglass(device, archive, &sharedTotal));
    std::array<PlayerRuntime, 1> parentPlayers;
    std::array<PlayerRuntime, 1> secretPlayers;
    CharacterSave save;
    save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 20);
    parentPlayers[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    secretPlayers[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    parent.stepHourglass(0, parentPlayers);
    secret.stepHourglass(0, secretPlayers);
    CHECK(sharedTotal == 60);
    Canvas canvas;
    const auto expectSand = [&](PartyHud& hud, std::span<const PlayerRuntime> players,
                                f32 remaining, f32 total) {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        REQUIRE(hud.drawHourglass(canvas, players));
        canvas.end();
        REQUIRE(device.draws.size() == 3);
        REQUIRE(device.draws.back().vertices.size() == 12);
        const auto expected = ChallengeHud::sand(remaining, total);
        CHECK(device.draws.back().vertices[0].position.y == expected.upper.y);
        CHECK(device.draws.back().vertices[6].position.y == expected.lower.y);
    };
    expectSand(parent, parentPlayers, 20, 60);
    expectSand(secret, secretPlayers, 20, 60);
    secretPlayers[0].actor.save().progress().inventory.addPowerup(powerup::kSpecial,
                                                                  powerup::kStopTime, 0, 40);
    secret.focusPickup(secretPlayers[0].actor, powerup::kSpecial, powerup::kStopTime);
    CHECK(sharedTotal == 40); // Twenty remaining plus half of the new forty.
    expectSand(secret, secretPlayers, 40, 40);
    expectSand(parent, parentPlayers, 20, 40); // The suspended scene sees the same global total.
    secret.clear();
    CHECK(sharedTotal == 40);
    expectSand(parent, parentPlayers, 20, 40);
    REQUIRE(secret.bindHourglass(device, archive, &sharedTotal));
    secret.stepHourglass(0, secretPlayers);
    expectSand(secret, secretPlayers, 40, 40);
    parent.clear();
    CHECK(sharedTotal == 40);
}
TEST_CASE("native inventory labels remain below crystal counts in every player lane",
          "[party-hud][selector][inventory-label][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    TextureSet art;
    REQUIRE(art.load(root / "STATIC"));
    BitmapFont font;
    REQUIRE(font.load(root / "FONTS/font32.fnt", 16));
    const auto sheet = art.find("FONT32");
    REQUIRE(sheet);
    TextPainter text;
    text.setFont(&font, &art.texture(device, *sheet));
    StatusBoxPainter boxes;
    REQUIRE(boxes.load(device, root, &strings));
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    Canvas canvas;
    for (s32 lane = 0; lane < 4; ++lane) {
        CAPTURE(lane);
        players[0].actor.spawn(lane, {}, nullptr, Vec3{0}, 0);
        auto& inventory = players[0].actor.save().progress().inventory;
        inventory.powerups[0] = {30, powerup::kSpecial, 0, powerup::kInvisible, false};
        hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 32, audio);
        hud.stepSelector(players[0].actor, {}, 1, audio);
        REQUIRE(hud.selector(lane).showing());
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        const std::array<StatusBoxPainter::CountEntry, 1> entries{{{"SM_CRYSTAL_BLU", 4, 5}}};
        boxes.drawCounts(canvas, lane, entries,
                         PickupHud::countRowY(hud.selector(lane).labelY(StatusBoxPainter::kY)));
        canvas.end();
        REQUIRE_FALSE(device.draws.empty());
        f32 countBottom = 0;
        for (const auto& draw : device.draws) {
            countBottom = std::max(countBottom, test::maxCorner(draw).y);
        }
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        hud.drawSelectors(canvas, text, &strings, players);
        canvas.end();
        REQUIRE(device.draws.size() == 1);
        CHECK(test::minCorner(device.draws.front()).y > countBottom);
        CHECK(test::maxCorner(device.draws.front()).y < StatusBoxPainter::kRuneY);
        CHECK(StatusBoxPainter::kRuneY - test::maxCorner(device.draws.front()).y < 1.0f);
        const std::string_view label = strings.get("powerup.invisible");
        const f32 scale = PowerupSelector::labelScale(text, label);
        const s32 top = hud.selector(lane).labelY(StatusBoxPainter::kY);
        const s32 fitted =
            top + text.lineHeight(PowerupSelector::kLabelScale) - text.lineHeight(scale);
        const s32 center = -(lane * StatusBoxPainter::kWidth + StatusBoxPainter::kWidth / 2);
        test::FakeRenderDevice expected;
        canvas.begin(expected, Mat4{1});
        text.draw(canvas, center, fitted, label, TextStyle{scale, Color::white()});
        canvas.end();
        REQUIRE(expected.draws.size() == 1);
        CHECK(device.draws.front().vertices == expected.draws.front().vertices);
        // Labels stay inside their lane, centered within glyph rounding.
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        PartyHud::drawSelector(canvas, text, &strings, nullptr, lane,
                               PowerupSlot{30, powerup::kSpecial, 0, powerup::kEnemyShrink, false},
                               top);
        canvas.end();
        REQUIRE(device.draws.size() == 1);
        const f32 laneLeft = static_cast<f32>(lane * StatusBoxPainter::kWidth);
        const f32 laneRight = laneLeft + static_cast<f32>(StatusBoxPainter::kWidth);
        const Vec2 low = test::minCorner(device.draws.front());
        const Vec2 high = test::maxCorner(device.draws.front());
        CHECK(low.x >= laneLeft);
        CHECK(high.x <= laneRight);
        CHECK(low.x - laneLeft == Catch::Approx(laneRight - high.x).margin(2.5f));
    }
}

TEST_CASE("party HUD rewind restores notices selectors shared sand and external help history",
          "[party-hud][party-hud-local-rewind]") {
    const auto root = test::scratchDirectory("party-hud-local-rewind");
    writeTextFile(root / "messages.json", R"({"messages":[
        {"name":"TRAPMOVE","lines":["TRAPS MOVE"]},
        {"name":"LEVELUP","lines":["LEVEL %d"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    struct State {
        test::FakeRenderDevice device;
        ItemArchive archive;
        LevelSoundscape audio;
        f32 total = 0;
        PartyHud hud;
        std::array<PlayerRuntime, 2> players;
    } expected, corrected;
    for (auto* f : {&expected, &corrected}) {
        // Failed artwork binding still retains the application's shared total.
        REQUIRE_FALSE(f->hud.bindHourglass(f->device, f->archive, &f->total));
        f->hud.help().setTexts(&messages);
        for (usize i = 0; i < f->players.size(); ++i) {
            auto& actor = f->players[i].actor;
            actor.spawn(i == 0 ? 3 : 1, {}, nullptr, Vec3{static_cast<f32>(i), 2, 3}, 0);
            actor.save().progress().inventory.addPowerup(powerup::kSpeed, 0, 5, 120);
            actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0,
                                                         60);
        }
    }
    const auto tick = [](State& f, s32 t) {
        const auto index = static_cast<usize>((t / 9) % 2);
        auto& actor = f.players[index].actor;
        if (t % 29 == 0) {
            f.hud.pickups().addCard(actor.player(), "CRYSTAL");
            f.hud.pickups().showCount(actor.player(), "SM_CRYSTAL_ORA", t, 250);
            f.hud.showRelics();
        }
        if (t % 41 == 0) {
            f.hud.focusPickup(actor, powerup::kSpecial, powerup::kStopTime);
        }
        SelectorInput input;
        input.up = t % 43 == 0;
        input.right = t % 11 == 0;
        input.down = t % 53 == 0;
        f.hud.stepSelector(actor, input, 1, f.audio);
        f.hud.pickups().step(1, 1.0f / 30);
        f.hud.stepHourglass(1.0f / 60, f.players);
        f.hud.help().update(1);
        f.hud.stepRelics(1);
        return f.hud.postHelp(t < 40 ? HelpMessages::kTrapsMove : HelpMessages::kLevelUp, index,
                              f.players, f.audio, t, Vec3{1, 5, 7});
    };
    s32 at = 0;
    s32 posted = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 delay = 1 + window % 8;
        CAPTURE(window, at, delay);
        PartyHud::LocalSnapshot saved;
        corrected.hud.saveLocal(saved);
        const std::array<PlayerSimulation, 2> party{corrected.players[0].simulation(),
                                                    corrected.players[1].simulation()};
        REQUIRE_FALSE(expected.hud.restoreLocal(saved));
        std::vector<bool> events;
        for (s32 t = at; t < at + delay; ++t) {
            events.push_back(tick(expected, t));
            corrected.total = 800;
            corrected.hud.help().clear();
            corrected.hud.pickups().clear();
            corrected.hud.stepRelics(1000);
            corrected.hud.stepSelector(corrected.players[0].actor,
                                       SelectorInput{.up = true, .right = true}, 20,
                                       corrected.audio);
            corrected.hud.postHelp(HelpMessages::kLevelUp, 0, corrected.players, corrected.audio,
                                   99, Vec3{9});
        }
        const auto created = corrected.device.texturesCreated;
        const auto uploaded = corrected.device.textureUpdates;
        REQUIRE(corrected.hud.restoreLocal(saved));
        for (usize i = 0; i < party.size(); ++i) {
            corrected.players[i].simulation() = party[i];
        }
        for (s32 t = at; t < at + delay; ++t) {
            const bool event = tick(corrected, t);
            CHECK(event == events[static_cast<usize>(t - at)]);
            posted += event ? 1 : 0;
        }
        CHECK(corrected.device.texturesCreated == created);
        CHECK(corrected.device.textureUpdates == uploaded);
        CHECK(corrected.total == expected.total);
        CHECK(corrected.hud.help().lines() == expected.hud.help().lines());
        CHECK(corrected.hud.help().showing() == expected.hud.help().showing());
        CHECK(corrected.hud.help().id() == expected.hud.help().id());
        CHECK(corrected.hud.helpAnchor(corrected.players) ==
              expected.hud.helpAnchor(expected.players));
        CHECK(corrected.hud.relicsShown() == expected.hud.relicsShown());
        const auto& a = corrected.hud.pickups().cards();
        const auto& b = expected.hud.pickups().cards();
        REQUIRE(a.size() == b.size());
        for (usize i = 0; i < a.size(); ++i) {
            CHECK(a[i].player == b[i].player);
            CHECK(a[i].texture == b[i].texture);
            CHECK(a[i].y == b[i].y);
            CHECK(a[i].state == b[i].state);
            CHECK(a[i].holdTicks == b[i].holdTicks);
        }
        for (usize i = 0; i < party.size(); ++i) {
            const auto& player = corrected.players[i];
            const auto& reference = expected.players[i];
            const s32 id = player.actor.player();
            CHECK(player.actor.save().helpSeen == reference.actor.save().helpSeen);
            CHECK(player.helpHeard == reference.helpHeard);
            CHECK(corrected.hud.selector(id).selection() == expected.hud.selector(id).selection());
            CHECK(corrected.hud.selector(id).state() == expected.hud.selector(id).state());
            CHECK(corrected.hud.selector(id).slide() == expected.hud.selector(id).slide());
            CHECK(corrected.hud.selector(id).usageSlot(player.actor.save().progress().inventory) ==
                  expected.hud.selector(id).usageSlot(reference.actor.save().progress().inventory));
            CHECK(corrected.hud.pickups().counts(id)[0].secondsLeft ==
                  expected.hud.pickups().counts(id)[0].secondsLeft);
            CHECK(corrected.hud.pickups().counts(id)[0].count ==
                  expected.hud.pickups().counts(id)[0].count);
        }
        at += delay;
    }
    CHECK(posted >= 3);
    PartyHud::LocalSnapshot stale;
    corrected.hud.saveLocal(stale);
    f32 otherTotal = 123;
    REQUIRE_FALSE(corrected.hud.bindHourglass(corrected.device, corrected.archive, &otherTotal));
    CHECK_FALSE(corrected.hud.restoreLocal(stale));
    CHECK(otherTotal == 123);
}
} // namespace
