#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/LevelFixtures.h"
#include "game/level/NativeStateResources.h"
#include "game/level/ScriptedFixtures.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digest = NativeStateDigest::Digest;
constexpr f32 kStep = 1.0f / 60;

template <class Owner> Digest liveHash(const Owner& owner, NativeStateResources& resources) {
    REQUIRE(owner.registerNativeResources(resources));
    NativeStateDigest digest{resources.resources()};
    REQUIRE(owner.appendNativeDigest(digest));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

template <class Owner>
Digest savedHash(const Owner& owner, const typename Owner::LocalSnapshot& saved,
                 NativeStateResources& resources) {
    REQUIRE(owner.registerNativeResources(resources));
    NativeStateDigest digest{resources.resources()};
    REQUIRE(owner.appendLocalDigest(digest, saved));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

struct Fixtures {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    ItemArchive items;
    Chests chests;
    Breakables barrels;
    LockedGates gates;
    Traps traps;
    StaticScenery scenery;
    SafeRocks rocks;
    Rubble rubble;
    NativeStateResources resources;
    std::filesystem::path directory;

    explicit Fixtures(std::string_view name) : directory(test::scratchDirectory(name)) {
        writeTextFile(directory / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(directory / "objects.json", R"({"objects":[
          {"name":"TRI","file":"tri.obj"},
          {"name":"ROCK0","file":"tri.obj"},{"name":"ROCK1","file":"tri.obj"},
          {"name":"ROCK2","file":"tri.obj"},{"name":"ROCK3","file":"tri.obj"}]})");
        writeFile(directory / "white.png", test::kTinyPng);
        writeTextFile(directory / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"WHITE1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"TRI","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"hitPoints":10,"armor":1},
            {"type":10,"subtype":45,"name":"BARREL","radius":1,"height":3,"hitPoints":5},
            {"type":7,"name":"GATE","radius":1,"height":3},
            {"type":8,"name":"TRAP","radius":2,"height":3,"collisionType":3,"value":5,"activeOff":-4},
            {"type":10,"subtype":0,"name":"PROP","radius":1,"height":2},
            {"type":10,"subtype":41,"name":"ROCK","radius":2,"height":5,"hitPoints":25},
            {"type":1,"subtype":2,"name":"KEY","radius":1,"value":1},
            {"type":1,"subtype":1,"name":"KEY","radius":1,"value":200},
            {"type":-1,"choices":[7,8]}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[9,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":1,"position":[20,0,0]},
            {"info":2,"position":[40,0,0]},
            {"info":3,"position":[60,0,0]},
            {"info":4,"position":[80,0,0]},
            {"info":5,"position":[100,0,0]},
            {"info":6,"position":[120,0,0],"params":[41,0,3,0,0,0,0,0,0,0,0,0]}]})");
        std::string trees;
        for (const auto* const name :
             {"CHEST", "BARREL", "GATE", "TRAP", "TRAP_D", "PROP", "KEY"}) {
            if (!trees.empty()) {
                trees += ',';
            }
            trees += std::string{R"({"name":")"} + name + R"(","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"NULL1","parent":0,"position":[0,1,0]}],
              "sequences":[{"name":"REST","frames":4,"frameRate":30},
                {"name":"ACTIVE","frames":5,"frameRate":30},
                {"name":"HIT","frames":6,"frameRate":30},
                {"name":"BACK","frames":5,"frameRate":30}]})";
        }
        writeTextFile(directory / "animations.json",
                      R"({"textureAnimations":[{"name":"WHITE","texture":0,"source":0,
                       "frames":2,"rate":2}],"trees":[)" +
                          trees + "]}");
        test::convertModelFixture(directory);
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        collision.build(
            {{{0, 1, 0}, {Vec3{-20, 0, -20}, Vec3{200, 0, -20}, Vec3{-20, 0, 20}}, 0, 4100},
             {{0, 1, 0}, {Vec3{200, 0, -20}, Vec3{200, 0, 20}, Vec3{-20, 0, 20}}, 0, 4100}});
        collision.setMovingObjects(std::array<s32, 1>{0});
        collision.setObjectTransform(0, Mat4{1});
        const AssetScope scope{items};
        REQUIRE(chests.bind(device, layout, scope, &collision));
        REQUIRE(barrels.bind(device, layout, scope, &collision));
        REQUIRE(gates.bind(device, layout, scope, &collision));
        REQUIRE(traps.bind(device, layout, scope, &collision, 17));
        scenery.bind(device, layout, scope, &collision);
        REQUIRE(rocks.bind(device, layout, items));
        rocks.bindAnimations(device, items);
        REQUIRE(chests.size() == 1);
        REQUIRE(barrels.size() == 2);
        REQUIRE(gates.size() == 1);
        REQUIRE(traps.size() == 1);
        REQUIRE(scenery.size() == 1);
        REQUIRE(rocks.size() == 1);
        REQUIRE(rocks.bodyResources().size() == 4);
        REQUIRE(resources.registerArchive(items, "fixtures/items"));
        REQUIRE(resources.registerLayout(layout, "fixtures/stage"));
        REQUIRE(resources.registerCollision(collision, "fixtures/stage"));
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    }

    auto owners() { return std::tie(chests, barrels, gates, traps, scenery, rocks, rubble); }
    std::array<Digest, 7> hash() {
        return {liveHash(chests, resources),  liveHash(barrels, resources),
                liveHash(gates, resources),   liveHash(traps, resources),
                liveHash(scenery, resources), liveHash(rocks, resources),
                liveHash(rubble, resources)};
    }
    void step(u32 tick, bool wrong = false) {
        const Mat4 platform = glm::translate(Mat4{1}, Vec3{0, static_cast<f32>(tick) * 0.03f, 0});
        collision.setObjectTransform(0, platform);
        chests.capturePresentation();
        barrels.capturePresentation();
        gates.capturePresentation();
        traps.capturePresentation();
        scenery.capturePresentation();
        rocks.capturePresentation();
        const std::array chestParty{ChestVisitor{{0, 0, 0}, 1, wrong ? 0 : 1, tick % 3 == 0}};
        chests.updateXray(device, AssetScope{items}, items, chestParty);
        chests.update(kStep, chestParty);
        const std::array gateParty{ChestVisitor{{60, 0, 0}, 1, wrong ? 0 : 1}};
        gates.update(1, kStep, gateParty);
        const std::array victims{TrapVictim{{80, 1.5f, 0}, 1, 1.5f}};
        traps.update(1, kStep, victims, wrong);
        if (tick % 4 == 0) {
            barrels.strike(wrong ? 1 : 0, 2);
            rocks.strike(0, wrong ? 5 : 2);
            const std::array archives{&items};
            REQUIRE(rubble.leave(
                device, archives, "TRI",
                glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), 0, wrong ? 7 : 0})));
        }
        barrels.update(kStep);
        scenery.update(kStep);
        rocks.update(kStep);
    }
    void draw(f32 alpha) {
        const CameraFrame camera;
        chests.draw(device, Mat4{1}, {}, &camera, alpha);
        barrels.draw(device, Mat4{1}, {}, alpha);
        gates.draw(device, Mat4{1}, {}, alpha);
        traps.draw(device, Mat4{1}, {}, &camera, TreeModel::Pass::All, alpha);
        scenery.draw(device, Mat4{1}, {}, &camera, TreeModel::Pass::All, alpha);
        rocks.draw(device, Mat4{1}, {}, alpha);
        rubble.draw(device, Mat4{1}, {});
        device.draws.clear();
    }
};

