#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "FakeMechanic.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/online/NativePlaySetup.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A build offering one mechanic as a room's rule, and one it keeps to a machine. */
const MechanicCatalog& offered() {
    static const MechanicCatalog kCatalog = [] {
        MechanicCatalog mechanics;
        REQUIRE(mechanics.add(test::FakeMechanic::descriptor("rage")));
        REQUIRE(mechanics.add(test::FakeMechanic::descriptor("local", false)));
        return mechanics;
    }();
    return kCatalog;
}

NativePlaySetup::Contract contract() {
    return {
        12,        true, false, true, false, "native-build-1", "native-assets-1", "resolved-data-1",
        &offered()};
}
CharacterSave source() {
    CharacterSave save;
    save.name = "PLAYER";
    save.character = 3;
    save.color = 2;
    save.gold = 42;
    save.levelTotal = 7;
    save.autoAttack = false;
    save.classUnlock = 0x1ff;
    save.moviesSeen = {"private-history"};
    save.classes[0].experience = 1234;
    save.progress().experience = 5678;
    save.progress().health = 400;
    save.progress().inventory.keys = 2;
    save.progress().inventory.potions = {1, 4};
    save.helpSeen = {150, 2, 7, 2};
    return save;
}
MatchParty party() {
    MatchParty result;
    result[1] = CharacterProfile::capture(source());
    auto other = source();
    other.name = "OTHER";
    other.character = 0;
    other.color = 1;
    result[3] = CharacterProfile::capture(other);
    return result;
}

TEST_CASE("native setup owns canonical sparse party and no local save authority",
          "[native-play-setup][determinism]") {
    auto profiles = party();
    REQUIRE(profiles[1]);
    REQUIRE(profiles[3]);
    auto setup = NativePlaySetup::create({}, {2, 2, 1}, profiles, {}, contract());
    REQUIRE(setup);
    REQUIRE(setup->party().size() == 2);
    CHECK(setup->party()[0].player == 1);
    CHECK(setup->party()[1].player == 3);
    for (const auto& member : setup->party()) {
        CAPTURE(member.player);
        CHECK_FALSE(member.slot);
        CHECK_FALSE(member.fallen);
        CHECK(member.turbo == 0.0f);
        CHECK(member.helpHeard.empty());
        CHECK_FALSE(member.resultsCheckpoint);
        CHECK(member.save.classUnlock == 0);
        CHECK(member.save.moviesSeen.empty());
        const auto index = static_cast<usize>(member.player);
        REQUIRE(profiles[index]);
        CHECK(member.save.toJson() == profiles[index]->gameplayCopy().toJson());
    }
    CHECK(setup->party()[0].save.classes[0].experience == 0);
    CHECK(setup->party()[0].save.progress().experience == 5678);
    CHECK(setup->party()[0].save.helpSeen == std::vector<s32>{2, 7, 150});
    const auto before = setup->party()[0].save.toJson();
    profiles[1]->progress.experience = 99;
    profiles[1]->helpSeen.clear();
    CHECK(setup->party()[0].save.toJson() == before);
    CHECK(source().classUnlock == 0x1ff);
}

