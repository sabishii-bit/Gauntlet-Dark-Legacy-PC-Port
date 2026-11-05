#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"
#include "game/level/ItemFigure.h"
#include "game/party/ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

std::filesystem::path nativeRoot() {
    return test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
}

WorldLayout layoutOf(std::string_view name, std::string_view text) {
    const auto directory = test::scratchDirectory(name);
    writeTextFile(directory / "world.json", text);
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    return layout;
}

struct Pool {
    test::FakeRenderDevice device;
    WorldLayout layout;
    Enemies enemies;
    Generators generators;
    std::array<EnemyView, 1> players{EnemyView{.player = 0, .position = {0, 0, 25}}};

    Pool() {
        layout = layoutOf("generator-checkpoint", R"({
          "objects":[{"name":"GROUND","position":[0,0,0]}],
          "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                         "collisionType":1,"hitPoints":10}],
          "itemInstances":[
            {"info":0,"minPlayers":1,"position":[0,0,0],
             "params":[2,0,7,0,2,0,1,0,0,0,0,0]},
            {"info":0,"minPlayers":1,"position":[30,0,0],
             "params":[1,0,15,0,1,0,1,0,0,0,0,0]},
            {"info":0,"minPlayers":2,"position":[60,0,0],
             "params":[3,0,7,0,2,0,1,0,0,0,0,0]}]})");
        enemies.open(device, nativeRoot(), nullptr, 8, {}, 347);
        REQUIRE(generators.bind(device, layout, enemies, nullptr, {1.5f, 1.25f, 1.5f}, 1));
        REQUIRE(generators.count() == 3);
    }

    void step(s32 tick) {
        players[0].position.x = static_cast<f32>(tick % 11) * 0.1f;
        if (tick == 8 || tick == 24) {
            EnemyHit hit;
            hit.player = 0;
            hit.damage = 1000;
            enemies.hurt(0, hit);
        }
        if (tick == 14 || tick == 43) {
            REQUIRE(generators.strike(0, tick == 14 ? 20.0f : 100.0f, 0));
        }
        generators.setPlayerCount(tick >= 12 && tick < 30 ? 2 : 1);
        if (tick >= 32 && tick < 38) {
            ViewVolume view;
            view.position = {10000, 0, -20};
            generators.setView(view, Vec3{10000, 0, 0});
        } else {
            generators.setView(std::nullopt);
        }
        const bool stopped = tick >= 18 && tick < 23;
        // This is the real LevelOpponents phase order. Enemy output from the last
        // phase remains queued until the next generator step consumes it.
        generators.update(1, enemies, players, {}, stopped);
        enemies.update(1, kStep, players, generators.enemyObstacles(), nullptr, 1, stopped);
    }
};

/** Both pools' local values at one moment. */
struct Frame {
    Generators::LocalSnapshot generators;
    Enemies::LocalSnapshot enemies;
};

void save(const Generators& generators, const Enemies& enemies, Frame& frame) {
    generators.saveLocal(frame.generators);
    REQUIRE(enemies.saveLocal(frame.enemies));
}

void restore(Generators& generators, Enemies& enemies, const Frame& frame) {
    REQUIRE(enemies.restoreLocal(frame.enemies));
    REQUIRE(generators.restoreLocal(frame.generators));
}