TEST_CASE("fixture native digests agree across independently loaded resources and draw cadences",
          "[native-state-digest][fixtures-native-digest]") {
    Fixtures first{"fixture-native-a"};
    Fixtures second{"fixture-native-b"};
    REQUIRE(&first.items != &second.items);
    CHECK(first.hash() == second.hash());
    for (u32 tick = 0; tick < 24; ++tick) {
        CAPTURE(tick);
        first.step(tick);
        second.step(tick);
        const auto before = second.hash();
        for (f32 const alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
            second.draw(alpha);
            CHECK(second.hash() == before);
        }
        CHECK(first.hash() == second.hash());
    }
}

TEST_CASE("fixture native saved values survive one to eight wrong-branch corrections",
          "[native-state-digest][fixtures-native-digest][local-rewind]") {
    Fixtures fixture{"fixture-native-replay"};
    auto owners = fixture.owners();
    const auto history = std::apply(
        [](const auto&... owner) {
            return std::tuple<typename std::remove_cvref_t<decltype(owner)>::LocalSnapshot...>{};
        },
        owners);
    auto saved = history;
    const auto save = [&]<usize... I>(std::index_sequence<I...>) {
        (std::get<I>(owners).saveLocal(std::get<I>(saved)), ...);
    };
    const auto restore = [&]<usize... I>(std::index_sequence<I...>) {
        const auto one = [](auto& owner, const auto& state) { REQUIRE(owner.restoreLocal(state)); };
        (one(std::get<I>(owners), std::get<I>(saved)), ...);
    };
    const auto checkSaved = [&]<usize... I>(const auto& expected, std::index_sequence<I...>) {
        const auto one = [&](const auto& owner, const auto& state, const auto& hash) {
            CHECK(savedHash(owner, state, fixture.resources) == hash);
        };
        (one(std::get<I>(owners), std::get<I>(saved), expected[I]), ...);
    };
    constexpr auto kIndices = std::make_index_sequence<7>{};
    save(kIndices);
    const auto initial = fixture.hash();
    for (u32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        restore(kIndices);
        std::array<std::array<Digest, 7>, 8> expected{};
        for (u32 tick = 0; tick < distance; ++tick) {
            fixture.step(tick);
            expected[tick] = fixture.hash();
        }
        checkSaved(initial, kIndices);
        restore(kIndices);
        for (u32 tick = 0; tick < distance; ++tick) {
            fixture.step(tick, true);
        }
        CHECK(fixture.hash() != expected[distance - 1]);
        checkSaved(initial, kIndices);
        const auto uploads = fixture.device.textureUpdates;
        const auto textures = fixture.device.texturesCreated;
        restore(kIndices);
        CHECK(fixture.hash() == initial);
        CHECK(fixture.device.textureUpdates == uploads);
        CHECK(fixture.device.texturesCreated == textures);
        for (u32 tick = 0; tick < distance; ++tick) {
            fixture.step(tick);
            CHECK(fixture.hash() == expected[tick]);
        }
    }
}

