#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/ScriptedAttacks.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct ChimeraTargetingScene {
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;
    std::shared_ptr<LegendsLevel> native;
    // A read-only acquisition query bound to the actual scene's world and
    // opponent/fixture owners. Weapon release below still runs through PlayScene.
    ClassDataSet classes;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    test::ScriptedFixtures fixtureHost;
    test::ScriptedAttacks attackHost;

    ChimeraTargetingScene() {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels/castle5";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Chimera arena is not installed");
        }
        const auto root = test::assetOrSkip("PLAYERS").parent_path();
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                                 readFile(directory / "ipr.rom"));
        for (auto& texture : formats::decodeLegendsN64Textures(objects)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency});
        }
        REQUIRE(map.destructibles.size() == 3);
        REQUIRE(map.objects[map.destructibles[0].object].name == "SHOOTW_1");
        REQUIRE(map.objects[map.destructibles[1].object].name == "SHOOTW_3");
        REQUIRE(map.objects[map.destructibles[2].object].name == "SHOOTW_2");
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE(adapter);
        PartyMember member;
        member.player = 0;
        member.save.name = "AIM";
        member.save.progress().health = 2000;
        const std::array party{member};
        LegendsTraversal traversal;
        traversal.open(map, 0, member.save);
        native = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
        native->initializeParty(party);
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.name = "castle5";
        const bool loaded =
            world.loadImported(device, root, ref, std::move(layout), traversal.collision(), native);
        REQUIRE(loaded);
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        context.config = &config;
        context.strings = &strings;
        context.unpackedRoot = root;
        PlayOptions options;
        options.position = traversal.start();
        options.yaw = traversal.actor().yaw();
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        scene.setCheats(0, {.god = true});
        REQUIRE(classes.load(root / "PDATA"));
        arsenal.bind({device,
                      classes,
                      weapons,
                      world.collision(),
                      effects,
                      audio,
                      nullptr,
                      {},
                      false,
                      true});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
    }

    void step(const PlayScene::Inputs& input = {}) {
        REQUIRE(scene.update(1.0 / 60, input) == PlayOutcome::Running);
    }

    std::optional<Vec3> floor(f32 x, f32 z) const {
        Vec3 at{x, world.layout().maxBounds().y + 10, z};
        if (const auto support = world.collision().floorAt(at, 0, 300)) {
            at.y = support->y;
            return at;
        }
        return {};
    }

    PlayerAttacks::Targets targets() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& opponents = const_cast<LevelOpponents&>(scene.opponents());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& fixtures = const_cast<LevelFixtures&>(scene.fixtures());
        return {.opponents = opponents,
                .fixtures = fixtures,
                .fixtureHost = fixtureHost,
                .attackHost = attackHost,
                .multiplayer = MultiplayerMode::Normal,
                .players = scene.participants()};
    }

    std::vector<MissileTarget> pillars() const {
        std::vector<MissileTarget> result;
        for (const auto& target : native->targets()) {
            if (target.id >= kImportedSceneryBase) {
                result.push_back({kImportedTargetBase + target.id, target.base, target.radius,
                                  target.height, target.surface});
            }
        }
        return result;
    }

    void ready() {
        for (s32 tick = 0; tick < 1200 && (scene.spawning() || scene.animator(0)->entering() ||
                                           !native->opponentsReady());
             ++tick) {
            step();
        }
        REQUIRE_FALSE(scene.spawning());
        REQUIRE_FALSE(scene.animator(0)->entering());
        REQUIRE(native->opponentsReady());
        const auto standing = floor(10, 0);
        REQUIRE(standing);
        REQUIRE(scene.relocate(*standing));
        for (s32 tick = 0; tick < 600 && !scene.bosses().view().awake; ++tick) {
            step();
        }
        REQUIRE(scene.bosses().view().awake);
    }

    f32 headHealth() const {
        f32 total = 0;
        for (s32 id = 1; id <= 3; ++id) {
            const auto* head = scene.bosses().fighter().child(id);
            REQUIRE(head);
            total += head->health();
        }
        return total;
    }

    struct Reproduction {
        Vec3 feet;
        Vec3 direction;
        Vec3 ordinary;
        Vec3 boss;
    };

    Reproduction reproduce() {
        const auto obstacles = pillars();
        REQUIRE(obstacles.size() == 3);
        auto all = obstacles;
        auto heads = scene.bosses().targets();
        for (auto& head : heads) {
            head.id += 8000; // Distinct from imported scenery; only query grouping matters.
        }
        all.insert(all.end(), heads.begin(), heads.end());
        s32 grounded = 0;
        s32 eligible = 0;
        // Find a real supported player position with a visible boss candidate
        // and a nearer pillar in the same controller cone. No posed boss nodes,
        // synthetic floor, or deleted pillar geometry enters this reproduction.
        for (const f32 z : {20.0f, 15.0f, 10.0f, 5.0f, 0.0f}) {
            for (const f32 x : {10.0f, -10.0f, 15.0f, -15.0f, 20.0f, -20.0f, 0.0f}) {
                const auto feet = floor(x, z);
                if (!feet) {
                    continue;
                }
                ++grounded;
                REQUIRE(scene.relocate(*feet));
                const auto& actor = *scene.actor(0);
                const Vec3 toward = *scene.bosses().position() - actor.position();
                const Vec3 direction = glm::normalize(Vec3{toward.x, 0, toward.z});
                const auto select = [&](std::span<const MissileTarget> candidates) {
                    return TargetAssist::select(actor.followPoint(), direction, candidates,
                                                TargetAssist::kBossRange, &world.collision());
                };
                const auto boss = select(heads);
                const auto old = select(all);
                if (!boss || !old) {
                    continue;
                }
                ++eligible;
                if (glm::distance(*boss, *old) > 5 &&
                    std::ranges::any_of(obstacles, [&](const auto& pillar) {
                        return glm::distance(pillar.pointNear(actor.followPoint()), *old) < .001f;
                    })) {
                    return {actor.position(), direction, *old, *boss};
                }
            }
        }
        CAPTURE(grounded, eligible);
        FAIL("No actual castle5 pillar/boss acquisition conflict reproduced");
        return {};
    }
};
} // namespace