TEST_CASE("generator and enemy pools rewind quotas timers presence textures and pending broods",
          "[rollback][generators][generator-local-rewind][assets]") {
    Pool pool;
    // Seventy-three records of both pools are too much for the stack: on the heap.
    const auto initial = std::make_unique<Frame>();
    save(pool.generators, pool.enemies, *initial);
    CHECK(pool.generators.slot(2).presence == Generators::Slot::Presence::Hidden);
    REQUIRE_FALSE(pool.generators.bodyResources().empty());
    const u32 firstFrame = pool.generators.bodyResources().front().textures->clock().frame;
    const auto uploads = pool.device.texturesCreated;
    std::vector<Frame> states(72);
    for (s32 tick = 0; tick < 72; ++tick) {
        pool.step(tick);
        if (tick == 0) {
            CHECK(pool.generators.slot(2).presence == Generators::Slot::Presence::Suppressed);
        }
        if (tick == 14) {
            CHECK(pool.generators.slot(0).most == 6);
        }
        save(pool.generators, pool.enemies, states[static_cast<usize>(tick)]);
    }
    CHECK(pool.generators.slot(0).state == 0);
    CHECK(pool.generators.bodyResources().front().textures->clock().frame > firstFrame);
    CHECK(pool.generators.slot(0).bred > 0);
    restore(pool.generators, pool.enemies, *initial);
    const auto current = std::make_unique<Frame>();
    save(pool.generators, pool.enemies, *current);
    CHECK(pool.generators.equalLocal(current->generators, initial->generators));
    for (s32 tick = 0; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        save(pool.generators, pool.enemies, *current);
        const auto& expected = states[static_cast<usize>(tick)];
        CHECK(pool.generators.equalLocal(current->generators, expected.generators));
        CHECK(pool.enemies.equalLocal(current->enemies, expected.enemies));
    }
    // A delayed correction in the middle of time-stop must resume the same
    // partially elapsed quota/texture clocks, not only a pristine level start.
    restore(pool.generators, pool.enemies, states[20]);
    for (s32 tick = 21; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        save(pool.generators, pool.enemies, *current);
        const auto& expected = states[static_cast<usize>(tick)];
        CHECK(pool.generators.equalLocal(current->generators, expected.generators));
        CHECK(pool.enemies.equalLocal(current->enemies, expected.enemies));
    }
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("native generator history rewinds broods quotas presence and linked enemy output",
          "[local-rewind][generator-local-rewind][native-equality][assets]") {
    Pool pool;
    // Sixteen frames of the whole pool are too much for the stack: on the heap.
    const auto frames = std::make_unique<std::array<Frame, 16>>();
    auto& history = *frames;
    const auto step = [&](s32 tick, bool wrong) {
        pool.players[0].position = {wrong ? -80.0f : static_cast<f32>(tick % 13), 0, 25};
        pool.generators.setPlayerCount(wrong ? 4 : 1 + (tick / 23) % 2);
        pool.generators.setView(std::nullopt);
        if (tick % 43 == 0) {
            (void)pool.generators.strike((tick / 43) % 3, wrong ? 500.0f : 2.0f, 0);
        }
        if (tick % 29 == 0 && pool.enemies.count() > 0) {
            EnemyHit hit;
            hit.damage = wrong ? 5.0f : 1000.0f;
            hit.player = 0;
            (void)pool.enemies.hurt(0, hit);
        }
        const bool stopped = wrong || tick % 31 < 6;
        pool.generators.update(1, pool.enemies, pool.players, {}, stopped);
        pool.enemies.update(1, kStep, pool.players, pool.generators.enemyObstacles(), nullptr, 1,
                            stopped);
    };
    s32 tick = 0;
    for (usize window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        pool.generators.saveLocal(saved.generators);
        REQUIRE(pool.enemies.saveLocal(saved.enemies));
        const s32 distance = static_cast<s32>(window % 8 + 1);
        std::vector<Frame> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            pool.generators.saveLocal(nativeExpected[static_cast<usize>(i)].generators);
            REQUIRE(pool.enemies.saveLocal(nativeExpected[static_cast<usize>(i)].enemies));
        }
        REQUIRE(pool.generators.restoreLocal(saved.generators));
        REQUIRE(pool.enemies.restoreLocal(saved.enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, true);
        }
        REQUIRE(pool.generators.restoreLocal(saved.generators));
        REQUIRE(pool.enemies.restoreLocal(saved.enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            Frame corrected;
            pool.generators.saveLocal(corrected.generators);
            REQUIRE(pool.enemies.saveLocal(corrected.enemies));
            CHECK(pool.generators.equalLocal(corrected.generators,
                                             nativeExpected[static_cast<usize>(i)].generators));
            CHECK(pool.enemies.equalLocal(corrected.enemies,
                                          nativeExpected[static_cast<usize>(i)].enemies));
        }
        tick += distance;
    }
}

TEST_CASE(
    "native generator history includes boss births animated landings and cancelled destruction",
    "[local-rewind][generator-local-rewind][native-equality][assets]") {
    Pool pool;
    test::ProjectileCheckpointFixture art;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 20;
    info.radius = 2;
    info.height = 5;
    const bool settled = GENERATE(false, true);
    struct Observation {
        std::vector<Generators::Slot> slots;
        std::vector<std::optional<ItemFigure>> figures;
        std::unique_ptr<Enemies::LocalSnapshot> enemies =
            std::make_unique<Enemies::LocalSnapshot>();
    };
    const auto observe = [&] {
        Observation result;
        for (usize i = 0; i < pool.generators.count(); ++i) {
            result.slots.push_back(pool.generators.slot(i));
        }
        for (const auto& item : pool.generators.presentation()) {
            result.figures.push_back(item.bossFigure != nullptr ? std::optional{*item.bossFigure}
                                                                : std::nullopt);
        }
        REQUIRE(pool.enemies.saveLocal(*result.enemies));
        return result;
    };
    const auto same = [&](const Observation& a, const Observation& b) {
        return a.slots == b.slots && a.figures == b.figures &&
               pool.enemies.equalLocal(*a.enemies, *b.enemies);
    };
    const auto step = [&](s32 tick, bool wrong) {
        if (tick % 31 == 0 || (wrong && tick % 5 == 0)) {
            REQUIRE(pool.generators.placeBoss(
                pool.device, info, art.archive, pool.enemies, kGruntKind,
                glm::translate(Mat4{1}, Vec3{static_cast<f32>(tick), wrong ? 9.0f : 0.0f, 10}),
                nullptr, "FLIGHT", settled));
        }
        if (tick % 7 == 0 && pool.generators.count() > 3) {
            (void)pool.generators.strike(static_cast<s32>(pool.generators.count() - 1),
                                         wrong ? 1000.0f : 1.0f, 0);
        }
        pool.generators.update(1, pool.enemies, pool.players, {}, wrong);
        pool.enemies.update(1, kStep, pool.players, pool.generators.enemyObstacles(), nullptr, 1,
                            wrong);
    };
    Generators::LocalSnapshot generators;
    Enemies::LocalSnapshot enemies;
    s32 tick = 0;
    for (s32 window = 0; window < 64; ++window) {
        CAPTURE(window, tick, settled);
        pool.generators.saveLocal(generators);
        REQUIRE(pool.enemies.saveLocal(enemies));
        const s32 distance = window % 8 + 1;
        std::vector<Observation> expected;
        std::vector<Generators::LocalSnapshot> nativeExpected(static_cast<usize>(distance));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            expected.push_back(observe());
            pool.generators.saveLocal(nativeExpected[static_cast<usize>(i)]);
        }
        REQUIRE(pool.generators.restoreLocal(generators));
        REQUIRE(pool.enemies.restoreLocal(enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, true);
        }
        REQUIRE(pool.generators.restoreLocal(generators));
        REQUIRE(pool.enemies.restoreLocal(enemies));
        for (s32 i = 0; i < distance; ++i) {
            step(tick + i, false);
            CHECK(same(observe(), expected[static_cast<usize>(i)]));
            Generators::LocalSnapshot corrected;
            pool.generators.saveLocal(corrected);
            CHECK(pool.generators.equalLocal(corrected, nativeExpected[static_cast<usize>(i)]));
        }
        tick += distance;
    }
    CHECK(pool.generators.count() > 3);
    CHECK(pool.generators.bodyShown(3));
    CHECK(pool.device.textureUpdates == 0);
}