TEST_CASE("fixture native digests include hidden contact clocks supports and choices",
          "[native-state-digest][fixtures-native-digest]") {
    Fixtures f{"fixture-native-hidden"};
    const auto changed = [&]<class Owner, class Mutate>(Owner& owner, Mutate mutate) {
        typename Owner::LocalSnapshot saved;
        owner.saveLocal(saved);
        const auto before = liveHash(owner, f.resources);
        mutate();
        CHECK(liveHash(owner, f.resources) != before);
        CHECK(savedHash(owner, saved, f.resources) == before);
        REQUIRE(owner.restoreLocal(saved));
        CHECK(liveHash(owner, f.resources) == before);
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.chests, [&] { const_cast<Chests::Chest&>(f.chests.chest(0)).refusalLeft = 0.25f; });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.chests, [&] { const_cast<Chests::Chest&>(f.chests.chest(0)).held = 41; });
    changed(f.chests, [&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& chest = const_cast<Chests::Chest&>(f.chests.chest(0));
        REQUIRE(chest.floor);
        chest.floor->local[3].x += 0.25f;
    });
    changed(f.chests, [&] {
        const auto chest = f.chests.chest(0);
        const std::array visitor{ChestVisitor{{0, 0, 0}, 1, 1}};
        REQUIRE_FALSE(f.chests.update(0, visitor).empty());
        // Restore the visible/native chest record only: the private choice RNG
        // must still differ, despite identical contents and artwork.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<Chests::Chest&>(f.chests.chest(0)) = chest;
    });
    changed(f.barrels, [&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<Breakables::Barrel&>(f.barrels.barrel(0)).breakingSeconds = 0.02f;
    });
    changed(f.barrels, [&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<Breakables::Barrel&>(f.barrels.barrel(0)).collisionOffset.z = 0.5f;
    });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.gates, [&] { const_cast<LockedGates::Gate&>(f.gates.gate(0)).openingTicks = 1; });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.traps, [&] { const_cast<Traps::Trap&>(f.traps.trap(0)).ticksLeft += 1; });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.traps, [&] { const_cast<Traps::Trap&>(f.traps.trap(0)).rectangularContact = false; });
    changed(f.traps, [&] {
        const auto before = f.traps.trap(0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& trap = const_cast<Traps::Trap&>(f.traps.trap(0));
        trap.action = 3;
        trap.ticksLeft = 0;
        f.traps.update(1, 0, {});
        REQUIRE(f.traps.trap(0).action == Traps::kResting);
        REQUIRE(f.traps.wakes().empty());
        trap = before; // Only the private rest-time RNG now differs.
    });
    changed(f.traps, [&] {
        const auto before = f.traps.trap(0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& trap = const_cast<Traps::Trap&>(f.traps.trap(0));
        trap.ticksLeft = 0;
        f.traps.update(1, 0, {});
        REQUIRE(f.traps.wakes().size() == 1);
        trap = before; // The pending wake is still part of this saved native tick.
    });
    changed(f.traps, [&] {
        const auto before = f.traps.trap(0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& trap = const_cast<Traps::Trap&>(f.traps.trap(0));
        trap.action = 2;
        trap.ticksLeft = 10;
        const std::array victims{TrapVictim{{80, 1.5f, 0}, 1, 1.5f}};
        REQUIRE(f.traps.update(0, 0, victims).size() == 1);
        trap = before; // Respite remains even though the visible trap is identical.
    });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.scenery, [&] { const_cast<StaticScenery::Prop&>(f.scenery.prop(0)).shown = false; });
    changed(f.rocks, [&] { f.rocks.scheduleActivation(0, 0.25f); });
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    changed(f.rocks, [&] { const_cast<SafeRocks::Rock&>(f.rocks.rock(0)).baseHealth += 1; });
}

struct Coordinator {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    NativeStateResources resources;
    std::array<PlayerRuntime, 2> players;
    test::ScriptedFixtures quiet;

    explicit Coordinator(const std::filesystem::path& directory = {}) {
        if (!directory.empty()) {
            const LevelRef level{"TEST", 0, "TEST", "", "", "", "", 0};
            REQUIRE(world.load(device, directory, level));
            REQUIRE(resources.registerArchive(world.items(), "coordinator/items"));
        }
        fixtures.bind({device, world, weapons, effects, audio, 1});
        REQUIRE(resources.registerLayout(world.layout(), "coordinator/stage"));
        REQUIRE(resources.registerCollision(world.collision(), "coordinator/stage"));
        players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, {6, 0, 0}, 0);
    }
    static test::FixtureScript events(s32 identity) {
        return {.opponents = [identity](const Vec3&, f32, f32, std::vector<s32>& reached, u32) {
            if (std::ranges::find(reached, identity) == reached.end()) {
                reached.push_back(identity);
            }
        }};
    }
};

