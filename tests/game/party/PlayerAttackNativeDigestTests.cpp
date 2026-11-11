#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/audio/SceneSoundEvents.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/level/NativeStateResources.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/AttackSceneFixture.h"
#include "game/party/MoveStrikes.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerAttacksTestPeer.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/ProjectileFixture.h"
#include "game/party/ScriptedAttacks.h"
#include "game/players/PlayerDeeds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class T> auto nativeHash(const T& owner, NativeStateDigest::Resources resources = {}) {
    NativeStateDigest digest(resources);
    REQUIRE(owner.appendNativeDigest(digest));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

template <class T>
auto savedHash(const T& owner, const typename T::LocalSnapshot& saved,
               NativeStateDigest::Resources resources = {}) {
    NativeStateDigest digest(resources);
    REQUIRE(owner.appendLocalDigest(digest, saved));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

TEST_CASE("native move strike digest preserves hidden launch delay and next identity",
          "[native-digest][player-attack-native-digest]") {
    MoveStrikes a;
    MoveStrikes b;
    CHECK(nativeHash(a) == nativeHash(b));
    MoveStrike row;
    row.type = 2;
    row.maxTime = 2;
    row.amount = 20;
    row.speed = 5;
    const auto id = a.start(row, 0, {}, {0, 0, 1}, 10);
    REQUIRE(id != 0);
    b.start(row, 0, {}, {0, 0, 1}, 10);
    CHECK(nativeHash(a) == nativeHash(b));
    MoveStrikes::LocalSnapshot base;
    a.saveLocal(base);
    const auto original = nativeHash(a);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<MoveStrikes::Strike&>(a.strike(0)).launchIn = 0.125f;
    CHECK(nativeHash(a) != original);
    CHECK(savedHash(a, base) == original);
    REQUIRE(a.restoreLocal(base));
    CHECK(nativeHash(a) == original);
    a.stop(id);
    const MoveStrikes empty;
    REQUIRE(a.count() == empty.count());
    CHECK(nativeHash(a) != nativeHash(empty)); // next identity still affects future births
    NativeStateDigest foreign;
    CHECK_FALSE(b.appendLocalDigest(foreign, base));
    CHECK_FALSE(foreign.checkedFinish());
    a.clear();
    NativeStateDigest stale;
    CHECK_FALSE(a.appendLocalDigest(stale, base));
}

TEST_CASE("native player projectile hashes include contact history pending impacts and specs",
          "[native-digest][player-attack-native-digest]") {
    PlayerMissiles missiles;
    PlayerMissiles other;
    const MissileSpec spec{"TEST", "0", 1, 0, 1, false, "SPARKS"};
    MissileSpec copy = spec;
    MissileLaunch launch;
    launch.position = {0, 20, 0};
    launch.spec = &spec;
    REQUIRE(missiles.launch(launch));
    launch.spec = &copy;
    REQUIRE(other.launch(launch));
    const auto initial = nativeHash(missiles);
    CHECK(nativeHash(other) == initial); // specification addresses are not state
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& shot = const_cast<PlayerMissiles::Missile&>(missiles.missile(0));
    const auto original = shot;
    const auto changes = [&](auto change) {
        shot = original;
        change(shot);
        CHECK(nativeHash(missiles) != initial);
    };
    changes([](auto& value) { value.pierced.push_back(73); });
    changes([](auto& value) { value.playerContacts.push_back({2, 0.125f}); });
    changes([](auto& value) { value.release = MissileRelease{7, 12, true, 2, 9}; });
    changes([](auto& value) { value.potionRelease = MissilePotionRelease{21}; });
    changes([](auto& value) { value.previousPosition.x += 1; });
    changes([](auto& value) { value.breaksPotions = false; });
    changes([](auto& value) { value.playerHitGap += 0.001f; });
    shot = original;
    const TreeModel unknown;
    shot.model = &unknown;
    NativeStateDigest unresolved;
    CHECK_FALSE(missiles.appendNativeDigest(unresolved));
    CHECK_FALSE(unresolved.checkedFinish());
    shot = original;
    copy.weight += 1;
    CHECK(nativeHash(other) != initial);

    PlayerMissiles::LocalSnapshot base;
    missiles.saveLocal(base);
    const std::array target{
        MissileTarget{.id = 17, .base = launch.position, .radius = 10, .height = 10}};
    missiles.update(1.0f / 60, nullptr, target);
    REQUIRE(missiles.count() == 0);
    const auto pending = nativeHash(missiles);
    REQUIRE_FALSE(missiles.takeImpacts().empty());
    CHECK(nativeHash(missiles) != pending);
    CHECK(savedHash(missiles, base) == initial);
    REQUIRE(missiles.restoreLocal(base));
    CHECK(nativeHash(missiles) == initial);
}

TEST_CASE("native player projectile values replay all eight correction distances",
          "[native-digest][player-attack-native-digest][local-rewind]") {
    PlayerMissiles reference;
    PlayerMissiles corrected;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 12, 0};
    launch.velocity = Vec3{2, 1, 7};
    REQUIRE(reference.launch(launch));
    REQUIRE(corrected.launch(launch));
    PlayerMissiles::LocalSnapshot base;
    corrected.saveLocal(base);
    const auto initial = nativeHash(corrected);
    std::array<NativeStateDigest::Digest, 8> expected{};
    for (auto& frame : expected) {
        reference.update(1.0f / 60, nullptr);
        frame = nativeHash(reference);
    }
    for (usize distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(corrected.restoreLocal(base));
        for (usize tick = 0; tick < distance; ++tick) {
            corrected.update(1.0f / 30, nullptr); // discarded speculative branch
        }
        CHECK(nativeHash(corrected) != expected[distance - 1]);
        CHECK(savedHash(corrected, base) == initial);
        REQUIRE(corrected.restoreLocal(base));
        for (usize tick = 0; tick < distance; ++tick) {
            corrected.update(1.0f / 60, nullptr);
            CHECK(nativeHash(corrected) == expected[tick]);
        }
    }
    corrected.clear();
    NativeStateDigest stale;
    CHECK_FALSE(corrected.appendLocalDigest(stale, base));
}