TEST_CASE("native setup freezes agreed gameplay but preserves local output preferences",
          "[native-play-setup][determinism]") {
    GameConfig first;
    first.timing.tickRate = 30;
    first.difficulty.level = "easy";
    first.multiplayer.mode = MultiplayerMode::Normal;
    first.combat.autoMelee = false;
    first.combat.autoActivateItems = true;
    first.game.expandedShop = false;
    first.combat.autoAim = true;
    first.game.mechanics["rage"] = true;
    GameConfig second = first;
    second.timing.tickRate = 240;
    second.timing.gameplayFrameRate = 144;
    second.display.windowWidth = 1920;
    second.display.vsync = false;
    second.camera.horizontalFovDegrees = 100.0f;
    second.audio.masterVolume = 0.0f;
    second.audio.stereo = false;
    second.combat.enemyHealthBars = false;
    second.combat.autoMelee = true;
    second.combat.autoActivateItems = false;
    second.game.expandedShop = true;
    second.combat.autoAim = false;
    second.game.mechanics["local"] = true;
    second.game.autoSave = true;
    second.controls[0].device = "local-device";
    second.save.directory = "private-save-directory";
    second.play.magicHoldSeconds = 0.5f;
    const auto a = NativePlaySetup::create(first, {4, 2, 2}, party(), {}, contract());
    const auto b = NativePlaySetup::create(second, {4, 2, 2}, party(), {}, contract());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->digest() == b->digest());
    CHECK(a->digest().size() == 64);
    CHECK(a->config().timing.tickRate == 60);
    CHECK(b->config().timing.tickRate == 60);
    CHECK(a->config().difficulty.level == "hard");
    CHECK(a->config().multiplayer.mode == MultiplayerMode::Hurt);
    CHECK(a->config().combat.autoMelee);
    CHECK_FALSE(a->config().combat.autoActivateItems);
    CHECK(a->config().game.expandedShop);
    CHECK(a->config().combat.autoAim == contract().autoAim);
    // The room's rules switch the mechanics, not either machine: one kept to a machine is off.
    for (const auto* setup : {a.get(), b.get()}) {
        CHECK_FALSE(MechanicCatalog::enabled(setup->config().game, "rage"));
        CHECK_FALSE(MechanicCatalog::enabled(setup->config().game, "local"));
    }
    CHECK(b->config().combat.autoAim == contract().autoAim);
    const auto raging = NativePlaySetup::create(first, {4, 2, 2, 0, 1}, party(), {}, contract());
    REQUIRE(raging);
    CHECK(MechanicCatalog::enabled(raging->config().game, "rage"));
    CHECK(raging->digest() != a->digest());
    // A rule the build does not offer is no room's to set.
    CHECK_FALSE(NativePlaySetup::create(first, {4, 2, 2, 0, 0b10}, party(), {}, contract()));
    CHECK(b->config().timing.gameplayFrameRate == 144);
    CHECK(b->config().display.windowWidth == 1920);
    CHECK_FALSE(b->config().display.vsync);
    CHECK(b->config().camera.horizontalFovDegrees == 100.0f);
    CHECK(b->config().audio.masterVolume == 0.0f);
    CHECK_FALSE(b->config().audio.stereo);
    CHECK_FALSE(b->config().combat.enemyHealthBars);
    CHECK(b->config().game.autoSave);
    CHECK(b->config().save.directory == "private-save-directory");
    CHECK(b->config().controls[0].device == "local-device");
    CHECK(b->config().play.magicHoldSeconds == 0.5f);
    second.display.windowWidth = 640;
    second.combat.autoMelee = false;
    CHECK(b->config().display.windowWidth == 1920);
    CHECK(b->config().combat.autoMelee);
}

TEST_CASE("native setup rejects protocol-invalid scene IDs and admits unplayed classes",
          "[native-play-setup][determinism]") {
    auto agreed = contract();
    for (const u32 id : {0U, 65536U, std::numeric_limits<u32>::max()}) {
        agreed.scene = id;
        CHECK_FALSE(NativePlaySetup::create({}, {}, party(), {}, agreed));
    }
    agreed = contract();
    auto profiles = party();
    profiles[1]->progress.health = 0;
    REQUIRE(profiles[1]->valid());
    auto setup = NativePlaySetup::create({}, {}, profiles, {}, agreed);
    REQUIRE(setup);
    REQUIRE(setup->party().size() == 2);
    CHECK_FALSE(setup->party()[0].fallen);
    CHECK(setup->party()[0].save.progress().health == 0);
    CHECK(setup->party()[0].save.health() == kStartingHealth);
    CHECK_FALSE(setup->party()[1].fallen);
}

TEST_CASE("native setup digest distinguishes every agreed contract field",
          "[native-play-setup][determinism]") {
    GameConfig config;
    RoomSettings room;
    auto agreed = contract();
    const auto original = NativePlaySetup::create(config, room, party(), {}, agreed);
    REQUIRE(original);
    SECTION("scene") {
        ++agreed.scene;
    }
    SECTION("build") {
        agreed.build += "-changed";
    }
    SECTION("native assets") {
        agreed.content += "-changed";
    }
    SECTION("resolved data") {
        agreed.data += "-changed";
    }
    SECTION("string boundaries") {
        agreed.build += agreed.content.substr(0, 1);
        agreed.content.erase(0, 1);
    }
    SECTION("automatic melee") {
        agreed.autoMelee = !agreed.autoMelee;
    }
    SECTION("automatic item activation") {
        agreed.autoActivateItems = !agreed.autoActivateItems;
    }
    SECTION("expanded shop") {
        agreed.expandedShop = !agreed.expandedShop;
    }
    SECTION("auto aim") {
        agreed.autoAim = !agreed.autoAim;
    }
    SECTION("room capacity") {
        room.maxPlayers = 3;
    }
    SECTION("difficulty") {
        room.difficulty = 2;
    }
    SECTION("friendly fire") {
        room.friendlyFire = 2;
    }
    SECTION("camera rule") {
        room.camera = 1;
    }
    SECTION("a mechanic's rule") {
        room.mechanics = 1;
    }
    SECTION("text language") {
        config.text.language = "fr";
    }
    const auto changed = NativePlaySetup::create(config, room, party(), {}, agreed);
    REQUIRE(changed);
    CHECK(changed->digest() != original->digest());
}