TEST_CASE("fixture coordinator hashes poison clouds and pending trapped chest destruction",
          "[native-state-digest][fixtures-native-digest]") {
    const Fixtures files{"fixture-native-coordinator"};
    Coordinator f{files.directory};
    REQUIRE(f.fixtures.barrels().size() == 2);
    REQUIRE(f.fixtures.chests().size() == 1);
    LevelFixtures::LocalSnapshot initial;
    f.fixtures.saveLocal(initial);
    const auto before = liveHash(f.fixtures, f.resources);
    Breakables::LocalSnapshot barrels;
    f.fixtures.barrels().saveLocal(barrels);
    f.fixtures.strikeBarrel(1, 1000, -1, {}, f.quiet);
    REQUIRE_FALSE(f.fixtures.barrels().standing(1));
    // Isolate the cloud from its source barrel: otherwise a missing cloud visitor
    // could be hidden by the barrel's independently changed health/animation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    REQUIRE(const_cast<Breakables&>(f.fixtures.barrels()).restoreLocal(barrels));
    const auto cloud = liveHash(f.fixtures, f.resources);
    CHECK(cloud != before);
    CHECK(savedHash(f.fixtures, initial, f.resources) == before);
    LevelFixtures::LocalSnapshot cloudState;
    f.fixtures.saveLocal(cloudState);
    f.fixtures.update(1, kStep, {}, f.quiet);
    const auto advanced = liveHash(f.fixtures, f.resources);
    CHECK(advanced != cloud);
    CHECK(savedHash(f.fixtures, cloudState, f.resources) == cloud);
    REQUIRE(f.fixtures.restoreLocal(cloudState));
    f.fixtures.update(1, kStep, {}, f.quiet);
    CHECK(liveHash(f.fixtures, f.resources) == advanced);

    REQUIRE(f.fixtures.restoreLocal(initial));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& chest = const_cast<Chests::Chest&>(f.fixtures.chests().chest(0));
    chest.subtype = Chests::kTrappedChest;
    const auto noPending = liveHash(f.fixtures, f.resources);
    std::vector<s32> reached;
    f.fixtures.blastScenery(chest.figure.position(), 8, 30, 0x400, {}, f.quiet, reached);
    REQUIRE_FALSE(chest.gone);
    CHECK(liveHash(f.fixtures, f.resources) != noPending);
    LevelFixtures::LocalSnapshot pending;
    f.fixtures.saveLocal(pending);
    const auto pendingHash = savedHash(f.fixtures, pending, f.resources);
    f.fixtures.update(1, kStep, {}, f.quiet);
    REQUIRE(f.fixtures.chests().chest(0).gone);
    const auto destroyed = liveHash(f.fixtures, f.resources);
    CHECK(savedHash(f.fixtures, pending, f.resources) == pendingHash);
    REQUIRE(f.fixtures.restoreLocal(pending));
    f.fixtures.update(1, kStep, {}, f.quiet);
    CHECK(liveHash(f.fixtures, f.resources) == destroyed);
}