struct AttackFixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 1> players;
    NativeStateResources resources;
    test::ScriptedFixtures fixtureHost;
    test::ScriptedAttacks attackHost;
    AttackFixture() {
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
        REQUIRE(resources.registerLayout(world.layout(), "stage/test"));
        REQUIRE(resources.registerCollision(world.collision(), "stage/test"));
    }
};

TEST_CASE("native player attacks preserve shield clocks potion waves and arsenal color cycle",
          "[native-digest][player-attack-native-digest][local-rewind]") {
    auto fixture = std::make_unique<AttackFixture>();
    auto other = std::make_unique<AttackFixture>();
    auto& attacks = fixture->attacks;
    const auto resources = fixture->resources.resources();
    CHECK(nativeHash(attacks, resources) ==
          nativeHash(other->attacks, other->resources.resources()));
    PlayerAttacks::LocalSnapshot empty;
    attacks.saveLocal(empty);
    const auto initial = nativeHash(attacks, resources);
    attacks.shatterPotion(1, {1, 2, 3});
    CHECK(nativeHash(attacks, resources) != initial);
    CHECK(savedHash(attacks, empty, resources) == initial);
    REQUIRE(attacks.restoreLocal(empty));
    fixture->players[0].actor.save().progress().inventory.potions.push_back(1);
    attacks.shieldPotion(0, fixture->players);
    REQUIRE(attacks.shieldCount() == 1);
    PlayerAttacks::LocalSnapshot shield;
    attacks.saveLocal(shield);
    const auto shieldHash = nativeHash(attacks, resources);
    attacks.updateShields(
        0.001f, fixture->players,
        {fixture->opponents, fixture->fixtures, fixture->fixtureHost, fixture->attackHost});
    REQUIRE(attacks.shieldCount() == 1);
    CHECK(nativeHash(attacks, resources) != shieldHash);
    CHECK(savedHash(attacks, shield, resources) == shieldHash);
    REQUIRE(attacks.restoreLocal(shield));
    CHECK(nativeHash(attacks, resources) == shieldHash);
    NativeStateDigest foreign(other->resources.resources());
    CHECK_FALSE(other->attacks.appendLocalDigest(foreign, shield));
    PlayerArsenal::LocalSnapshot cycle;
    fixture->arsenal.saveLocal(cycle);
    const auto beforeCycle = nativeHash(fixture->arsenal, resources);
    fixture->arsenal.resolvePotionKind(0);
    CHECK(nativeHash(fixture->arsenal, resources) != beforeCycle);
    CHECK(savedHash(fixture->arsenal, cycle, resources) == beforeCycle);
    REQUIRE(fixture->arsenal.restoreLocal(cycle));
    CHECK(nativeHash(fixture->arsenal, resources) == beforeCycle);
    attacks.clear();
    NativeStateDigest stale(resources);
    CHECK_FALSE(attacks.appendLocalDigest(stale, shield));
}