TEST_CASE("Native Chimera ranged acquisition does not dive at nearer breakable pillars",
          "[legends][native-chimera-targeting][assets]") {
    ChimeraTargetingScene test;
    test.ready();
    const auto original = test.reproduce();
    const auto& actor = *test.scene.actor(0);
    const auto selected = test.attacks.aim(actor, original.direction, test.targets());
    CAPTURE(original.feet.x, original.feet.y, original.feet.z, original.ordinary.x,
            original.ordinary.y, original.ordinary.z, original.boss.x, original.boss.y,
            original.boss.z);
    REQUIRE(selected);
    CHECK(glm::distance(*selected, original.boss) < .001f);
    CHECK(glm::distance(*selected, original.ordinary) > 5);

    const f32 health = test.headHealth();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<PlayerActor&>(actor).turnTo(std::atan2(original.direction.x, original.direction.z));
    PlayScene::Inputs held;
    held[0].attack = true; // Controller path: no cursor height-only override.
    bool fired = false;
    bool upward = false;
    for (s32 tick = 0; tick < 600 && test.headHealth() == health; ++tick) {
        test.step(held);
        for (usize i = 0; i < test.scene.missiles().count(); ++i) {
            const auto& shot = test.scene.missiles().missile(i);
            if (shot.owner == 0 && shot.age < .05f) {
                fired = true;
                upward |= shot.velocity.y > 0;
            }
        }
    }
    CHECK(fired);
    CHECK(upward);
    CHECK(test.headHealth() < health);
    for (const usize pillar : {1U, 2U, 4U}) {
        CHECK(test.native->adapter().worldObjectVisible(pillar));
    }
}