TEST_CASE("native generator history cannot outlive its level or owner",
          "[local-rewind][generator-local-rewind][native-equality]") {
    Generators::LocalSnapshot saved;
    Generators a;
    Generators b;
    CHECK_FALSE(a.restoreLocal(saved));
    CHECK_FALSE(a.equalLocal(saved, saved));
    a.saveLocal(saved);
    CHECK_FALSE(b.restoreLocal(saved));
    CHECK_FALSE(b.equalLocal(saved, saved));
    REQUIRE(a.restoreLocal(saved));
    CHECK(a.equalLocal(saved, saved));
    a.clear();
    CHECK_FALSE(a.restoreLocal(saved));
    CHECK_FALSE(a.equalLocal(saved, saved));
    {
        const Generators temporary;
        temporary.saveLocal(saved);
    }
    CHECK_FALSE(a.restoreLocal(saved));
}

TEST_CASE("generator source lifetimes survive authored restore and corrected dynamic births",
          "[local-rewind][generator-local-rewind][native-sound-identity][assets]") {
    Pool pool;
    std::vector<u64> authored;
    for (usize index = 0; index < pool.generators.count(); ++index) {
        const auto instance = pool.generators.instanceOf(static_cast<s32>(index));
        REQUIRE(instance != 0);
        CHECK(std::ranges::find(authored, instance) == authored.end());
        authored.push_back(instance);
    }
    CHECK(pool.generators.instanceOf(-1) == 0);
    CHECK(pool.generators.instanceOf(static_cast<s32>(authored.size())) == 0);
    Generators::LocalSnapshot authoredState;
    pool.generators.saveLocal(authoredState);
    const auto hit = pool.generators.strike(0, 1, 0);
    REQUIRE(hit);
    CHECK(hit->instance == authored[0]);
    REQUIRE(pool.generators.restoreLocal(authoredState));
    for (usize index = 0; index < authored.size(); ++index) {
        CHECK(pool.generators.instanceOf(static_cast<s32>(index)) == authored[index]);
    }

    test::ProjectileCheckpointFixture art;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 20;
    info.radius = 2;
    info.height = 5;
    const auto birth = [&](s32 kind) {
        return pool.generators.placeBoss(pool.device, info, art.archive, pool.enemies, kind,
                                         Mat4{1}, nullptr, "FLIGHT", true);
    };
    Generators::LocalSnapshot saved;
    pool.generators.saveLocal(saved);
    REQUIRE(birth(kGruntKind));
    const auto dynamic = static_cast<s32>(authored.size());
    const u64 first = pool.generators.instanceOf(dynamic);
    CHECK(first > authored.back());
    const auto destroyed = pool.generators.strike(dynamic, 1000, 0);
    REQUIRE(destroyed);
    REQUIRE(destroyed->destroyed);
    CHECK(destroyed->instance == first);
    CHECK(pool.generators.instanceOf(dynamic) == first);
    REQUIRE(birth(kGruntKind));
    const u64 second = pool.generators.instanceOf(dynamic + 1);
    CHECK(second > first);

    REQUIRE(pool.generators.restoreLocal(saved));
    CHECK(pool.generators.instanceOf(dynamic) == 0);
    // Rejected births do not consume a logical lifetime. Replaying accepted
    // births restores the original sequence, including a now-retired emitter.
    CHECK_FALSE(birth(-1));
    REQUIRE(birth(kGruntKind));
    CHECK(pool.generators.instanceOf(dynamic) == first);
    const auto replayed = pool.generators.strike(dynamic, 1000, 0);
    REQUIRE(replayed);
    CHECK(*replayed == *destroyed);
    REQUIRE(birth(kGruntKind));
    CHECK(pool.generators.instanceOf(dynamic + 1) == second);
}