TEST_CASE("native player attack digest bytes stay pinned for strikes shots and shields",
          "[native-digest][player-attack-native-digest][native-digest-pin]") {
    MoveStrikes strikes;
    MoveStrike row;
    row.type = 2;
    row.maxTime = 2;
    row.amount = 20;
    row.speed = 5;
    REQUIRE(strikes.start(row, 1, {3, 0, 4}, {0, 0, 1}, 10) != 0);
    PlayerMissiles missiles;
    const MissileSpec spec{"TEST", "0", 1, 0, 1, false, "SPARKS"};
    MissileLaunch launch;
    launch.position = {0, 20, 0};
    launch.velocity = Vec3{2, 1, 7};
    launch.spec = &spec;
    REQUIRE(missiles.launch(launch));
    auto fixture = std::make_unique<AttackFixture>();
    fixture->attacks.shatterPotion(1, {1, 2, 3});
    fixture->players[0].actor.save().progress().inventory.potions.push_back(1);
    fixture->attacks.shieldPotion(0, fixture->players);
    REQUIRE(fixture->attacks.shieldCount() == 1);
    fixture->arsenal.resolvePotionKind(0);
    const auto resources = fixture->resources.resources();
    // SHA-256 of the native digest of each owner above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kStrikes =
        "43c08eabbcd6f0efa0b0a1eeb69018de2775d740debde68db19e564ece83072e";
    constexpr std::string_view kMissiles =
        "9d3563069ae076fbd4ac70d75dfee8cc48614e67d7cc1a06216152bb5f5af87a";
    constexpr std::string_view kAttacks =
        "ceac6b0d4116f25d3b4a8febc073cb450116ca8c979d275b551bf332553bd1ad";
    constexpr std::string_view kArsenal =
        "8b11c88b94a3745acce3c9d57a69c3263bf2510f7ebc89e57675fea84c17360f";
    CHECK(test::hexBytes(nativeHash(strikes)) == kStrikes);
    CHECK(test::hexBytes(nativeHash(missiles)) == kMissiles);
    CHECK(test::hexBytes(nativeHash(fixture->attacks, resources)) == kAttacks);
    CHECK(test::hexBytes(nativeHash(fixture->arsenal, resources)) == kArsenal);
}

TEST_CASE("native move strike digest bytes stay pinned after a burst and a flight and their save",
          "[native-digest][player-attack-native-digest][digest-golden]") {
    MoveStrikes strikes;
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.delay = 0.1f;
    burst.radius = 12;
    burst.amount = 50;
    MoveStrike flight;
    flight.type = MoveStrike::kFlies;
    flight.hitRadius = 1;
    flight.maxTime = 2;
    flight.amount = 20;
    flight.speed = 5;
    REQUIRE(strikes.start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 0.75f) != 0);
    REQUIRE(strikes.start(flight, 1, {3, 0, 4}, {1, 0, 0}, 10) != 0);
    constexpr f32 kFrame = 1.0f / 30;
    for (s32 frame = 1; frame <= 4; ++frame) {
        (void)strikes.update(kFrame, nullptr);
    }
    MoveStrikes::LocalSnapshot saved;
    strikes.saveLocal(saved);
    for (s32 frame = 5; frame <= 12; ++frame) {
        (void)strikes.update(kFrame, nullptr);
    }
    REQUIRE(strikes.count() == 2);
    // SHA-256 of the strikes' native digest twelve frames in and of what they saved four frames
    // in, both under way at each, which netplay peers must hash alike: a literal moves only when
    // what is visited changes on purpose.
    test::checkPinnedDigests("MoveStrikes", test::liveDigest(strikes),
                             test::savedDigest(strikes, saved),
                             "de64fc9b2d1ade962bd82e617476a116f9364edd870c3281a73c0842d9ea022e",
                             "529ca0951eb29b09a86e664f8f6eef2ae2b4d2e4392d748b96a84f24f61d1d6f");
}

