#include <array>
#include <memory>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PlayerAttacks.h"

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
    attacks.updateShields(0.001f, fixture->players, {fixture->opponents, fixture->fixtures, {}});
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
} // namespace