TEST_CASE("Native boss preference retains ordinary fallback and fixed cursor bearing",
          "[legends][native-chimera-targeting][assets]") {
    ChimeraTargetingScene test;
    test.ready();
    const auto original = test.reproduce();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& actor = const_cast<PlayerActor&>(*test.scene.actor(0));
    actor.turnTo(std::atan2(original.direction.x, original.direction.z));
    SECTION("a wall blocks the preferred boss but not the nearer pillar") {
        // Keep every native floor/pillar and append an independent sight blocker
        // beyond them. The preference must not shoot through that wall.
        const std::array wall{
            CollisionTriangle{{0, 0, 1},
                              {Vec3{-100, -100, -35}, Vec3{100, -100, -35}, Vec3{100, 150, -35}},
                              -1,
                              2},
            CollisionTriangle{{0, 0, 1},
                              {Vec3{-100, -100, -35}, Vec3{100, 150, -35}, Vec3{-100, 150, -35}},
                              -1,
                              2}};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        const_cast<WorldCollision&>(test.world.collision()).append(wall);
        CHECK_FALSE(TargetAssist::select(actor.followPoint(), original.direction,
                                         test.scene.bosses().targets(), TargetAssist::kBossRange,
                                         &test.world.collision()));
        const auto selected = test.attacks.aim(actor, original.direction, test.targets());
        REQUIRE(selected);
        CHECK(glm::distance(*selected, original.ordinary) < .001f);
    }
    SECTION("absent boss falls back to physically targetable scenery") {
        test.scene.bosses().close();
        REQUIRE_FALSE(test.scene.bosses().present());
        const auto selected = test.attacks.aim(actor, original.direction, test.targets());
        REQUIRE(selected);
        CHECK(glm::distance(*selected, original.ordinary) < .001f);
    }
    SECTION("outside the boss facing cone still aims at the nearer pillar") {
        const auto pillars = test.pillars();
        const auto heads = test.scene.bosses().targets();
        std::optional<Vec3> ordinary;
        Vec3 direction{0};
        // The arena's back edge is not a clear sightline to every pillar. Find
        // a supported side approach with a visible pillar and every boss NODE
        // outside the cone, independently of the imported priority policy.
        for (const f32 z : {-15.f, -10.f, -5.f, 0.f, 5.f, 10.f, 15.f}) {
            for (const f32 x : {10.f, -10.f, 15.f, -15.f, 20.f, -20.f, 0.f}) {
                const auto feet = test.floor(x, z);
                if (!feet) {
                    continue;
                }
                REQUIRE(test.scene.relocate(*feet));
                for (const auto& pillar : pillars) {
                    const Vec3 toward = pillar.pointNear(actor.followPoint()) - actor.followPoint();
                    const Vec3 flat{toward.x, 0, toward.z};
                    if (glm::length(flat) < .001f) {
                        continue;
                    }
                    direction = glm::normalize(flat);
                    const bool inCone = std::ranges::any_of(heads, [&](const auto& head) {
                        const Vec3 point = head.acquisition ? head.acquisition->point
                                                            : head.pointNear(actor.followPoint());
                        const Vec3 offset = point - actor.followPoint();
                        const Vec3 bearing{offset.x, 0, offset.z};
                        return glm::length(bearing) > .001f &&
                               glm::dot(glm::normalize(bearing), direction) >=
                                   TargetAssist::kFacingDot;
                    });
                    if (!inCone) {
                        ordinary =
                            TargetAssist::select(actor.followPoint(), direction, pillars,
                                                 TargetAssist::kBossRange, &test.world.collision());
                    }
                    if (ordinary) {
                        break;
                    }
                }
                if (ordinary) {
                    break;
                }
            }
            if (ordinary) {
                break;
            }
        }
        REQUIRE(ordinary);
        actor.turnTo(std::atan2(direction.x, direction.z));
        CHECK_FALSE(TargetAssist::select(actor.followPoint(), direction, heads,
                                         TargetAssist::kBossRange, nullptr));
        const auto selected = test.attacks.aim(actor, direction, test.targets());
        REQUIRE(selected);
        CHECK(glm::distance(*selected, *ordinary) < .001f);
    }
    SECTION("cursor targeting changes elevation without rotating its firing line") {
        auto weapon = test.arsenal.weaponThrow(actor, 0);
        REQUIRE(weapon);
        weapon->direction = original.direction;
        weapon->fixedFacing = true;
        weapon->target = test.attacks.aim(actor, original.direction, test.targets(), true);
        REQUIRE(weapon->target);
        const auto launches = PlayerMissiles::weaponLaunches(*weapon);
        REQUIRE(launches.size() == 1);
        REQUIRE(launches.front().velocity);
        const Vec3 velocity = *launches.front().velocity;
        CHECK(std::abs(velocity.x * original.direction.z - velocity.z * original.direction.x) <
              .001f);
        const Vec3 away = -original.direction;
        CHECK_FALSE(TargetAssist::select(actor.followPoint(), away, test.scene.bosses().targets(),
                                         TargetAssist::kBossRange, &test.world.collision(),
                                         TargetAssist::kFacingDot, true));
    }
}