TEST_CASE("native generator equality includes player admission and offscreen attention",
          "[local-rewind][generator-local-rewind][native-equality]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    Generators generators;
    REQUIRE(generators.bind(device, WorldLayout{}, enemies, nullptr, {}, 1));
    Generators::LocalSnapshot initial;
    generators.saveLocal(initial);
    const auto changed = [&](auto&& change) {
        REQUIRE(generators.restoreLocal(initial));
        change();
        Generators::LocalSnapshot after;
        generators.saveLocal(after);
        CHECK_FALSE(generators.equalLocal(initial, after));
        CHECK_FALSE(generators.equalLocal(after, initial));
        REQUIRE(generators.restoreLocal(initial));
        generators.saveLocal(after);
        CHECK(generators.equalLocal(initial, after));
    };
    changed([&] { generators.setPlayerCount(4); });
    changed([&] { generators.setView(ViewVolume{}); });
    changed([&] { generators.setView(std::nullopt, Vec3{7, 8, 9}); });
}

TEST_CASE("cached generator births reactivate their enemy stock after a native rewind",
          "[local-rewind][generator-local-rewind][generator-cached-birth][assets]") {
    Pool pool;
    REQUIRE_FALSE(pool.enemies.kindLoaded(kRatKind));
    Generators::LocalSnapshot initialGenerators;
    Enemies::LocalSnapshot initialEnemies;
    pool.generators.saveLocal(initialGenerators);
    REQUIRE(pool.enemies.saveLocal(initialEnemies));
    const auto birth = [&] {
        REQUIRE(pool.generators.spawn(pool.device, pool.enemies, kRatKind, 2, {0, 0, 5}, 0));
        REQUIRE(pool.enemies.kindLoaded(kRatKind));
        pool.generators.update(1, pool.enemies, pool.players, {}, false);
        pool.enemies.update(1, kStep, pool.players, pool.generators.enemyObstacles(), nullptr, 1,
                            false);
    };
    birth();
    Generators::LocalSnapshot expectedGenerators;
    Enemies::LocalSnapshot expectedEnemies;
    pool.generators.saveLocal(expectedGenerators);
    REQUIRE(pool.enemies.saveLocal(expectedEnemies));
    const auto uploads = pool.device.texturesCreated;
    REQUIRE(pool.generators.restoreLocal(initialGenerators));
    REQUIRE(pool.enemies.restoreLocal(initialEnemies));
    REQUIRE_FALSE(pool.enemies.kindLoaded(kRatKind));
    birth();
    Generators::LocalSnapshot replayedGenerators;
    Enemies::LocalSnapshot replayedEnemies;
    pool.generators.saveLocal(replayedGenerators);
    REQUIRE(pool.enemies.saveLocal(replayedEnemies));
    CHECK(pool.generators.equalLocal(expectedGenerators, replayedGenerators));
    CHECK(pool.enemies.equalLocal(expectedEnemies, replayedEnemies));
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("generator restoration preserves pending detached quota until the native next phase",
          "[rollback][generators][generator-local-rewind][assets]") {
    Pool pool;
    pool.generators.update(1, pool.enemies, pool.players);
    REQUIRE(pool.generators.livingOf(0) == 1);
    EnemyHit hit;
    hit.damage = 1000;
    hit.player = 0;
    REQUIRE(pool.enemies.hurt(0, hit));
    const auto before = std::make_unique<Frame>();
    save(pool.generators, pool.enemies, *before);
    const auto events = pool.enemies.takeGeneratorEvents();
    REQUIRE_FALSE(events.empty());
    REQUIRE(pool.enemies.restoreLocal(before->enemies));
    const s32 wait = pool.generators.countdownOf(0);
    pool.generators.update(1, pool.enemies, pool.players);
    CHECK(pool.generators.livingOf(0) == 0);
    CHECK(pool.generators.countdownOf(0) == wait - 1);
    const auto expected = std::make_unique<Frame>();
    save(pool.generators, pool.enemies, *expected);
    restore(pool.generators, pool.enemies, *before);
    CHECK(pool.generators.livingOf(0) == 1); // do not reconstruct this from living bodies
    CHECK(pool.enemies.takeGeneratorEvents() == events);
    REQUIRE(pool.enemies.restoreLocal(before->enemies));
    pool.generators.update(1, pool.enemies, pool.players);
    const auto current = std::make_unique<Frame>();
    save(pool.generators, pool.enemies, *current);
    CHECK(pool.generators.equalLocal(current->generators, expected->generators));
    CHECK(pool.enemies.equalLocal(current->enemies, expected->enemies));
}

TEST_CASE("special generator shared species cursor rewinds even when a birth was refused",
          "[rollback][generators][generator-local-rewind][assets]") {
    const s32 realm = GENERATE(5, 6);
    const auto root = nativeRoot();
    const std::string archive = realm == 5 ? "ITEMS/LEVELE" : "ITEMS/LEVELF";
    test::assetOrSkip(archive + "/ANIM.PS2");
    ItemArchive items;
    REQUIRE(items.load(root / archive));
    auto layout = layoutOf("generator-checkpoint-special", R"({
      "objects":[{"name":"GROUND","position":[0,0,0]}],
      "itemInfos":[{"type":3,"name":"SPECIAL","radius":2,"height":5,"hitPoints":10}],
      "itemInstances":[
        {"info":0,"position":[0,0,0],"params":[2,0,7,0,4,0,1,0,0,0,0,0]},
        {"info":0,"position":[30,0,0],"params":[2,0,7,0,4,0,1,0,0,0,0,0]}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 16, {}, 859);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, {}, realm, &items));
    REQUIRE(generators.count() == 2);
    const std::array players{EnemyView{.player = 0, .position = {0, 0, 30}}};
    const std::array blockers{Obstacle{.halfAcross = 1000, .halfAlong = 1000, .height = 100}};
    generators.update(1, enemies, players, blockers);
    REQUIRE(generators.bredOf(0) == 0);
    REQUIRE(generators.bredOf(1) == 0);
    // Ninety-one records of both pools are too much for the stack: on the heap.
    const auto initial = std::make_unique<Frame>();
    save(generators, enemies, *initial);
    std::vector<Frame> states(90);
    const auto current = std::make_unique<Frame>();
    for (s32 pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            restore(generators, enemies, *initial);
        }
        for (s32 tick = 0; tick < 90; ++tick) {
            generators.update(1, enemies, players);
            enemies.update(1, kStep, players, generators.enemyObstacles());
            if (pass == 0) {
                save(generators, enemies, states[static_cast<usize>(tick)]);
            } else {
                CAPTURE(realm, tick);
                save(generators, enemies, *current);
                const auto& expected = states[static_cast<usize>(tick)];
                CHECK(generators.equalLocal(current->generators, expected.generators));
                CHECK(enemies.equalLocal(current->enemies, expected.enemies));
            }
        }
    }
    CHECK(generators.bredOf(0) > 0);
    CHECK(generators.bredOf(1) > 0);
}