TEST_CASE("native player projectile digest bytes stay pinned after two flights and their save",
          "[native-digest][player-attack-native-digest][digest-golden]") {
    test::ProjectileFixture fixture;
    PlayerMissiles missiles;
    missiles.bindVisuals(fixture.device);
    constexpr f32 kTick = 1.0f / 60;
    REQUIRE(missiles.launch(fixture.launch()));
    for (s32 tick = 1; tick <= 3; ++tick) {
        missiles.update(kTick, nullptr);
    }
    // A second throw beside the first, three ticks after it and back the other way.
    auto other = fixture.launch();
    other.position = {4, 8, 0};
    other.velocity = Vec3{0, 2, -15};
    other.release = MissileRelease{0, 73, false};
    REQUIRE(missiles.launch(other));
    for (s32 tick = 4; tick <= 5; ++tick) {
        missiles.update(kTick, nullptr);
    }
    PlayerMissiles::LocalSnapshot saved;
    missiles.saveLocal(saved);
    for (s32 tick = 6; tick <= 12; ++tick) {
        missiles.update(kTick, nullptr);
    }
    REQUIRE(missiles.count() == 2);
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(fixture.archive, "stage/projectiles"));
    REQUIRE(resources.registerBuiltinTexture(fixture.device.whiteTexture(), "opaque-white"));
    // SHA-256 of the pool's native digest twelve ticks in and of what it saved five ticks in,
    // both throws in flight at each, which netplay peers must hash alike: a literal moves only
    // when what is visited changes on purpose.
    test::checkPinnedDigests("PlayerMissiles", test::liveDigest(missiles, resources),
                             test::savedDigest(missiles, saved, resources),
                             "4c0535cc123c6687a078ebfdf0584cf17d76a29c3c6d7da0886555874786fda4",
                             "909481c404a2594cd8314356fa8b3b6fad9126eb46efa5bcaab18bb0180c6b47");
}

/** What the attack scene's owners borrow, under the keys every load of it shares. */
void registerAttackScene(NativeStateResources& resources, const test::AttackSceneFixture& scene) {
    REQUIRE(resources.registerBuiltinTexture(scene.device.whiteTexture(), "opaque-white"));
    REQUIRE(resources.registerArchive(scene.weapons, "weapons"));
    REQUIRE(resources.registerLayout(scene.world.layout(), "stage"));
    REQUIRE(resources.registerCollision(scene.world.collision(), "stage"));
}