TEST_CASE("native setup digest includes optional presence authored order and exact float bits",
          "[native-play-setup][determinism]") {
    PlayOptions options;
    options.position = Vec3{1.0f, 2.0f, 3.0f};
    options.yaw = 0.0f;
    options.welcome = false;
    options.items = {{"GOLD", {1.0f, 0.0f, 2.0f}}, {"KEY", {3.0f, 0.0f, 4.0f}}};
    options.arrivalWorld = 3;
    options.arriving = true;
    const auto original = NativePlaySetup::create({}, {}, party(), options, contract());
    REQUIRE(original);
    SECTION("absent position") {
        options.position.reset();
    }
    SECTION("position x") {
        options.position->x += 1.0f;
    }
    SECTION("position y") {
        options.position->y += 1.0f;
    }
    SECTION("position z") {
        options.position->z += 1.0f;
    }
    SECTION("absent yaw") {
        options.yaw.reset();
    }
    SECTION("signed zero yaw") {
        options.yaw = -0.0f;
    }
    SECTION("absent welcome is not false") {
        options.welcome.reset();
    }
    SECTION("welcome") {
        options.welcome = true;
    }
    SECTION("item name") {
        options.items[0].name = "OTHER";
    }
    SECTION("item position") {
        options.items[0].position.y = 1.0f;
    }
    SECTION("item order") {
        std::swap(options.items[0], options.items[1]);
    }
    SECTION("duplicate item") {
        options.items.push_back(options.items.front());
    }
    SECTION("arrival realm") {
        ++options.arrivalWorld;
    }
    SECTION("arrival flag") {
        options.arriving = false;
    }
    const auto changed = NativePlaySetup::create({}, {}, party(), options, contract());
    REQUIRE(changed);
    CHECK(changed->digest() != original->digest());
    CHECK(original->options().items[0].name == "GOLD");
    CHECK(original->options().items.size() == 2);
    CHECK(original->scene() == contract().scene);
}

TEST_CASE("native setup digest binds complete profile bytes and their room seats",
          "[native-play-setup][determinism]") {
    auto profiles = party();
    REQUIRE(profiles[1]);
    const auto original = NativePlaySetup::create({}, {}, profiles, {}, contract());
    REQUIRE(original);
    SECTION("wallet") {
        ++profiles[1]->gold;
    }
    SECTION("experience") {
        ++profiles[1]->progress.experience;
    }
    SECTION("health") {
        ++profiles[1]->progress.health;
    }
    SECTION("help") {
        profiles[1]->helpSeen = {1};
    }
    SECTION("native auto attack") {
        profiles[1]->autoAttack = !profiles[1]->autoAttack;
    }
    SECTION("inventory") {
        profiles[1]->progress.inventory.potions.push_back(2);
    }
    SECTION("lifetime fraction") {
        profiles[1]->progress.lifetime.playSeconds = 0.5;
    }
    SECTION("seat ownership") {
        std::swap(profiles[1], profiles[3]);
    }
    SECTION("sparse placement") {
        std::swap(profiles[0], profiles[1]);
    }
    SECTION("member absent") {
        profiles[3].reset();
    }
    const auto changed = NativePlaySetup::create({}, {}, profiles, {}, contract());
    REQUIRE(changed);
    CHECK(changed->digest() != original->digest());
}

