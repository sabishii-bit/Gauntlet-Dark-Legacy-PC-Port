#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/hud/LevelMessages.h"
#include "game/hud/PartyHud.h"
#include "game/level/NativeStateResources.h"
#include "game/menu/ScrollBox.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class Owner>
NativeStateDigest::Digest liveDigest(const Owner& owner, const NativeStateResources& resources) {
    NativeStateDigest digest{resources.resources()};
    REQUIRE(owner.appendNativeDigest(digest));
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

template <class Owner>
NativeStateDigest::Digest savedDigest(const Owner& owner,
                                      const typename Owner::LocalSnapshot& saved,
                                      const NativeStateResources& resources) {
    NativeStateDigest digest{resources.resources()};
    REQUIRE(owner.appendLocalDigest(digest, saved));
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

template <class Value> NativeStateDigest::Digest valueDigest(const Value& value) {
    NativeStateDigest digest;
    digest.value(value);
    const auto result = digest.checkedFinish();
    REQUIRE(result.has_value());
    return *result;
}

struct ScrollFixture {
    test::FakeRenderDevice device;
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    const test::FakeTexture sheet{32, 16};
    const test::FakeTexture backdrop{64, 64};
    const test::FakeTexture ring{2, 2};
    const Image source = Image::filled(4, 4, Color::rgba(200, 180, 120, 255));
    const Image mask = Image::filled(2, 2, Color::white());
    const Image burnt = Image::filled(2, 2, Color::white().withAlpha(0));
    TextPainter text;
    ScrollBox box;
    NativeStateResources resources;

    ScrollFixture() {
        text.setFont(&font, &sheet);
        box.setText(&text);
        box.setArt({&backdrop, &source, nullptr, nullptr, {&mask, &burnt}, {&ring}});
        REQUIRE(resources.registerBuiltinTexture(backdrop, "test/hud/backdrop"));
        REQUIRE(resources.registerBuiltinTexture(ring, "test/hud/ring"));
        REQUIRE(resources.registerImage(source, "test/hud/source"));
        REQUIRE(resources.registerImage(mask, "test/hud/mask/0"));
        REQUIRE(resources.registerImage(burnt, "test/hud/mask/1"));
        REQUIRE(box.open(device, {"AB", "B"}, 1, "AB"));
    }

    void draw() {
        device.draws.clear();
        box.prepare(device);
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        box.draw(canvas);
        canvas.end();
    }
};

TEST_CASE("native scroll digests cover page holds and burns across eight-frame corrections",
          "[scene-hud-native-digest][native-state-digest]") {
    ScrollFixture reference;
    ScrollFixture corrected;
    CHECK(liveDigest(reference.box, reference.resources) ==
          liveDigest(corrected.box, corrected.resources));
    s32 tick = 0;
    for (s32 window = 0; window < 24; ++window) {
        const s32 count = 1 + window % 8;
        CAPTURE(tick, count);
        ScrollBox::LocalSnapshot before;
        corrected.box.saveLocal(before);
        const auto initial = liveDigest(corrected.box, corrected.resources);
        CHECK(savedDigest(corrected.box, before, corrected.resources) == initial);
        for (s32 n = 0; n < count; ++n) {
            reference.box.step(1, 1);
            corrected.box.step(2, 0);
        }
        // Hashing a saved state cannot consult a later page, burn or upload cache.
        CHECK(savedDigest(corrected.box, before, corrected.resources) == initial);
        const auto creations = corrected.device.texturesCreated;
        const auto uploads = corrected.device.textureUpdates;
        REQUIRE(corrected.box.restoreLocal(before));
        CHECK(corrected.device.texturesCreated == creations);
        CHECK(corrected.device.textureUpdates == uploads);
        CHECK(liveDigest(corrected.box, corrected.resources) == initial);
        for (s32 n = 0; n < count; ++n) {
            corrected.box.step(1, 1);
            if ((tick + n) % 2 == 0) {
                const auto prior = liveDigest(corrected.box, corrected.resources);
                corrected.draw();
                CHECK(liveDigest(corrected.box, corrected.resources) == prior);
            }
        }
        CHECK(liveDigest(reference.box, reference.resources) ==
              liveDigest(corrected.box, corrected.resources));
        tick += count;
    }
    CHECK_FALSE(corrected.box.active());
    ScrollBox::LocalSnapshot saved;
    corrected.box.saveLocal(saved);
    NativeStateDigest foreign{reference.resources.resources()};
    CHECK_FALSE(reference.box.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    corrected.box.setText(&corrected.text);
    NativeStateDigest stale{corrected.resources.resources()};
    CHECK_FALSE(corrected.box.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("native burn fingerprints retain copied pixels and reject unidentified mask art",
          "[scene-hud-native-digest][native-state-digest]") {
    ScrollFixture fixture;
    BurnDialogueScroll first;
    BurnDialogueScroll second;
    Image changed = fixture.source;
    changed.setPixel(1, 2, Color::rgba(255, 12, 34, 255));
    REQUIRE(first.start(fixture.device, {1, 2, 3, 4}, fixture.source, {&fixture.mask},
                        {&fixture.ring}));
    REQUIRE(second.start(fixture.device, {1, 2, 3, 4}, changed, {&fixture.mask}, {&fixture.ring}));
    CHECK(liveDigest(first, fixture.resources) != liveDigest(second, fixture.resources));
    BurnDialogueScroll::LocalSnapshot saved;
    second.saveLocal(saved);
    const auto expected = savedDigest(second, saved, fixture.resources);
    changed.setPixel(1, 2, Color::white()); // start copied the source, not this caller's Image.
    CHECK(liveDigest(second, fixture.resources) == expected);
    second.step(7);
    CHECK(liveDigest(second, fixture.resources) != expected);
    CHECK(savedDigest(second, saved, fixture.resources) == expected);
    REQUIRE(second.restoreLocal(saved));
    CHECK(liveDigest(second, fixture.resources) == expected);
    NativeStateDigest absent;
    CHECK_FALSE(second.appendNativeDigest(absent));
    CHECK_FALSE(absent.checkedFinish());
}

TEST_CASE("native HUD values include help lifetime and nonvisible acquisition history",
          "[scene-hud-native-digest][native-state-digest]") {
    const auto directory = test::scratchDirectory("scene-hud-digest-help");
    writeTextFile(directory / "messages.json",
                  R"({"messages":[{"name":"LEVELUP","lines":["LEVEL %d"]}]})");
    MessageTable firstText;
    MessageTable secondText;
    REQUIRE(firstText.load(directory / "messages.json"));
    REQUIRE(secondText.load(directory / "messages.json"));
    PartyHud first;
    PartyHud second;
    first.help().setTexts(&firstText);
    second.help().setTexts(&secondText);
    REQUIRE(first.help().post(HelpMessages::kLevelUp, 0, {}, 2));
    REQUIRE(second.help().post(HelpMessages::kLevelUp, 0, {}, 2));
    const NativeStateResources resources;
    CHECK(liveDigest(first, resources) == liveDigest(second, resources));
    PartyHud::LocalSnapshot saved;
    second.saveLocal(saved);
    const auto original = liveDigest(second, resources);
    second.help().update(1);
    REQUIRE(second.help().lines() == first.help().lines());
    CHECK(liveDigest(first, resources) != liveDigest(second, resources));
    CHECK(savedDigest(second, saved, resources) == original);
    REQUIRE(second.restoreLocal(saved));
    for (s32 delay = 1; delay <= 8; ++delay) {
        second.saveLocal(saved);
        first.help().update(delay);
        first.pickups().step(delay, static_cast<f32>(delay) / 60);
        second.help().update(delay + 11);
        second.pickups().addCard(2, "CRYSTAL");
        second.showRelics();
        REQUIRE(second.restoreLocal(saved));
        second.help().update(delay);
        second.pickups().step(delay, static_cast<f32>(delay) / 60);
        CHECK(liveDigest(first, resources) == liveDigest(second, resources));
    }
    second.saveLocal(saved);
    second.clear();
    NativeStateDigest stale;
    CHECK_FALSE(second.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());

    Inventory inventory;
    inventory.addPowerup(powerup::kWeapon, powerup::kThunderHammer, 7, 20);
    inventory.addPowerup(powerup::kWeapon, powerup::kSuperShot, 9, 20);
    PowerupSelector direct;
    PowerupSelector history;
    direct.focus(inventory, powerup::kWeapon, powerup::kSuperShot);
    history.focus(inventory, powerup::kWeapon, powerup::kThunderHammer);
    history.focus(inventory, powerup::kWeapon, powerup::kSuperShot);
    CHECK(direct.state() == history.state());
    CHECK(direct.selection() == history.selection());
    CHECK(direct.slide() == history.slide());
    CHECK(valueDigest(direct) != valueDigest(history));

    PickupHud pickups;
    pickups.showCount(0, "SM_CRYSTAL_ORA", 3, 15);
    const auto unstepped = pickups;
    pickups.step(0, 1.0f / 120);
    CHECK(pickups.counts(0)[0].count == unstepped.counts(0)[0].count);
    CHECK(valueDigest(pickups) != valueDigest(unstepped));
}

TEST_CASE("native hourglass digests include fractional clocks and independently bound sand art",
          "[scene-hud-native-digest][native-state-digest][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/textures.ngc").parent_path();
    std::array<test::FakeRenderDevice, 2> devices;
    std::array<ItemArchive, 2> archives;
    std::array<ChallengeHud, 2> hud;
    std::array<NativeStateResources, 2> resources;
    for (usize i = 0; i < 2; ++i) {
        REQUIRE(archives[i].load(directory));
        REQUIRE(hud[i].bind(devices[i], archives[i]));
        REQUIRE(resources[i].registerArchive(archives[i], "POWERUPS"));
    }
    CHECK(liveDigest(hud[0], resources[0]) == liveDigest(hud[1], resources[1]));
    ChallengeHud::LocalSnapshot initial;
    hud[1].saveLocal(initial);
    hud[1].step(1.0f / 240);
    CHECK(hud[0].look(30, 60, true).fallingFrame == hud[1].look(30, 60, true).fallingFrame);
    CHECK(liveDigest(hud[0], resources[0]) != liveDigest(hud[1], resources[1]));
    REQUIRE(hud[1].restoreLocal(initial));
    for (s32 count = 1; count <= 8; ++count) {
        ChallengeHud::LocalSnapshot saved;
        hud[1].saveLocal(saved);
        const auto before = liveDigest(hud[1], resources[1]);
        for (s32 n = 0; n < count; ++n) {
            hud[0].step(1.0f / 120);
            hud[1].step(0.21f);
        }
        CHECK(savedDigest(hud[1], saved, resources[1]) == before);
        REQUIRE(hud[1].restoreLocal(saved));
        for (s32 n = 0; n < count; ++n) {
            hud[1].step(1.0f / 120);
        }
        CHECK(liveDigest(hud[0], resources[0]) == liveDigest(hud[1], resources[1]));
        const auto priorDraw = liveDigest(hud[1], resources[1]);
        Canvas canvas;
        canvas.begin(devices[1], Mat4{1});
        hud[1].draw(canvas, 30, 60, true);
        canvas.end();
        CHECK(liveDigest(hud[1], resources[1]) == priorDraw);
    }
    hud[1].saveLocal(initial);
    NativeStateDigest foreign{resources[0].resources()};
    CHECK_FALSE(hud[0].appendLocalDigest(foreign, initial));
    hud[1].clear();
    NativeStateDigest stale{resources[1].resources()};
    CHECK_FALSE(hud[1].appendLocalDigest(stale, initial));
}

std::filesystem::path messageRoot() {
    const auto root = test::scratchDirectory("scene-hud-digest-messages");
    std::filesystem::create_directories(root / "STATIC");
    std::filesystem::create_directories(root / "fonts");
    std::filesystem::create_directories(root / "text");
    writeFile(root / "STATIC/tile.png", test::kTinyPng);
    constexpr std::array kNames{"FONT32",           "SCROLL_A",         "FONT32_GLOW",
                                "GREENCIRCTRANS",   "GREENCIRCTRANS+1", "GREENCIRCTRANSM",
                                "GREENCIRCTRANSM+1"};
    std::string manifest = R"({"bitmaps":[)";
    for (usize i = 0; i < kNames.size(); ++i) {
        if (i != 0) {
            manifest += ',';
        }
        manifest += std::format(
            R"({{"index":{},"name":"{}","file":"tile.png","width":2,"height":2}})", i, kNames[i]);
    }
    writeTextFile(root / "STATIC/textures.json", manifest + "]}");
    writeTextFile(root / "fonts/font32.json", R"({"height":10,"glyphs":[
        {"code":65,"width":6,"x":0,"y":0},{"code":66,"width":8,"x":0,"y":0}]})");
    writeTextFile(root / "text/scroll_e.json", R"({"fonts":["font32"],"messages":[
        {"name":"WELCOME","font":0,"scale":1,"lines":["A","B"]}],"lists":[]})");
    return root;
}

