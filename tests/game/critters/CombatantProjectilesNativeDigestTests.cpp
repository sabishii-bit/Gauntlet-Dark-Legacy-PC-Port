#include <array>
#include <functional>
#include <string_view>
#include <typeindex>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/critters/CombatantProjectiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterData data;
    EffectTrees effects;
    CombatantProjectiles pool;
    u32 archiveId = 8, specId = 5;

    explicit Fixture(std::string_view name = "combatant-projectile-native-digest") {
        const auto root = test::scratchDirectory(name);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
          {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]},
          {"name":"LOOP","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
          {"name":"HIT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
          "damages":[
            {"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":2,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":2097184,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":1,"flags":67108864,"behaviorFlags":9,"radius":0.5,"maxDistance":2,"damage":1,"minSpeed":30,"maxSpeed":30,"sfxIndex":3,"sfx":2},
            {"type":8,"flags":67108864,"radius":3,"maxDistance":3,"damage":1,"sfxIndex":0,"morph":1,"morphEnd":2,"morphLife":0.5},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":4,"sfx":2},
            {"type":1,"flags":0,"behaviorFlags":9,"radius":0.5,"damage":0,"minSpeed":30,"maxSpeed":30,"sfxIndex":5,"sfx":2}],
          "sounds":[{"name":"SHOT","flags":8},{"name":"LOOP"},{"name":"HIT","flags":16},
            {"name":"LOOP","life":100},{"name":"SHOT","flags":131072},{"name":"SHOT","flags":4194304}]})");
        test::convertModelFixture(root);
        REQUIRE(archive.load(root));
        REQUIRE(data.load(root / "critter.json"));
    }

    static bool resource(NativeStateDigest& digest, std::type_index type, const void* address,
                         void* context) {
        const auto& fixture = *static_cast<const Fixture*>(context);
        if (type == typeid(CritterData) && address == &fixture.data) {
            digest.string("fixture/combatant/spec");
            digest.word(fixture.specId);
            return true;
        }
        if (type == typeid(ItemArchive) && address == &fixture.archive) {
            digest.string("fixture/combatant/archive");
            digest.word(fixture.archiveId);
            return true;
        }
        return false;
    }
    NativeStateDigest::Resources resources() { return {this, resource}; }
    NativeStateDigest::Digest hash() {
        NativeStateDigest digest(resources());
        REQUIRE(pool.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    }
    CombatShot shot(s32 row) {
        CombatShot value;
        value.data = &data;
        value.critter = 11;
        value.damageIndex = row;
        value.origin = {0, 3, 0};
        value.target = Vec3{0, 3, 30};
        return value;
    }
    void launch(s32 row = 0, bool wrong = false) {
        auto value = shot(row);
        if (wrong) {
            value.target = Vec3{9, 7, -20};
            value.damageScale = 0.5f;
        }
        pool.launch(value, archive, device, effects, {});
    }
    void step(f32 seconds, std::span<const EnemyView> players = {}) {
        effects.capturePresentation();
        effects.update(seconds);
        pool.update(seconds, nullptr, players, device, effects, {});
    }
};

TEST_CASE("combatant native digest distinguishes semantic archive and spec bindings",
          "[native-digest][combatant-projectiles-native-digest]") {
    Fixture a("combatant-native-digest-owner-a");
    Fixture b("combatant-native-digest-owner-b");
    a.launch();
    b.launch();
    REQUIRE(a.pool.count() == 1);
    REQUIRE(b.pool.count() == 1);
    CHECK(a.hash() == b.hash());
    ++b.archiveId;
    CHECK(a.hash() != b.hash());
    --b.archiveId;
    ++b.specId;
    CHECK(a.hash() != b.hash());
    --b.specId;
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    for (u32 tick = 0; tick < 20; ++tick) {
        a.step(1.0f / 60, players);
        b.step(1.0f / 60, players);
        CHECK(a.hash() == b.hash());
    }
    // Recreate a flight so both borrowed resources must be resolved.
    a.launch();
    NativeStateDigest unbound;
    CHECK_FALSE(a.pool.appendNativeDigest(unbound));
    CHECK_FALSE(unbound.checkedFinish());
    NativeStateDigest wrong(b.resources());
    CHECK_FALSE(a.pool.appendNativeDigest(wrong));
    CHECK_FALSE(wrong.checkedFinish());
    CombatantProjectiles::LocalSnapshot snapshot;
    a.pool.saveLocal(snapshot);
    NativeStateDigest unresolvedLocal;
    CHECK_FALSE(a.pool.appendLocalDigest(unresolvedLocal, snapshot));
    CHECK_FALSE(unresolvedLocal.checkedFinish());
}