TEST_CASE("Native Chimera pillars remain manually breakable by actual scene melee",
          "[legends][native-chimera-targeting][assets]") {
    ChimeraTargetingScene test;
    test.ready();
    test.scene.setCheats(0, {.god = true, .noTarget = true});
    // The native sloping front face is about z=-2.5 at body height. From
    // z=0 its 2.561-unit surface distance is inside a held swing (3.5),
    // while the previous z=4 position correctly chose a ranged attack.
    const auto feet = test.floor(0, 0);
    REQUIRE(feet);
    REQUIRE(test.scene.relocate(*feet));
    REQUIRE(test.native->adapter().worldObjectVisible(2)); // SHOOTW_3, the centre pillar.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& actor = const_cast<PlayerActor&>(*test.scene.actor(0));
    actor.turnTo(std::atan2(0.f, -1.f));
    const auto pillars = test.pillars();
    REQUIRE(pillars.size() == 3);
    const Vec3 actual = actor.position();
    const Vec3 closest = pillars[1].pointNear(actor.followPoint());
    const f32 distance = TargetAssist::distanceTo(actual, actor.height(), pillars[1]);
    const auto sense = test.attacks.meleeSense(actor, true, test.targets(), Vec3{0, 0, -1});
    CAPTURE(feet->x, feet->y, feet->z, actual.x, actual.y, actual.z, actor.radius(), actor.height(),
            actor.reach(), pillars[1].base.x, pillars[1].base.y, pillars[1].base.z,
            pillars[1].height, pillars[1].radius, closest.x, closest.y, closest.z, distance,
            static_cast<s32>(sense.range));
    REQUIRE(
        TargetAssist::around(actor.position(), actor.height(), std::span{&pillars[1], 1},
                             actor.reach() + PlayerAttacks::kSwingReach + PlayerAttacks::kHeldReach,
                             &test.world.collision()));
    REQUIRE(sense.range == MeleeRange::Swing);
    PlayScene::Inputs attack;
    attack[0].attack = true;
    attack[0].aimDirection = Vec3{0, 0, -1};
    bool swung = false;
    bool fired = false;
    for (s32 tick = 0; tick < 600 && test.native->adapter().worldObjectVisible(2); ++tick) {
        test.step(attack);
        swung |= test.scene.animator(0)->meleeing();
        fired |= test.scene.missiles().count() != 0;
    }
    CHECK(swung);
    CHECK_FALSE(fired);
    CHECK_FALSE(test.native->adapter().worldObjectVisible(2));
    CHECK(test.native->adapter().worldObjectVisible(1));
    CHECK(test.native->adapter().worldObjectVisible(4));
    CHECK(test.pillars().size() == 2);
}