TEST_CASE("generator support correction restores exact local placement without touching the world",
          "[rollback][generators][generator-local-rewind][assets]") {
    auto layout = layoutOf("generator-checkpoint-support", R"({
      "objects":[{"name":"LIFT","position":[10,5,20],"flags":4102}],
      "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                    "xSize":3,"zSize":1,"hitPoints":10}],
      "itemInstances":[{"info":0,"position":[12,5,23],
                        "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
    WorldCollision collision;
    collision.build(
        {CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{30, 0, 30}, Vec3{30, 0, -30}},
                           .object = 0},
         CollisionTriangle{.vertices = {Vec3{-30, 0, -30}, Vec3{-30, 0, 30}, Vec3{30, 0, 30}},
                           .object = 0}});
    collision.setMovingObjects(std::array<s32, 1>{0});
    const Mat4 original = glm::translate(Mat4{1}, Vec3{10, -15, 20});
    collision.setObjectTransform(0, original);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, nativeRoot(), &collision, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
    Generators::LocalSnapshot before;
    generators.saveLocal(before);
    REQUIRE(generators.slot(0).support == 0);
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{30, 10, 40}), 1.57079637f, Vec3{0, 1, 0});
    collision.setObjectTransform(0, moved);
    generators.syncFloors();
    Generators::LocalSnapshot after;
    generators.saveLocal(after);
    CHECK_FALSE(generators.equalLocal(after, before));
    REQUIRE(generators.restoreLocal(before));
    CHECK(collision.objectTransform(0) == moved);
    Generators::LocalSnapshot current;
    generators.saveLocal(current);
    CHECK(generators.equalLocal(current, before)); // restore does not re-run syncFloors
    generators.syncFloors();
    generators.saveLocal(current);
    CHECK(generators.equalLocal(current, after));
    collision.setObjectTransform(0, original);
    REQUIRE(generators.restoreLocal(before));
    generators.syncFloors();
    generators.saveLocal(current);
    CHECK(generators.equalLocal(current, before));
}
} // namespace