TEST_CASE("combatant native digest follows one through eight tick native replay",
          "[native-digest][combatant-projectiles-native-digest][local-rewind]") {
    Fixture f;
    const auto row = GENERATE(0, 1, 2, 3, 4, 5);
    const auto ticks = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    f.launch(row);
    REQUIRE(f.pool.count() == 1);
    f.step(0.06f, players);
    CombatantProjectiles::LocalSnapshot saved;
    EffectTrees::LocalSnapshot effects;
    f.pool.saveLocal(saved);
    f.effects.saveLocal(effects);
    const auto baseline = f.hash();
    NativeStateDigest stored(f.resources());
    REQUIRE(f.pool.appendLocalDigest(stored, saved));
    CHECK(stored.checkedFinish() == baseline);
    std::array<NativeStateDigest::Digest, 8> expected{};
    auto advance = [&](s32 tick, bool wrong) {
        if (tick % 3 == 0) {
            f.launch(row, wrong);
        }
        f.step(1.0f / 60, players);
    };
    for (s32 tick = 0; tick < ticks; ++tick) {
        advance(tick, false);
        expected[static_cast<usize>(tick)] = f.hash();
    }
    REQUIRE(f.pool.restoreLocal(saved));
    REQUIRE(f.effects.restoreLocal(effects));
    for (s32 tick = 0; tick < ticks; ++tick) {
        advance(tick, true);
    }
    CHECK(f.hash() != expected[static_cast<usize>(ticks - 1)]);
    REQUIRE(f.pool.restoreLocal(saved));
    REQUIRE(f.effects.restoreLocal(effects));
    CHECK(f.hash() == baseline);
    for (s32 tick = 0; tick < ticks; ++tick) {
        advance(tick, false);
        CHECK(f.hash() == expected[static_cast<usize>(tick)]);
    }
}

TEST_CASE("combatant native digest retains undelivered impacts and refuses invalid history",
          "[native-digest][combatant-projectiles-native-digest]") {
    Fixture f;
    f.launch();
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    f.step(0.3f, players);
    CombatantProjectiles::LocalSnapshot saved;
    f.pool.saveLocal(saved);
    const auto baseline = f.hash();
    REQUIRE_FALSE(f.pool.takeHits().empty());
    CHECK(f.hash() != baseline);
    REQUIRE(f.pool.restoreLocal(saved));
    CHECK(f.hash() == baseline);
    const CombatantProjectiles foreign;
    NativeStateDigest invalid;
    const auto untouched = invalid.finish();
    CHECK_FALSE(foreign.appendLocalDigest(invalid, saved));
    CHECK(invalid.finish() == untouched);
    f.pool.clear(f.effects);
    CHECK_FALSE(f.pool.appendLocalDigest(invalid, saved));
    CHECK(invalid.finish() == untouched);
    const CombatantProjectiles::LocalSnapshot empty;
    CHECK_FALSE(f.pool.appendLocalDigest(invalid, empty));
}