TEST_CASE("native level message digests retain selected message page and immutable text content",
          "[scene-hud-native-digest][native-state-digest]") {
    const auto root = messageRoot();
    std::array<test::FakeRenderDevice, 2> devices;
    std::array<TextureSet, 2> textures;
    std::array<LevelMessages, 2> messages;
    std::array<NativeStateResources, 2> resources;
    for (usize i = 0; i < 2; ++i) {
        messages[i].load(devices[i], textures[i], root, nullptr);
        REQUIRE(messages[i].nativeReady());
        REQUIRE(resources[i].registerTextures(textures[i], "STATIC"));
        REQUIRE(messages[i].open(devices[i], "WELCOME", nullptr, 1));
    }
    CHECK(liveDigest(messages[0], resources[0]) == liveDigest(messages[1], resources[1]));
    for (s32 count = 1; count <= 8; ++count) {
        LevelMessages::LocalSnapshot saved;
        messages[1].saveLocal(saved);
        const auto before = liveDigest(messages[1], resources[1]);
        REQUIRE(messages[1].open(devices[1], "WELCOME", nullptr, 0));
        messages[1].step(count + 2, 0);
        CHECK(savedDigest(messages[1], saved, resources[1]) == before);
        REQUIRE(messages[1].restoreLocal(saved));
        for (s32 n = 0; n < count; ++n) {
            const auto a = messages[0].step(1, 1);
            const auto b = messages[1].step(1, 1);
            CHECK(a.stopVoice == b.stopVoice);
            CHECK(a.burnSound == b.burnSound);
        }
        CHECK(liveDigest(messages[0], resources[0]) == liveDigest(messages[1], resources[1]));
        const auto prior = liveDigest(messages[1], resources[1]);
        messages[1].prepare(devices[1]);
        Canvas canvas;
        canvas.begin(devices[1], Mat4{1});
        messages[1].draw(canvas);
        canvas.end();
        CHECK(liveDigest(messages[1], resources[1]) == prior);
    }
    LevelMessages::LocalSnapshot saved;
    messages[1].saveLocal(saved);
    NativeStateDigest foreign{resources[0].resources()};
    CHECK_FALSE(messages[0].appendLocalDigest(foreign, saved));
    messages[1].clear();
    NativeStateDigest stale{resources[1].resources()};
    CHECK_FALSE(messages[1].appendLocalDigest(stale, saved));
}
} // namespace
