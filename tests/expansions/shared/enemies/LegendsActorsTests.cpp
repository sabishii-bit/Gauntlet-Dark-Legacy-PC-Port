#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/critters/Critters.h"
#include "expansions/n64/critters/LegendsCritters.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMissiles.h"
#include "expansions/shared/enemies/LegendsActors.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
std::filesystem::path artwork() {
    const auto root = test::scratchDirectory("legends-pending-actors");
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    writeTextFile(root / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(root / "objects.json", R"({"objects":[
      {"index":0,"name":"STONE","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(root / "textures/skin.png", test::kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(root);
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"GOL_STATUE","nodes":[{"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"IDLE","frames":30,"frameRate":30},
                    {"name":"ACTIVE","frames":10,"frameRate":30}]},
      {"name":"GRU2","nodes":[{"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"READY","frames":30,"frameRate":30}]},
      {"name":"GRUA","nodes":[{"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"READY","frames":30,"frameRate":30}]}
    ]})");
    return root;
}
ImportedActor golem() {
    ImportedActor actor;
    actor.nativeIndex = 24;
    actor.spawn.kind = kGolemEnemyKind;
    actor.spawn.position = {3, .2f, 4};
    actor.spawn.placed = true;
    actor.activationRadius = 16;
    actor.activationHeight = 10;
    actor.floorObject = 2;
    return actor;
}
WorldCollision floor() {
    WorldCollision collision;
    CollisionTriangle triangle;
    triangle.vertices = {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}};
    triangle.normal = {0, 1, 0};
    triangle.object = 2;
    collision.build({triangle});
    const std::array<s32, 1> objects{2};
    collision.setMovingObjects(objects);
    return collision;
}
std::unique_ptr<LegendsSceneAdapter> itScene(test::FakeRenderDevice& device,
                                             const std::filesystem::path& native,
                                             std::string_view name, s32 players = 3) {
    const auto directory = native / "levels" / name;
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    return LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, players);
}
} // namespace

TEST_CASE("Native pending actors keep idle art until proximity and successful live allocation",
          "[legends][legends-actors]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(artwork()));
    auto collision = floor();
    const auto originalTriangles = collision.triangleCount();
    LegendsActors actors;
    std::array roster{golem()};
    actors.open(device, roster, [&](const ImportedActor&) { return &archive; }, &collision);
    REQUIRE(actors.count() == 1);
    CHECK(actors.warnings().empty());
    CHECK(actors.figure(0).sequence() == 0);
    CHECK(collision.triangleCount() == originalTriangles); // no shape5 statue body injected
    actors.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    usize attempts = 0;
    bool accept = false;
    const auto spawn = [&](const ImportedActor& actor) {
        ++attempts;
        CHECK(actor.nativeIndex == 24);
        CHECK(actor.spawn.position == actors.figure(0).position());
        return accept;
    };
    std::array players{
        EnemyView{.player = 0, .position = roster[0].spawn.position + Vec3{17, 0, 0}}};
    actors.update(1.0f / 60, players, false, spawn);
    CHECK(attempts == 0);
    players[0].position.x -= 1;
    actors.update(1.0f / 60, players, true, spawn);
    CHECK(attempts == 0);
    actors.update(1.0f / 60, players, false, spawn);
    CHECK(attempts == 1);
    CHECK(actors.pending(0)); // pool full, do not lose the native placement
    accept = true;
    actors.update(1.0f / 60, players, false, spawn);
    CHECK(attempts == 2);
    CHECK_FALSE(actors.pending(0));
    CHECK(actors.figure(0).sequence() == 0); // no invented ACTIVE delay
    device.draws.clear();
    actors.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty());
    actors.update(1, players, false, spawn);
    CHECK(attempts == 2);
    actors.clear();
    CHECK(actors.count() == 0);
}

TEST_CASE("Pending actor presentation and activation follow only the selected native floor",
          "[legends][legends-actors]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(artwork()));
    auto collision = floor();
    auto actor = golem();
    auto stationary = actor;
    stationary.nativeIndex = 25;
    stationary.floorObject = 7;
    const std::array roster{actor, stationary};
    LegendsActors actors;
    actors.open(device, roster, [&](const ImportedActor&) { return &archive; }, &collision);
    collision.setObjectTransform(2, glm::translate(Mat4{1}, Vec3{0, 5, 0}));
    actors.syncFloors();
    CHECK(actors.descriptor(0).spawn.position == actor.spawn.position + Vec3{0, 5, 0});
    CHECK(actors.figure(0).position() == actors.descriptor(0).spawn.position);
    CHECK(actors.descriptor(1).spawn.position == stationary.spawn.position);
    actors.syncFloors();
    CHECK(actors.descriptor(0).spawn.position == actor.spawn.position + Vec3{0, 5, 0});
    collision.setObjectTransform(2, glm::translate(Mat4{1}, Vec3{0, -3, 0}));
    actors.update(1.0f / 60, {}, true, {});
    CHECK(actors.figure(0).position() == actor.spawn.position + Vec3{0, -3, 0});
}

TEST_CASE(
    "Pending ordinary bodies use corresponding variant artwork and tolerate explicitly missing art",
    "[legends][legends-actors]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(artwork()));
    auto archer = golem();
    archer.spawn.kind = kGruntKind;
    archer.spawn.tier = 4;
    auto bomber = archer;
    bomber.nativeIndex = 25;
    bomber.spawn.tier = 5;
    auto it = archer;
    it.nativeIndex = 26;
    it.spawn.kind = kItKind;
    it.spawn.tier = 1;
    const std::array roster{archer, bomber, it};
    LegendsActors actors;
    actors.open(device, roster, [&](const ImportedActor&) { return &archive; }, nullptr);
    REQUIRE(actors.count() == 3);
    REQUIRE(actors.figure(0).presentation().tree != nullptr);
    CHECK(actors.figure(0).presentation().tree->name == "GRUA");
    REQUIRE(actors.figure(1).presentation().tree != nullptr);
    CHECK(actors.figure(1).presentation().tree->name == "GRU2");
    CHECK_FALSE(actors.figure(2).hasFigure());
    REQUIRE(actors.warnings().size() == 1);
    CHECK(actors.warnings()[0].find("IT") != std::string::npos);
    usize attempts = 0;
    std::array players{EnemyView{.player = 0, .position = it.spawn.position}};
    actors.update(1.0f / 60, players, false, [&](const ImportedActor&) {
        ++attempts;
        return true;
    });
    CHECK(attempts == 3);
}

TEST_CASE("Native pending golems use actual GDL counterpart statues before entering common combat",
          "[legends][legends-actors][assets]") {
    const auto root = test::assetOrSkip("CRITTER").parent_path();
    test::FakeRenderDevice device;
    for (const char costume : std::string_view("ABIGHF")) {
        CAPTURE(costume);
        Critters combat;
        combat.open(device, root, nullptr, {}, costume);
        auto actor = golem();
        actor.costumeRealm = costume;
        const std::array roster{actor};
        LegendsActors actors;
        actors.open(
            device, roster,
            [&](const ImportedActor&) { return combat.archiveFor(CombatantKind::Golem); }, nullptr);
        REQUIRE(actors.count() == 1);
        REQUIRE(actors.warnings().empty());
        REQUIRE(actors.figure(0).hasFigure());
        CHECK(combat.count() == 0);
        CHECK(combat.targets().empty());
        const std::array players{
            EnemyView{.player = 0, .position = actor.spawn.position + Vec3{0, 0, 10}}};
        actors.update(1.0f / 60, players, false, [&](const ImportedActor& ready) {
            return combat.spawnGolem(ready.spawn.position, ready.yaw).has_value();
        });
        CHECK_FALSE(actors.pending(0));
        CHECK(combat.count() == 1);
        CHECK_FALSE(combat.targets().empty());
        actors.clear(); // borrowed artwork is released only after the presenter
        combat.close();
    }
}

TEST_CASE("Every native archer bomber and suicide placement binds and draws its specific variant",
          "[legends][legends-actors][legends-variants][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(native / "_program/game.bin")) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto root = test::assetOrSkip("MONSTERS/TRO").parent_path().parent_path();
    const auto program = readFile(native / "_program/game.bin");
    const auto pgen = readFile(native / "items/generic/pgen.rom");
    std::map<std::pair<s32, s32>, ImportedActor> variants;
    std::array<usize, 3> placements{};
    for (const auto& entry : std::filesystem::directory_iterator(native / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        const auto decoded = decodeLegendsN64Actors(readFile(entry.path() / "ipr.rom"), pgen,
                                                    program, entry.path().filename().string(), {});
        REQUIRE(decoded.warnings.empty());
        for (const auto& actor : decoded.actors) {
            if (actor.spawn.kind < kSwarmKindCount && actor.spawn.tier >= 4) {
                ++placements.at(static_cast<usize>(actor.spawn.tier - 4));
                variants.try_emplace(std::pair{actor.spawn.kind, actor.spawn.tier}, actor);
            }
        }
    }
    CHECK(placements == std::array<usize, 3>{331, 172, 155});
    REQUIRE(variants.size() == 15); // five native species, each with A/B/S artwork
    constexpr std::array kSuffixes{"A", "B", "S"};
    for (const auto& [key, descriptor] : variants) {
        CAPTURE(key.first, key.second, descriptor.nativeIndex);
        test::FakeRenderDevice device;
        Enemies live;
        live.open(device, root, nullptr, 1, {}, 27);
        REQUIRE(live.loadKind(descriptor.spawn.kind));
        const auto expected = std::format("{}{}", enemyKind(descriptor.spawn.kind).prefix,
                                          kSuffixes[static_cast<usize>(descriptor.spawn.tier - 4)]);
        CAPTURE(expected);
        const std::array roster{descriptor};
        LegendsActors pending;
        pending.open(
            device, roster,
            [&](const ImportedActor& actor) { return live.archive(actor.spawn.kind); }, nullptr);
        REQUIRE(pending.warnings().empty());
        REQUIRE(pending.figure(0).hasFigure());
        REQUIRE(pending.figure(0).presentation().tree);
        CHECK(pending.figure(0).presentation().tree->name == expected);
        pending.draw(device, Mat4{1}, WorldLighting{});
        CHECK_FALSE(device.draws.empty());
        const std::array players{
            EnemyView{.player = 0, .position = descriptor.spawn.position + Vec3{0, 0, 12}}};
        std::optional<s32> id;
        pending.update(1.0f / 60, players, false, [&](const ImportedActor& actor) {
            id = live.spawn(actor.spawn, players);
            return id.has_value();
        });
        REQUIRE(id);
        CHECK_FALSE(pending.pending(0));
        CHECK(live.variantOf(*id) == descriptor.spawn.tier);
        CHECK(live.tierOf(*id) == (descriptor.spawn.tier == 6 ? 1 : 2));
        CHECK(live.algorithmOf(*id) == descriptor.spawn.algorithm);
        REQUIRE(live.animatorOf(*id));
        const auto pose = live.animatorOf(*id)->checkpoint();
        REQUIRE(pose);
        CHECK(pose->tree == expected);
        const auto* tree = live.appearanceTree(
            {descriptor.spawn.kind, live.tierOf(*id), descriptor.spawn.tier, false});
        REQUIRE(tree);
        CHECK(tree->name == expected); // not an unnoticed fallback to a generic tier body
        device.draws.clear();
        pending.draw(device, Mat4{1}, WorldLighting{});
        CHECK(device.draws.empty());
        live.draw(device, Mat4{1}, WorldLighting{}, nullptr, nullptr);
        CHECK_FALSE(device.draws.empty());
        if (descriptor.spawn.tier < 6) {
            // The five bomber bodies only author THROW2; the common dispatcher
            // redirects Throw to that windup and releases on entering THROWF.
            REQUIRE((tree->findSequence("THROW1") || tree->findSequence("THROW2")));
            REQUIRE(tree->findSequence("THROWF"));
            EnemyAnimator throwing;
            REQUIRE(throwing.bind(*tree));
            bool released = false;
            for (s32 tick = 0; tick < 120 && !released; ++tick) {
                throwing.request(EnemyAction::Throw);
                throwing.update(1, 1.0f / 60);
                released = throwing.threw();
            }
            REQUIRE(released);
            CHECK(throwing.action() == EnemyAction::ThrowFinish);
            const auto* missile = live.projectileModel(
                descriptor.spawn.kind,
                descriptor.spawn.tier == 4 ? EnemyMissileKind::kArrow : EnemyMissileKind::kBomb);
            REQUIRE(missile);
            CHECK(missile->bound());
        }
        pending.clear();
        live.close();
    }
}

TEST_CASE("All six native It placements retain their own animated billboard artwork",
          "[legends][legends-actors][legends-it][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(native / "it/objects.wad")) {
        SKIP("Native N64 extraction is not installed");
    }
    test::FakeRenderDevice device;
    constexpr std::array kNames{"battle1", "castle3", "mount3", "mount5", "town1", "town2"};
    for (const char* const name : kNames) {
        CAPTURE(name);
        auto scene = itScene(device, native, name);
        auto* archive = scene->actorArchive(kItKind);
        REQUIRE(archive);
        CHECK(scene->actorArchive(kGruntKind) == nullptr);
        const auto found = std::ranges::find_if(
            scene->actors(), [](const auto& actor) { return actor.spawn.kind == kItKind; });
        REQUIRE(found != scene->actors().end());
        CHECK(found->minimumPlayers == 3);
        const auto body = archive->trees.find("IT:WALK");
        REQUIRE(body);
        const auto& tree = archive->trees.tree(*body);
        REQUIRE(tree.nodes.size() == 1);
        CHECK(tree.nodes[0].object == "NEWITCFNSIT");
        REQUIRE(archive->trees.textureAnimations().size() == 1);
        const auto& cycle = archive->trees.textureAnimations().front();
        CHECK(cycle.frames == 15);
        CHECK(cycle.rate == 2); // native rate32: 15 texture frames per second
        CHECK(cycle.source == 1);
        CHECK(cycle.texture == 0);
        LegendsActors actors;
        const std::array roster{*found};
        actors.open(device, roster, [&](const ImportedActor&) { return archive; }, nullptr);
        REQUIRE(actors.warnings().empty());
        REQUIRE(actors.figure(0).hasFigure());
        CHECK(actors.figure(0).presentation().tree == &tree);
        const CameraFrame camera{};
        std::set<const Texture*> frames;
        for (s32 tick = 0; tick < 60; ++tick) {
            actors.update(1.0f / 60, {}, false, {});
            device.draws.clear();
            actors.draw(device, Mat4{1}, WorldLighting{}, &camera);
            REQUIRE_FALSE(device.draws.empty());
            for (const auto& draw : device.draws) {
                CHECK_FALSE(draw.vertices.empty());
                frames.insert(draw.texture);
            }
        }
        CHECK(frames.size() == 15);
    }
    const auto excluded = itScene(device, native, "mount3", 2);
    CHECK(excluded->actorArchive(kItKind) == nullptr);
    CHECK(std::ranges::none_of(excluded->actors(),
                               [](const auto& actor) { return actor.spawn.kind == kItKind; }));
}

TEST_CASE("Native It artwork follows only its live common enemy lifetime and respects time stop",
          "[legends][legends-actors][legends-it][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(native / "it/objects.wad")) {
        SKIP("Native N64 extraction is not installed");
    }
    test::FakeRenderDevice device;
    auto scene = itScene(device, native, "mount3");
    auto* archive = scene->actorArchive(kItKind);
    REQUIRE(archive);
    const auto found = std::ranges::find_if(
        scene->actors(), [](const auto& actor) { return actor.spawn.kind == kItKind; });
    REQUIRE(found != scene->actors().end());
    const std::array roster{*found};
    const auto root = test::scratchDirectory("legends-it-common-bodiless");
    Enemies live;
    live.open(device, root, nullptr, 1, {}, 49);
    REQUIRE(live.loadKind(kItKind));
    LegendsActors actors;
    actors.open(device, roster, [&](const ImportedActor&) { return archive; }, nullptr);
    REQUIRE(actors.warnings().empty());
    std::array players{EnemyView{.player = 2, .position = found->spawn.position + Vec3{0, 0, 12}}};
    std::optional<s32> id;
    const auto spawn = [&](const ImportedActor& actor) {
        id = live.spawn(actor.spawn, players);
        if (id) {
            actors.bindLive(actor.nativeIndex, *id, live.instanceOf(*id));
        }
        return id.has_value();
    };
    actors.update(1.0f / 60, players, false, spawn);
    REQUIRE(id);
    REQUIRE_FALSE(actors.pending(0));
    actors.syncLive(live);
    device.draws.clear();
    live.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty()); // native art supplements, never duplicates a GC body
    actors.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    CHECK(live.targets().empty());
    const auto held = actors.figure(0).checkpoint();
    REQUIRE(held);
    for (s32 tick = 0; tick < 8; ++tick) {
        actors.update(1.0f / 60, players, true, spawn);
        live.update(1, 1.0f / 60, players, {}, nullptr, 1, true);
        actors.syncLive(live);
    }
    const auto stopped = actors.figure(0).checkpoint();
    REQUIRE(stopped);
    CHECK(stopped->animation == held->animation);
    CHECK(stopped->textures.frame == held->textures.frame);
    CHECK(stopped->position == held->position);
    SECTION("tagging removes native art immediately and never respawns the placement") {
        std::vector<s32> tagged;
        bool moved = false;
        for (s32 tick = 0; tick < 600 && tagged.empty(); ++tick) {
            actors.update(1.0f / 60, players, false, spawn);
            live.update(1, 1.0f / 60, players);
            actors.syncLive(live);
            moved |= actors.figure(0).position() != held->position;
            tagged = live.takeTagged();
        }
        CHECK(moved);
        CHECK(tagged == std::vector<s32>{2});
        CHECK_FALSE(live.alive(*id));
        CHECK(live.takeBlows().empty());
        device.draws.clear();
        actors.draw(device, Mat4{1}, WorldLighting{});
        CHECK(device.draws.empty());
        for (s32 tick = 0; tick < 60; ++tick) {
            actors.update(1.0f / 60, players, false, spawn);
            live.update(1, 1.0f / 60, players);
            actors.syncLive(live);
        }
        CHECK(live.takeTagged().empty());
        CHECK_FALSE(actors.pending(0));
    }
    SECTION("a reused pool slot cannot inherit the retired native figure") {
        const u64 original = live.instanceOf(*id);
        live.close();
        live.open(device, root, nullptr, 1, {}, 49);
        REQUIRE(live.loadKind(kItKind));
        const auto replacement = live.spawn(roster[0].spawn, players);
        REQUIRE(replacement);
        CHECK(*replacement == *id);
        CHECK(live.instanceOf(*replacement) != original);
        actors.syncLive(live);
        device.draws.clear();
        actors.draw(device, Mat4{1}, WorldLighting{});
        CHECK(device.draws.empty());
    }
}