TEST_CASE("native arsenal digest bytes stay pinned after its casts and a flight and its save",
          "[native-digest][player-attack-native-digest][digest-golden]") {
    test::AttackSceneFixture fixture;
    auto& actor = fixture.players[0].actor;
    auto& progress = actor.save().progress();
    // From level 75 a caster's potion also shows its healing cast about them.
    progress.experience = levelExperience(80);
    REQUIRE(progress.inventory.addPotions(0, 2) == 2);
    constexpr f32 kFrame = 1.0f / 30;
    const auto step = [&](s32 frame) {
        actor.place({static_cast<f32>(frame) * 0.05f, 0, 0});
        fixture.arsenal.followCaster(actor);
        fixture.arsenal.missiles().update(kFrame, &fixture.world.collision());
        fixture.effects.update(kFrame);
    };
    REQUIRE(fixture.arsenal.usePotion(actor));
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 4, 0};
    launch.velocity = Vec3{0, 3, 12};
    REQUIRE(fixture.arsenal.missiles().launch(launch));
    for (s32 frame = 1; frame <= 4; ++frame) {
        step(frame);
    }
    PlayerArsenal::LocalSnapshot saved;
    PlayerMissiles::LocalSnapshot missiles;
    fixture.arsenal.saveLocal(saved);
    fixture.arsenal.missiles().saveLocal(missiles);
    // A second potion, of the next colour round the cycle, shows a second cast.
    REQUIRE(fixture.arsenal.usePotion(actor));
    for (s32 frame = 5; frame <= 10; ++frame) {
        step(frame);
    }
    REQUIRE(fixture.arsenal.casts().size() == 2);
    REQUIRE(fixture.arsenal.missiles().count() == 1);
    NativeStateResources resources;
    registerAttackScene(resources, fixture);
    // SHA-256 of the arsenal's native digest ten frames in and of what it saved four frames in,
    // with its missiles beside it, which netplay peers must hash alike: a literal moves only when
    // what is visited changes on purpose.
    test::checkPinnedDigests("PlayerArsenal", test::liveDigest(fixture.arsenal, resources),
                             test::savedDigest(fixture.arsenal, saved, resources),
                             "a11461c9d89b2f095cdd179fbd6c0ce181b6c1818471d86cf4f990965f07637c",
                             "b466a7f490321b86e43046c183d8c38c212bab02de79d0852cefa7a63e0d4824");
    test::checkPinnedDigests("PlayerArsenal's PlayerMissiles",
                             test::liveDigest(fixture.arsenal.missiles(), resources),
                             test::savedDigest(fixture.arsenal.missiles(), missiles, resources),
                             "d1d3e52c5bb31b964f2beedfe714eea53115cb134b21331c698c03c6dc09b9f5",
                             "4366f9e46a58103466b3d199ebcb0afd537a136fa2a8ff3f3c55c99eddda8d12");
}

/** The class data of the attacks' script: a lesser turbo whose burst rides the body with a
 * trail of particles beside it, chained to a strike that flies with an effect of its own; the
 * burst and the particles last eight seconds and what flies six. */
constexpr std::string_view kTurboChain = R"({"height":6,"width":2,
    "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
    "moves":{"turboB":0},"moveEffects":[
    {"tree":"BURST","lifetime":8,"next":1},
    {"flags":33554432,"tree":"SKIN","sound":"BODY","offset":[0,2,0],"lifetime":8,"radius":3,
     "alphaMod":150},
    {"tree":"EXPRING"}],"moveStrikes":[
    {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50,"effect":0,"next":1},
    {"type":2,"startFrame":1,"hitRadius":10,"arc":-1,"offset":[0,3,2],"speedMin":1,
     "speedMax":1,"maxTime":6,"amount":70,"effect":2}]})";