TEST_CASE("fixture coordinator hashes blast hit identities clocks and confirmed-replay state",
          "[native-state-digest][fixtures-native-digest]") {
    Coordinator f;
    const auto reaches17 = Coordinator::events(17);
    const auto reaches19 = Coordinator::events(19);
    const auto reaches31 = Coordinator::events(31);
    test::ScriptedFixtures marks17{reaches17};
    test::ScriptedFixtures marks19{reaches19};
    test::ScriptedFixtures marks31{reaches31};
    LevelFixtures::LocalSnapshot initial;
    f.fixtures.saveLocal(initial);
    const auto before = liveHash(f.fixtures, f.resources);
    f.fixtures.blast({0, 0, 0}, 12, 30, {}, marks17);
    const auto first = liveHash(f.fixtures, f.resources);
    CHECK(first != before);
    CHECK(savedHash(f.fixtures, initial, f.resources) == before);
    REQUIRE(f.fixtures.restoreLocal(initial));
    f.fixtures.blast({0, 0, 0}, 12, 30, {}, marks19);
    CHECK(liveHash(f.fixtures, f.resources) != first);
    REQUIRE(f.fixtures.restoreLocal(initial));
    f.fixtures.blast({0, 0, 0}, 12, 30, {}, marks17);
    CHECK(liveHash(f.fixtures, f.resources) == first);
    for (u32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        LevelFixtures::LocalSnapshot saved;
        f.fixtures.saveLocal(saved);
        std::array<Digest, 8> expected{};
        for (u32 tick = 0; tick < distance; ++tick) {
            f.fixtures.advanceBlasts(kStep, {}, marks17);
            expected[tick] = liveHash(f.fixtures, f.resources);
        }
        REQUIRE(f.fixtures.restoreLocal(saved));
        f.fixtures.blast({7, 0, 0}, 18, 40, {}, marks31);
        f.fixtures.advanceBlasts(0.2f, {}, marks31);
        CHECK(liveHash(f.fixtures, f.resources) != expected[distance - 1]);
        REQUIRE(f.fixtures.restoreLocal(saved));
        for (u32 tick = 0; tick < distance; ++tick) {
            f.fixtures.advanceBlasts(kStep, {}, marks17);
            CHECK(liveHash(f.fixtures, f.resources) == expected[tick]);
        }
    }
}