TEST_CASE("native setup refuses invalid room roster profile and option values",
          "[native-play-setup][determinism]") {
    GameConfig config;
    RoomSettings room;
    auto profiles = party();
    auto agreed = contract();
    PlayOptions options;
    REQUIRE(profiles[1]);
    SECTION("zero room capacity") {
        room.maxPlayers = 0;
    }
    SECTION("large room capacity") {
        room.maxPlayers = 5;
    }
    SECTION("unknown difficulty") {
        room.difficulty = 3;
    }
    SECTION("unknown friendly fire") {
        room.friendlyFire = 3;
    }
    SECTION("unknown camera rule") {
        room.camera = 2;
    }
    SECTION("empty party") {
        profiles = {};
    }
    SECTION("room too small") {
        room.maxPlayers = 1;
    }
    SECTION("invalid class") {
        profiles[1]->character = kClassCount;
    }
    SECTION("invalid profile float") {
        profiles[1]->progress.speedAdd = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("noncanonical help order") {
        profiles[1]->helpSeen = {7, 2};
    }
    SECTION("nonfinite position") {
        options.position = Vec3{0.0f, std::numeric_limits<f32>::infinity(), 0.0f};
    }
    SECTION("nonfinite yaw") {
        options.yaw = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("nonfinite item") {
        options.items = {{"GOLD", {std::numeric_limits<f32>::infinity(), 0.0f, 0.0f}}};
    }
    SECTION("empty item name") {
        options.items = {{"", Vec3{0.0f}}};
    }
    SECTION("embedded item terminator") {
        options.items = {{std::string{"GOLD\0KEY", 8}, Vec3{0.0f}}};
    }
    SECTION("empty build") {
        agreed.build.clear();
    }
    SECTION("empty native content") {
        agreed.content.clear();
    }
    SECTION("empty data") {
        agreed.data.clear();
    }
    SECTION("control character identifier") {
        agreed.content += '\n';
    }
    SECTION("empty language") {
        config.text.language.clear();
    }
    CHECK_FALSE(NativePlaySetup::create(config, room, profiles, options, agreed));
}

TEST_CASE("native setup bounds every variable setup field without truncation",
          "[native-play-setup][determinism]") {
    GameConfig config;
    config.text.language.assign(NativePlaySetup::kMaxLanguageBytes, 'l');
    auto agreed = contract();
    agreed.build.assign(NativePlaySetup::kMaxIdentifierBytes, 'b');
    agreed.content.assign(NativePlaySetup::kMaxIdentifierBytes, 'c');
    agreed.data.assign(NativePlaySetup::kMaxIdentifierBytes, 'd');
    PlayOptions options;
    options.items.assign(NativePlaySetup::kMaxDroppedItems,
                         {std::string(NativePlaySetup::kMaxItemNameBytes, 'i'), Vec3{0.0f}});
    REQUIRE(NativePlaySetup::create(config, {}, party(), options, agreed));
    SECTION("build limit") {
        agreed.build += 'b';
    }
    SECTION("content limit") {
        agreed.content += 'c';
    }
    SECTION("data limit") {
        agreed.data += 'd';
    }
    SECTION("language limit") {
        config.text.language += 'l';
    }
    SECTION("item count") {
        options.items.push_back(options.items.front());
    }
    SECTION("item name limit") {
        options.items.front().name += 'i';
    }
    CHECK_FALSE(NativePlaySetup::create(config, {}, party(), options, agreed));
}

TEST_CASE("native setup owns a stable independent zero campaign timer",
          "[native-play-setup][determinism]") {
    static_assert(!std::is_copy_constructible_v<NativePlaySetup>);
    static_assert(!std::is_move_constructible_v<NativePlaySetup>);
    auto first = NativePlaySetup::create({}, {}, party(), {}, contract());
    const auto second = NativePlaySetup::create({}, {}, party(), {}, contract());
    REQUIRE(first);
    REQUIRE(second);
    auto* timer = &first->stopTimeTotal();
    const auto* config = &first->config();
    CHECK(*timer == 0.0f);
    CHECK(second->stopTimeTotal() == 0.0f);
    CHECK(timer != &second->stopTimeTotal());
    const auto initialDigest = first->digest();
    *timer = 9.0f;
    auto retained = std::move(first);
    CHECK(&retained->stopTimeTotal() == timer);
    CHECK(&retained->config() == config);
    CHECK(retained->stopTimeTotal() == 9.0f);
    CHECK(second->stopTimeTotal() == 0.0f);
    // It is a startup contract, not a claimed live-state agreement or handshake.
    CHECK(retained->digest() == initialDigest);
    CHECK(retained->digest() == second->digest());
}
} // namespace