TEST_CASE("native player attack digest bytes stay pinned holding every value and their save",
          "[native-digest][player-attack-native-digest][digest-golden]") {
    test::AttackSceneFixture fixture;
    writeTextFile(fixture.root / "pdata/WAR.json", kTurboChain);
    REQUIRE(fixture.classes.load(fixture.root / "pdata"));
    std::filesystem::copy(test::deathArchive() / "MONSTERS/DEATH", fixture.root / "MONSTERS/DEATH",
                          std::filesystem::copy_options::recursive);
    auto& player = fixture.players[0];
    player.actor.spawn(3, {}, fixture.classes.stats(0), {}, 0);
    player.turbo.add(100);
    auto& inventory = player.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kFireBreath, 3, 30);
    inventory.addPowerup(powerup::kArmor, DeathRules::kProtection, 0, 60);
    REQUIRE(inventory.addPotions(2, 1) == 1);
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    auto& enemies = fixture.opponents.enemies();
    EnemyScales scales;
    scales.health = 1000;
    enemies.open(fixture.device, fixture.root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    REQUIRE(enemies.loadKind(kDeathKind));
    // A grunt behind the player, within its shield's ring, and Death before it, touching it.
    REQUIRE(enemies.spawn({.kind = kGruntKind, .position = {0, 0, -1}, .placed = true}, {}));
    REQUIRE(enemies.spawn({.kind = kDeathKind, .position = {0, 0, 2}, .placed = true}, {}));
    constexpr f32 kFrame = 1.0f / 30;
    const auto step = [&](PlayerDeed deed) {
        player.figure->capturePresentation();
        fixture.effects.capturePresentation();
        player.figure->animate(0, 2, kFrame, deed);
        fixture.attacks.updateTurbo(0, 2, kFrame, fixture.players, [](s32, usize) {});
        fixture.attacks.useItemAttack(0, fixture.players);
        fixture.attacks.updateStrikes(kFrame, fixture.players, fixture.targets);
        fixture.attacks.updateProjectiles(kFrame, fixture.players, fixture.targets);
        fixture.attacks.updateShields(kFrame, fixture.players, fixture.targets);
        fixture.effects.update(kFrame);
    };
    // The lesser turbo, played out; then the fire breath, the halo's hold on Death, heard as a
    // recorded scene hears it, the shield, and a potion broken far off.
    step(PlayerDeed::TurboStrong);
    for (s32 frame = 0; frame < 120 && player.figure->animator().turboing(); ++frame) {
        step(PlayerDeed::None);
    }
    REQUIRE_FALSE(player.figure->animator().turboing());
    step(PlayerDeed::Breathe);
    REQUIRE(fixture.attacks.itemAreas().size() == 1);
    REQUIRE(fixture.attacks.deathContact(0, fixture.players, fixture.targets));
    SceneSoundEvents sounds;
    sounds.open(71, nullptr);
    fixture.audio.recordTo(&sounds);
    sounds.beginTick(1);
    REQUIRE(fixture.attacks.grabDeath(0, 2, true, fixture.players, fixture.targets));
    (void)sounds.endFrame();
    fixture.audio.recordTo(nullptr);
    fixture.attacks.shieldPotion(0, fixture.players);
    fixture.attacks.shatterPotion(0, {60, 0, 60});
    step(PlayerDeed::None);
    const auto keepsEveryValue = [&fixture] {
        const auto kept = PlayerAttacksTestPeer::keptValues(fixture.attacks);
        CAPTURE(kept);
        REQUIRE(std::ranges::none_of(kept, [](usize count) { return count == 0; }));
        REQUIRE_FALSE(fixture.attacks.potionShields().front().marks.empty());
        REQUIRE(std::ranges::any_of(
            fixture.attacks.strikeSources(),
            [](const PlayerAttacks::StrikeSource& source) { return !source.contacts.empty(); }));
    };
    keepsEveryValue();
    PlayerAttacks::LocalSnapshot saved;
    MoveStrikes::LocalSnapshot strikes;
    fixture.attacks.saveLocal(saved);
    fixture.attacks.strikes().saveLocal(strikes);
    for (s32 frame = 0; frame < 8; ++frame) {
        step(PlayerDeed::None);
    }
    keepsEveryValue();
    NativeStateResources resources;
    registerAttackScene(resources, fixture);
    // SHA-256 of the attacks' native digest eight frames after it saved and of what it saved,
    // every value it keeps in play at each, with its strikes beside it, which netplay peers must
    // hash alike: a literal moves only when what is visited changes on purpose.
    test::checkPinnedDigests("PlayerAttacks",
                             test::liveDigest(fixture.attacks, resources.resources()),
                             test::savedDigest(fixture.attacks, saved, resources.resources()),
                             "5f5cca6b92ca7dc6c83605de719be3edef04fad0f5a35a24c86129bf30c90ac2",
                             "2a224484adb76fcac2c3c1a1423534aead82ea9e2950124f6f6da0cbcca5d147");
    test::checkPinnedDigests("PlayerAttacks' MoveStrikes",
                             test::liveDigest(fixture.attacks.strikes()),
                             test::savedDigest(fixture.attacks.strikes(), strikes),
                             "b419818750b2b85192507e374c41cb30d55036aec3605dbdd4ab3a4bec0887b3",
                             "2bc53b10cd3daa53dbd3b97a5436a5b4eedba4f8692062e3394603ab620509bd");
}
} // namespace