TEST_CASE("native combat area and launch visitors retain every mutable field",
          "[native-digest][combatant-projectiles-native-digest]") {
    const auto hash = [](const auto& value) {
        NativeStateDigest digest;
        digest.value(value);
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    CritterArea area;
    area.worldParent = Mat4{1};
    const auto baseline = hash(area);
    const std::array<std::function<void(CritterArea&)>, 12> mutations{
        [](auto& v) { v.local[3].x += 1; },
        [](auto& v) { v.node = "ROOT"; },
        [](auto& v) { (*v.worldParent)[3].y += 1; },
        [](auto& v) { v.velocity.x += 1; },
        [](auto& v) { v.displacement.z += 1; },
        [](auto& v) { v.radius += 1; },
        [](auto& v) { v.minDot += 0.1f; },
        [](auto& v) { v.damage += 1; },
        [](auto& v) { ++v.flags; },
        [](auto& v) { v.secondsLeft += 1; },
        [](auto& v) { v.lifetime += 1; },
        [](auto& v) { v.expanding = !v.expanding; }};
    for (usize i = 0; i < mutations.size(); ++i) {
        CAPTURE(i);
        auto changed = area;
        mutations[i](changed);
        CHECK(hash(changed) != baseline);
    }
    const CombatShot shot;
    const auto shotBaseline = hash(shot);
    const std::array<std::function<void(CombatShot&)>, 11> shotMutations{
        [](auto& v) { ++v.critter; },        [](auto& v) { ++v.damageIndex; },
        [](auto& v) { v.origin.x += 1; },    [](auto& v) { v.forward.y += 1; },
        [](auto& v) { v.target = Vec3{1}; }, [](auto& v) { v.rate += 0.1f; },
        [](auto& v) { v.scale += 0.1f; },    [](auto& v) { v.damageScale += 0.1f; },
        [](auto& v) { v.birthLife += 1; },   [](auto& v) { v.endVisual = !v.endVisual; },
        [](auto& v) { v.realm = 'B'; }};
    for (usize i = 0; i < shotMutations.size(); ++i) {
        CAPTURE(i);
        auto changed = shot;
        shotMutations[i](changed);
        CHECK(hash(changed) != shotBaseline);
    }
}

TEST_CASE("combatant native digest bytes stay pinned for a launched flight",
          "[native-digest][combatant-projectiles-native-digest][native-digest-pin]") {
    Fixture f("combatant-projectile-native-pin");
    auto shot = f.shot(1);
    shot.damageScale = 0.75f;
    f.pool.launch(shot, f.archive, f.device, f.effects, {});
    REQUIRE(f.pool.count() == 1);
    // SHA-256 of the native digest of the projectile pool above, which netplay peers must hash
    // alike: a literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "045347dfd3ff2be7a691cd0ce1cfba342936f29bf5bf6717b19f2bf940ff75e1";
    CHECK(test::hexBytes(f.hash()) == kDigest);
}

TEST_CASE("combatant native digest bytes stay pinned after the volley script and what it saved",
          "[native-digest][combatant-projectiles-native-digest][digest-golden]") {
    Fixture f("combatant-projectile-native-golden");
    const std::array players{EnemyView{0, {0, 0, 3}, 1, 6}};
    f.launch();
    f.step(0.06f, players);
    // Another shot every third tick, a player standing in their path.
    const auto advance = [&](s32 tick) {
        if (tick % 3 == 0) {
            f.launch();
        }
        f.step(1.0f / 60, players);
    };
    for (s32 tick = 0; tick < 4; ++tick) {
        advance(tick);
    }
    CombatantProjectiles::LocalSnapshot saved;
    f.pool.saveLocal(saved);
    for (s32 tick = 4; tick < 12; ++tick) {
        advance(tick);
    }
    // SHA-256 of the pool's native digest twelve ticks after its first step and of what it saved
    // four ticks after it, which netplay peers must hash alike: a literal moves only when what is
    // visited changes on purpose.
    test::checkPinnedDigests("CombatantProjectiles", test::liveDigest(f.pool, f.resources()),
                             test::savedDigest(f.pool, saved, f.resources()),
                             "751a980e2862374836acb449ff91a9d8d2219613aec5c44a9964f633e5a2412b",
                             "407f79e541e67db8e034ced7b54dfb2b424a3f95ac38620bd6dc1cf4e6a14631");
}
} // namespace