TEST_CASE("fixture native digests refuse missing resources foreign saves and stale child bindings",
          "[native-state-digest][fixtures-native-digest]") {
    Fixtures first{"fixture-native-guards"};
    const Fixtures foreign{"fixture-native-foreign"};
    std::apply(
        [&](auto&... owner) {
            const auto test = [&](auto& value) {
                using Owner = std::remove_cvref_t<decltype(value)>;
                typename Owner::LocalSnapshot saved;
                const typename Owner::LocalSnapshot empty{};
                value.saveLocal(saved);
                NativeStateDigest absent;
                CHECK_FALSE(value.appendLocalDigest(absent, empty));
                CHECK_FALSE(absent.checkedFinish());
                value.clear();
                NativeStateDigest stale{first.resources.resources()};
                CHECK_FALSE(value.appendLocalDigest(stale, saved));
                CHECK_FALSE(stale.checkedFinish());
            };
            (test(owner), ...);
        },
        first.owners());
    Chests::LocalSnapshot other;
    foreign.chests.saveLocal(other);
    NativeStateDigest different{first.resources.resources()};
    CHECK_FALSE(first.chests.appendLocalDigest(different, other));
    NativeStateDigest unresolved;
    CHECK_FALSE(foreign.chests.appendNativeDigest(unresolved));
    CHECK_FALSE(unresolved.checkedFinish());
    Coordinator coordinator;
    LevelFixtures::LocalSnapshot saved;
    coordinator.fixtures.saveLocal(saved);
    coordinator.fixtures.gates().clear();
    NativeStateDigest stale{coordinator.resources.resources()};
    CHECK_FALSE(coordinator.fixtures.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("fixture native digest bytes stay pinned for a bound coordinator",
          "[native-state-digest][fixtures-native-digest][native-digest-pin]") {
    const Fixtures files{"fixture-native-pin"};
    Coordinator f{files.directory};
    REQUIRE(f.fixtures.safeRocks().size() == 1);
    f.fixtures.setPlayerCount(3);
    REQUIRE(f.fixtures.setChestLocked(0, false));
    f.fixtures.safeRocks().scheduleActivation(0, 0.25f);
    // SHA-256 of the native digest of the fixtures above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "5c9ac85316d653984c35575018b61c85719ee92067f0abd41dbfd2349302829c";
    CHECK(test::hexBytes(liveHash(f.fixtures, f.resources)) == kDigest);
}

TEST_CASE("fixture native digest bytes stay pinned after the fixtures' script and what they saved",
          "[native-state-digest][fixtures-native-digest][digest-golden]") {
    Fixtures f{"fixture-native-golden"};
    for (u32 tick = 0; tick < 8; ++tick) {
        f.step(tick);
    }
    Chests::LocalSnapshot chests;
    Breakables::LocalSnapshot barrels;
    LockedGates::LocalSnapshot gates;
    Traps::LocalSnapshot traps;
    StaticScenery::LocalSnapshot scenery;
    SafeRocks::LocalSnapshot rocks;
    Rubble::LocalSnapshot rubble;
    f.chests.saveLocal(chests);
    f.barrels.saveLocal(barrels);
    f.gates.saveLocal(gates);
    f.traps.saveLocal(traps);
    f.scenery.saveLocal(scenery);
    f.rocks.saveLocal(rocks);
    f.rubble.saveLocal(rubble);
    for (u32 tick = 8; tick < 20; ++tick) {
        f.step(tick);
    }
    // SHA-256 of each fixture's native digest twenty ticks in and of what it saved eight ticks
    // in, which netplay peers must hash alike: a literal moves only when what is visited changes
    // on purpose.
    test::checkPinnedDigests("Chests", liveHash(f.chests, f.resources),
                             savedHash(f.chests, chests, f.resources),
                             "0f1172307b1850c522103a48c9c3b771556fbd415b612499b365eebbf2188845",
                             "8154ad3babbc54d4b59227aab6d6ad05b63e8f65b989b13d3c3e75a1abfeaa36");
    test::checkPinnedDigests("Breakables", liveHash(f.barrels, f.resources),
                             savedHash(f.barrels, barrels, f.resources),
                             "bbfca70cd5241373d49c8a96a6647e1502020f9b15c41a1831669d8e20446223",
                             "db0979a1fc877cddbba6f6733bea0b08e73d2db137f08a20ef72a1e0eda6360f");
    test::checkPinnedDigests("LockedGates", liveHash(f.gates, f.resources),
                             savedHash(f.gates, gates, f.resources),
                             "d46ea6bdc910b9858ea20babb0874c9dada6dab4a3765aad7731dd4d056cd8b4",
                             "075cf292fb924bcf9b105fed838e0e7eee5f2e36609bb302dfb286707dd42114");
    test::checkPinnedDigests("Traps", liveHash(f.traps, f.resources),
                             savedHash(f.traps, traps, f.resources),
                             "0ba88a1b02389baac1f5a1b6dcddcd8a2e4ae876b1901dc2abcc465a436737c7",
                             "7f8ce9178bad9d3e7c2c3346b8059773d202cd538033be00bb14d05392ac9f18");
    test::checkPinnedDigests("StaticScenery", liveHash(f.scenery, f.resources),
                             savedHash(f.scenery, scenery, f.resources),
                             "43e9f636515e3630bb8dd2af65b16d3f28a7c6b1075656681736884d056f0aa4",
                             "56c52e3867389fdf658b00eae32a3284a8a76434f4f6cedc572f29e43195f5da");
    test::checkPinnedDigests("SafeRocks", liveHash(f.rocks, f.resources),
                             savedHash(f.rocks, rocks, f.resources),
                             "8cc2221a6c84beb3ac5ce77b1fc1b0aa5aa3711a31b21754fc970b6d1972e8d1",
                             "7ed3bdc23a18825a5ebf8978a3c720453c9d9d097acc8817a87b72c39700d3a9");
    test::checkPinnedDigests("Rubble", liveHash(f.rubble, f.resources),
                             savedHash(f.rubble, rubble, f.resources),
                             "e8806dd05a41d4817c857c4edab8c4e6a447a16135785e1127b006d0c8e02736",
                             "4252fd9784cba56fddf781030e33c856e4c3a0d2141d83e59c154ef78daf02ee");
}

TEST_CASE("fixture coordinator and child digest bytes stay pinned after a script and its save",
          "[native-state-digest][fixtures-native-digest][digest-golden]") {
    const Fixtures files{"fixture-native-golden-coordinator"};
    Coordinator f{files.directory};
    const auto update = [&f](u32 ticks) {
        for (u32 tick = 0; tick < ticks; ++tick) {
            f.fixtures.capturePresentation();
            f.fixtures.update(1, kStep, {}, f.quiet);
        }
    };
    f.fixtures.setPlayerCount(3);
    update(4);
    f.fixtures.strikeBarrel(1, 1000, -1, {}, f.quiet);
    update(2);
    const LevelFixtures& fixtures = f.fixtures;
    LevelFixtures::LocalSnapshot saved;
    Chests::LocalSnapshot chests;
    LockedGates::LocalSnapshot gates;
    Traps::LocalSnapshot traps;
    Breakables::LocalSnapshot barrels;
    SafeRocks::LocalSnapshot rocks;
    StaticScenery::LocalSnapshot scenery;
    fixtures.saveLocal(saved);
    fixtures.chests().saveLocal(chests);
    fixtures.gates().saveLocal(gates);
    fixtures.traps().saveLocal(traps);
    fixtures.barrels().saveLocal(barrels);
    fixtures.safeRocks().saveLocal(rocks);
    fixtures.scenery().saveLocal(scenery);
    REQUIRE(f.fixtures.setChestLocked(0, false));
    REQUIRE(f.fixtures.unlockGate(0));
    f.fixtures.strikeBarrel(0, 4, -1, {}, f.quiet);
    f.fixtures.strikeSafeRock(0, 3);
    f.fixtures.blast({100, 0, 0}, 12, 30, {}, f.quiet);
    update(6);
    // SHA-256 of the coordinator's native digest after the script and of what it saved before
    // the chest was unlocked, the gate opened, the first barrel and the rock struck and the blast
    // set off, then of each child beside it, so that a moved coordinator names its child: a
    // literal moves only when what is visited changes on purpose. The rubble is left out: this
    // level's archive has none of the rubble models, so it stays empty, and the fixtures' own
    // pins above hold its bytes.
    test::checkPinnedDigests("LevelFixtures", liveHash(fixtures, f.resources),
                             savedHash(fixtures, saved, f.resources),
                             "0c334abfddf5570a184be7b4f5a786e25b3018518f3b7fa9a59ffdcb4fe420ef",
                             "abf940e130f7b49b4b6a83514700c1343821de98371288b30e92cd8e3ad9dd78");
    test::checkPinnedDigests("Chests", liveHash(fixtures.chests(), f.resources),
                             savedHash(fixtures.chests(), chests, f.resources),
                             "7acf81db02a35a922705af157f1e235debf05f28966b32b4fc8f3731340fefdd",
                             "c19e3dc0252e0a012338a03f07856d985024795d9e4c3ce3a17a52bb3b1e9ef9");
    test::checkPinnedDigests("LockedGates", liveHash(fixtures.gates(), f.resources),
                             savedHash(fixtures.gates(), gates, f.resources),
                             "c172ce007f2c3926801ee018c558c878cf1bdbde27f83d0b253f72854388c931",
                             "1e1596602be6669d4cdd0a43ce3897290697bcef2b36703077aa58326508d8cd");
    test::checkPinnedDigests("Traps", liveHash(fixtures.traps(), f.resources),
                             savedHash(fixtures.traps(), traps, f.resources),
                             "691b9e01e777a878a51648110009892140120301875fbbec2f85f63fafbcf1b7",
                             "5e2c631b70b7939b191801881dfa6bcfa3934820692c9709f719fa2e9d92818f");
    test::checkPinnedDigests("Breakables", liveHash(fixtures.barrels(), f.resources),
                             savedHash(fixtures.barrels(), barrels, f.resources),
                             "466fac1641504067308b6b27399f712208d682a41ae5d84832fc86c6118fc1f6",
                             "f86a6a076d0b2ee4601d1fcca36d3d50e5ad9877b6dacb372a8063ade6a75641");
    test::checkPinnedDigests("SafeRocks", liveHash(fixtures.safeRocks(), f.resources),
                             savedHash(fixtures.safeRocks(), rocks, f.resources),
                             "a847aa4b3f32765cd439769cd871b935afd4f54f93124df913b913baa6deb6bf",
                             "6e544e07490e95de66d13385dca422316b52dec9bfa89ccf52d55f04c7e13f57");
    test::checkPinnedDigests("StaticScenery", liveHash(fixtures.scenery(), f.resources),
                             savedHash(fixtures.scenery(), scenery, f.resources),
                             "44b78868be6618e7a99f5bf34d1d27ee332c209b8d9754ea1a961d0cc3a3dd22",
                             "76b76ab2cfd353795a5d6d30161a08d9b6d0ec96a7524488a2922bc44aa9ce46");
}
} // namespace
