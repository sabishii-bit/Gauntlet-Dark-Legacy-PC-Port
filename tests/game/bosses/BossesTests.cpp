#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/ApproximateLength.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/bosses/Bosses.h"
#include "game/bosses/LegendProfile.h"
#include "game/critters/CombatantAssets.h"
#include "game/critters/CombatantFixture.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/level/BossKinds.h"
#include "game/level/LevelTriggers.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

EnemyView playerAt(const Vec3& position, s32 player = 0) {
    EnemyView view;
    view.player = player;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

/** A synthetic boss archive for `name` of TYPE flags `typeFlags`: a body with an EYEBALL node,
 * and the plague fiend's replacement eye mesh beside it, so the legend's eye can be checked
 * without game data. */
std::filesystem::path eyedBossAssets(const std::string& name, u32 typeFlags = 0) {
    const auto root = test::scratchDirectory(std::format("eyed-boss-{}-{}", name, typeFlags));
    std::filesystem::create_directories(root / "critter");
    const auto archive = root / "MONSTERS" / name;
    std::filesystem::create_directories(archive / "models");
    std::filesystem::create_directories(archive / "textures");
    writeTextFile(archive / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1},
      {"index":1,"name":"PBOSSQEYEBALL","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(archive / "textures/skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
               {"name":"EYEBALL","object":"BODY","parent":0,"position":[0,5,0]}],
      "sequences":[{"name":"READY","frames":30}]}]})");
    writeTextFile(root / "critter" / (name + ".json"),
                  std::format(R"({{"descriptors":[{{"prefix":"BODY","name":"{}","type":4}}],
      "types":[{{"moveCount":1,"maxHealth":1000,"radius":3,"expValue":500,"typeFlags":{},
                 "lookNode0":""}}],
      "moves":[{{"name":"READY","anim":"READY","type":32,"priority":512,"interrupt":20}}]}})",
                              name, typeFlags));
    return root;
}

/** CHIMERA's TYPE flags (0x82): aimed at by parts, and asleep it arms its wake only once its
 * floor's trigger moves it. */
constexpr u32 kChimeraTypeFlags = CritterData::kByParts | CritterData::kSupportActivated;

/**
 * A lift for a boss to stand on, A5's elevator in small. LIFT (object 0) at (0, 10, 0) carries
 * DECK (1) two up; beside the deck lies LIFT's own face, and STILL ground (2) lies thirty under
 * the lift. The pad at kPad, of trigger `subtype`, lowers LIFT by twenty at four a second.
 * The collision follows the scene once the triggers have moved it, as LevelWorld places it.
 */
struct LiftFixture {
    static constexpr s32 kLift = 0;
    static constexpr s32 kDeck = 1;
    static constexpr s32 kStill = 2;
    static constexpr Vec3 kPad{-30, 0, 0};
    static constexpr Vec3 kOnDeck{0, 12, 0};
    static constexpr Vec3 kOnLiftFace{30, 10, 0};
    static constexpr f32 kTravel = 20;

    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    WorldAnimator animator;
    LevelTriggers triggers;
    WorldCollision collision;

    explicit LiftFixture(s32 subtype) {
        const auto dir = test::sampleLevel(std::format("boss-lift-{}", subtype));
        // The pad's open endpoint is -200 tenths (bytes 56, 255).
        writeTextFile(dir / "world.json", std::format(R"({{
          "objects":[{{"name":"WALL","position":[0,10,0],"flags":4096,"child":1,"next":2}},
                     {{"name":"WALL","position":[0,2,0],"flags":4096}},
                     {{"name":"WALL","position":[0,-20,0]}}],
          "itemInfos":[{{"type":5,"subtype":{},"name":"BRIDGEPAD","radius":1}}],
          "itemInstances":[{{"info":0,"position":[{},{},{}],
                            "params":[0,0,0,0,2,255,0,0,0,0,56,255]}}]}})",
                                                      subtype, kPad.x, kPad.y, kPad.z));
        REQUIRE(layout.load(dir));
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
        animator.bind(layout);
        std::vector<CollisionTriangle> faces;
        floor(faces, kLift, 20, 40, 0); // local to the lift, as moving objects keep theirs
        floor(faces, kDeck, -10, 10, 0);
        floor(faces, kStill, -100, 100, -20); // in world space
        std::vector<s32> parents;
        for (const WorldObject& object : layout.objects()) {
            parents.push_back(object.parent);
        }
        collision.build(std::move(faces), parents);
        collision.setMovingObjects(std::array{kLift, kDeck});
        place();
        triggers.bind(layout, animator, &collision);
        triggers.openMet({}, animator, scene, &collision);
        place();
    }
    /** A floor of `object` over x from `from` to `to` and z within ten, at height `y`. */
    static void floor(std::vector<CollisionTriangle>& faces, s32 object, f32 from, f32 to, f32 y) {
        CollisionTriangle half;
        half.object = object;
        half.vertices = {Vec3{from, y, -10}, Vec3{to, y, 10}, Vec3{to, y, -10}};
        faces.push_back(half);
        half.vertices = {Vec3{from, y, -10}, Vec3{from, y, 10}, Vec3{to, y, 10}};
        faces.push_back(half);
    }
    void place() {
        for (const s32 object : {kLift, kDeck}) {
            collision.setObjectTransform(object, scene.worldTransform(static_cast<usize>(object)));
        }
    }
    /** The triggers' turn in a frame, after the opponents', with `visitors` on them. */
    void step(f32 seconds, std::span<const TriggerVisitor> visitors = {}) {
        triggers.update(seconds, visitors, animator, scene, &collision);
        place();
    }
    f32 descent() const { return 10 - scene.worldTransform(static_cast<usize>(kLift))[3].y; }
};

TEST_CASE("Wraith waits in its lowered entrance pose and rises only in START3",
          "[game][enemies][wraith][assets]") {
    const auto root = test::assetOrSkip("CRITTER/WRAITH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/WRAITH/ANIM.PS2");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'J');
    REQUIRE(bosses.spawn(40, Vec3{0}, 0));
    REQUIRE(bosses.moveName() == "START");
    const auto initial = bosses.nodeTransform("ROOT");
    REQUIRE(initial.has_value());
    // The negative key cancels ROOT's 24.9408-unit bind offset above L1ROOT.
    REQUIRE((*initial)[3].y == Approx(4.020784f));
    const std::vector<EnemyView> far{playerAt({0, 0, 100})};
    bosses.update(kTicks, kStep, far);
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.nodeTransform("ROOT") == initial);

    const std::vector<EnemyView> near{playerAt({0, 0, 40})};
    bool second = false;
    bool third = false;
    bool risen = false;
    for (s32 frame = 0; frame < 450 && !risen; ++frame) {
        bosses.update(kTicks, kStep, near);
        const auto pose = bosses.nodeTransform("ROOT");
        REQUIRE(pose.has_value());
        if (bosses.moveName() == "START" || bosses.moveName() == "START2") {
            REQUIRE((*pose)[3].y == Approx((*initial)[3].y));
            second |= bosses.moveName() == "START2";
        } else if (bosses.moveName() == "START3") {
            third = true;
            risen |= (*pose)[3].y > (*initial)[3].y + 10.0f;
        }
        bosses.takeCues();
    }
    REQUIRE(second);
    REQUIRE(third);
    REQUIRE(risen);
}

TEST_CASE("Wraith health and range windows expose every authored attack family",
          "[game][enemies][wraith][assets]") {
    const auto root = test::assetOrSkip("CRITTER/WRAITH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/WRAITH/ANIM.PS2");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'J');
    REQUIRE(fixture.spawn("WRAITH", {0, -15.9375f, -35}, 0));
    std::set<std::string> attacks;
    std::set<s32> projectiles;
    bool rootLaunch = false;
    for (const f32 health : {1.0f, 0.7f, 0.4f, 0.15f}) {
        for (const Vec3 offset :
             {Vec3{0, 0, 15}, Vec3{0, 0, 45}, Vec3{0, 0, 70}, Vec3{35, 0, 35}, Vec3{-35, 0, 35}}) {
            REQUIRE(
                fixture.actor.spawn(fixture.assets, 0, {0, -15.9375f, -35}, 0, nullptr, {}, 'J'));
            if (health < 1) {
                EnemyHit hit;
                hit.damage = fixture.actor.maxHealth() * (1 - health);
                fixture.actor.hurt(hit);
            }
            const std::vector<EnemyView> players{playerAt(Vec3{0, 0, -35} + offset)};
            for (s32 frame = 0; frame < 7200; ++frame) {
                fixture.update(2, kStep, players);
                if (fixture.actor.moveType() >= 128) {
                    attacks.emplace(fixture.actor.moveName());
                }
                for (const auto& shot : fixture.actor.takeShots()) {
                    projectiles.insert(shot.damageIndex);
                    if (shot.damageIndex == 10) {
                        REQUIRE(shot.origin.y == Approx(1.0625f));
                        rootLaunch = true;
                    }
                }
                fixture.actor.takeCues();
                fixture.actor.takeBlows();
            }
            REQUIRE(glm::length(Vec2{fixture.actor.position().x,
                                     fixture.actor.position().z + 35}) <= 3.001f);
        }
    }
    REQUIRE(attacks == std::set<std::string>{"SWIPEL", "SWIPER", "BLENDER", "SPINBLADE", "RSTRETCH",
                                             "LSTRETCH", "THROW", "PECKERSHOT", "SNAKES", "GBALL",
                                             "BOLT"});
    REQUIRE(projectiles == std::set<s32>{8, 10, 11, 12});
    REQUIRE(rootLaunch);
}

TEST_CASE("the bosses are named by kind, from the dragon to the garm", "[game][enemies]") {
    REQUIRE(bossNameOf(41) == "LICH");
    REQUIRE(bossNameOf(34) == "DRAGON");
    REQUIRE(bossNameOf(44) == "GARM");
    REQUIRE(bossNameOf(-1).empty());
    REQUIRE(bossNameOf(33).empty());
    BossView none;
    REQUIRE(none.fraction() == 0.0f);
    none.health = 750.0f;
    none.maxHealth = 3000.0f;
    REQUIRE(none.fraction() == Approx(0.25f));
}

TEST_CASE("an absent boss has no height or camera offset", "[game][enemies]") {
    Bosses bosses;
    REQUIRE(bosses.height() == 0.0f);
    REQUIRE(bosses.cameraOffset() == Vec3{0.0f});
    bosses.close();
    REQUIRE(bosses.height() == 0.0f);
    REQUIRE(bosses.cameraOffset() == Vec3{0.0f});
}

TEST_CASE("arena mesh cues belong only to the Lich and Garm", "[game][enemies][boss-effects]") {
    REQUIRE(bossArenaObject(41) == "G5BIGDIRT");
    REQUIRE(bossArenaObject(44) == "H4NSFFXL_PURPLE");
    for (s32 kind = -1; kind <= 45; ++kind) {
        if (kind != 41 && kind != 44) {
            REQUIRE(bossArenaObject(kind).empty());
        }
    }
}

TEST_CASE("a boss sleeps until the party comes near, then fights by its table, and its "
          "meter follows its health to the end",
          "[game][enemies][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.health = 0.5f;
    bosses.open(device, root, nullptr, scales, 'G');
    REQUIRE_FALSE(bosses.present());
    REQUIRE_FALSE(bosses.spawn(33, Vec3{0.0f, 0.0f, 0.0f}, 0.0f)); // no boss of that kind
    REQUIRE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 20.0f));
    REQUIRE_FALSE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f)); // one to a level
    REQUIRE(bosses.present());
    BossView view = bosses.view();
    REQUIRE(view.kind == 41);
    REQUIRE(view.name == "LICH");
    REQUIRE(view.maxHealth == 1500.0f); // three thousand at the level's half
    REQUIRE(view.health == 1500.0f);
    REQUIRE(view.fraction() == 1.0f);
    REQUIRE(view.alive);
    REQUIRE_FALSE(view.awake);
    // Six authored hit nodes plus the origin-offset body volume, not a cylinder
    // at the floor anchor. Keep the node identity used by missile damage routing.
    const auto targets = bosses.targets();
    REQUIRE(targets.size() == 7);
    for (usize i = 0; i < 6; ++i) {
        CHECK(targets[i].id == Bosses::kTargetId);
        CHECK(targets[i].node == static_cast<s32>(i));
    }
    CHECK(targets.back().node == -1);
    // Its meter is two strips with backgrounds, capped 44 on the left and 53 on the right,
    // drawn from its own archive.
    const HealthMeterDefinition* meter = bosses.meter();
    REQUIRE(meter != nullptr);
    REQUIRE(meter->shown);
    REQUIRE(meter->backed);
    REQUIRE(meter->pieces == 2);
    REQUIRE(meter->advance == 256);
    REQUIRE(meter->leftInset == 44);
    REQUIRE(meter->rightInset == 53);
    REQUIRE(meter->barOffset == Vec3{0.0f});
    REQUIRE(bosses.archive() != nullptr);
    REQUIRE(bosses.archive()->textures.find("METER_FG1").has_value());
    // Beyond its threshold it sleeps: nothing moves.
    const std::vector<EnemyView> far{playerAt(Vec3{0.0f, 0.0f, 40.0f})};
    for (s32 i = 0; i < 120; ++i) {
        bosses.update(kTicks, kStep, far);
    }
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.moveName() == "START");
    REQUIRE(bosses.position()->z == 0.0f);
    // Within it, it wakes, makes its entrance and comes for the player.
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 18.0f})};
    bosses.update(kTicks, kStep, near);
    REQUIRE(bosses.view().awake);
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 1500 && blows.empty(); ++i) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE_FALSE(blows.empty());
    REQUIRE(blows[0].player == 0);
    REQUIRE(bosses.position()->z > 5.0f);
    // Sounds and visuals start together at sfxFrame; the glow's own sequence contains
    // the wind-up. Root-attached effects follow the full transform, not just translation.
    const std::vector<CombatCue> cues = bosses.takeCues();
    bool entrance = false;
    bool swingSound = false;
    bool swingGlow = false;
    for (const CombatCue& cue : cues) {
        entrance = entrance || (cue.tree == "GENFX" && cue.sound == "S_LICHENT");
        if (cue.sound.starts_with("S_LICHATK")) {
            swingSound = true;
        }
        if (cue.tree.starts_with("ATK")) {
            swingGlow = true;
            REQUIRE(cue.follows);
            REQUIRE(cue.node == "GROUP1");
            REQUIRE_FALSE(cue.rootAttachment);
        }
    }
    REQUIRE(entrance);
    REQUIRE(swingSound);
    REQUIRE(swingGlow);
    // Struck, the meter falls; the blow pays its share; killed, a fifth of its value goes to
    // everyone, and once its death has played it is gone.
    EnemyHit hit;
    hit.damage = 300.0f;
    hit.player = 0;
    bosses.hurt(hit);
    view = bosses.view();
    REQUIRE(view.health == Approx(1500.0f - 300.0f + 1.0f)); // its armour of one
    REQUIRE(view.fraction() == Approx(view.health / 1500.0f));
    auto losses = bosses.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].player == 0);
    REQUIRE(losses[0].experience > 0.0f);
    EnemyHit slay;
    slay.damage = 5000.0f;
    slay.player = 0;
    bosses.hurt(slay);
    losses = bosses.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[1].killed);
    REQUIRE(losses[1].player == -1);
    REQUIRE(losses[1].experience == Approx(0.2f * 3860.0f));
    REQUIRE_FALSE(bosses.view().alive);
    REQUIRE(bosses.view().fraction() == 0.0f);
    s32 gone = 0;
    while (bosses.present() && gone < 900) {
        bosses.update(kTicks, kStep, near);
        ++gone;
    }
    REQUIRE_FALSE(bosses.present());
    REQUIRE(gone > 10);
}

TEST_CASE("sustained damage cannot lock the Lich in its roar reaction",
          "[game][enemies][lich][assets]") {
    const auto root = test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.health = 100.0f;
    bosses.open(device, root, nullptr, scales, 'G');
    REQUIRE(bosses.spawn(41, Vec3{0}, 0, 20));
    REQUIRE(bosses.rewardOffset() == Vec3{0, 10, 0});
    const std::vector<EnemyView> near{playerAt({0, 0, 19.5f})}; // strictly inside his twenty
    // Two seconds asleep, then the whole of his entrance, which nothing may cut.
    for (s32 frame = 0; frame < 400; ++frame) {
        bosses.update(kTicks, kStep, near);
    }
    s32 roars = 0;
    bool attackedAfterRoar = false;
    std::string previous;
    for (s32 frame = 0; frame < 600; ++frame) {
        if (frame % 6 == 0) {
            EnemyHit hit;
            hit.damage = 50.0f;
            bosses.hurt(hit);
        }
        bosses.update(kTicks, kStep, near);
        const std::string move{bosses.moveName()};
        if (move == "ROAR" && previous != move) {
            ++roars;
        }
        attackedAfterRoar |=
            roars > 0 && (move.starts_with("AXE") || move.starts_with("SPIKE") || move == "CHAIN" ||
                          move == "STOMP" || move == "HEADTOSS" || move == "CHARGE" ||
                          move == "SPIT" || move == "FSTEPF");
        previous = move;
        bosses.takeCues();
    }
    REQUIRE(bosses.view().alive);
    REQUIRE(roars == 1);
    REQUIRE(attackedAfterRoar);
}

TEST_CASE("a legend item brought to the boss is thrown as it rises and takes its toll",
          "[game][enemies][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("CRITTER/LICH.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/LICH/ANIM.PS2");
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.health = 0.5f;
    bosses.open(device, root, nullptr, scales, 'G');
    REQUIRE_FALSE(bosses.bringLegend(0)); // no boss yet
    REQUIRE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 20.0f));
    // The lich's item is the town's, the book of protection.
    REQUIRE(bosses.legendRealm() == 7);
    REQUIRE(bosses.bringLegend(0));
    REQUIRE_FALSE(bosses.bringLegend(1)); // one rite
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Carried);
    // Asleep, nothing happens.
    const std::vector<EnemyView> far{playerAt(Vec3{0.0f, 0.0f, 40.0f})};
    for (s32 i = 0; i < 60; ++i) {
        bosses.update(kTicks, kStep, far);
    }
    REQUIRE(bosses.takeLegendEvents().empty());
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Carried);
    // Awake, it rises, and once risen the item goes up and is thrown: a quarter of its
    // health goes at once, less its armour of one, paid as a hit to the bearer.
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 18.0f})};
    std::vector<LegendEvent> events;
    s32 waited = 0;
    while (!bosses.legend().thrown() && waited < 3000) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeLegendEvents();
        events.insert(events.end(), taken.begin(), taken.end());
        ++waited;
    }
    REQUIRE(bosses.legend().thrown());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].cue == LegendCue::Brandished);
    REQUIRE(events[0].player == 0);
    REQUIRE(events[0].realm == 7);
    REQUIRE(events[1].cue == LegendCue::Thrown);
    REQUIRE(bosses.view().health == Approx(1500.0f - (0.25f * 1500.0f - 1.0f)));
    auto losses = bosses.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].player == 0);
    REQUIRE_FALSE(bosses.frozen());
    REQUIRE_FALSE(bosses.curbed());
    // It roars at that, and the rite is over: the book's toll is paid once.
    bool roared = false;
    for (s32 i = 0; i < 600 && !roared; ++i) {
        bosses.update(kTicks, kStep, near);
        for (const LegendEvent& event : bosses.takeLegendEvents()) {
            roared = roared || event.cue == LegendCue::Roared;
        }
    }
    REQUIRE(roared);
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Over);
    REQUIRE_FALSE(bosses.legend().running());
    // Then it fights.
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 1500 && blows.empty(); ++i) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE_FALSE(blows.empty());
    bosses.close();
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::None);
}

TEST_CASE("the Plague javelin harms on impact and blindness ends 29 seconds after its roar",
          "[game][legend][plague][native-assets]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'K');
    REQUIRE(bosses.spawn(38, Vec3{0}, 0, 100));
    CHECK(bosses.legendTargetTransform() == bosses.nodeTransform("BODY1_EYEBALL"));
    REQUIRE(bosses.bringLegend(0));
    const auto checkEye = [&](std::string_view expected, std::string_view absent) {
        device.draws.clear();
        bosses.draw(device, Mat4{1}, {});
        const auto actual = device.draws;
        REQUIRE_FALSE(actual.empty());
        const auto transform = bosses.nodeTransform("EYEBALL");
        REQUIRE(transform.has_value());
        for (const auto name : {expected, absent}) {
            auto* archive = bosses.archive();
            REQUIRE(archive != nullptr);
            REQUIRE(archive->models.find(name).has_value());
            TreeInfo tree;
            TreeNodeInfo node;
            node.object = name;
            tree.nodes.push_back(node);
            TreeModel eye;
            REQUIRE(eye.bind(tree, archive->models, archive->textures, device));
            device.draws.clear();
            eye.draw(device, Mat4{1}, *transform);
            REQUIRE_FALSE(device.draws.empty());
            for (const auto& draw : device.draws) {
                const bool drawn = std::ranges::any_of(actual, [&](const auto& candidate) {
                    return candidate.texture == draw.texture && candidate.vertices == draw.vertices;
                });
                CHECK(drawn == (name == expected));
            }
        }
    };
    checkEye("PBOSSEYEBALL", "PBOSSQEYEBALL");
    const std::vector<EnemyView> party{playerAt({0, 0, 40})};
    const f32 originalHealth = bosses.view().health;
    bosses.landLegend(); // A stale notification before the gesture cannot spend the item.
    CHECK(bosses.view().health == originalHealth);
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(kTicks, kStep, party);
    }
    REQUIRE(bosses.legend().thrown());
    CHECK(bosses.view().health == originalHealth);
    CHECK_FALSE(bosses.blinded());
    // Release and travel are separate from the gesture request. Even a completed
    // roar cannot discard the projectile's pending impact.
    bool roared = false;
    for (s32 frame = 0; frame < 300 && !roared; ++frame) {
        bosses.update(kTicks, kStep, party);
        for (const auto& event : bosses.takeLegendEvents()) {
            roared |= event.cue == LegendCue::Roared;
        }
    }
    REQUIRE(roared);
    REQUIRE(bosses.legend().running());
    CHECK_FALSE(bosses.blinded());
    bosses.landLegend();
    CHECK(bosses.blinded());
    checkEye("PBOSSQEYEBALL", "PBOSSEYEBALL");
    const f32 struckHealth = bosses.view().health;
    CHECK(struckHealth < originalHealth);
    CHECK_FALSE(bosses.legend().darkens());
    bosses.landLegend();
    CHECK(bosses.view().health == struckHealth);
    s32 recoveryFrames = 0;
    bool attackedBlind = false;
    bool aimedBlind = false;
    while (bosses.legend().running() && recoveryFrames < 1000) {
        bosses.update(kTicks, kStep, party);
        for (const auto& shot : bosses.takeShots()) {
            attackedBlind = true;
            aimedBlind |= shot.target.has_value();
        }
        ++recoveryFrames;
    }
    CHECK(recoveryFrames == Approx(29 * 30).margin(1));
    CHECK(attackedBlind);
    CHECK_FALSE(aimedBlind);
    CHECK_FALSE(bosses.blinded());
    checkEye("PBOSSEYEBALL", "PBOSSQEYEBALL");
    CHECK(bosses.view().health == struckHealth);
    bosses.landLegend(); // Recovery cannot reapply the javelin.
    CHECK_FALSE(bosses.blinded());
    bool aimedAfter = false;
    for (s32 frame = 0; frame < 600 && !aimedAfter; ++frame) {
        bosses.update(kTicks, kStep, party);
        for (const auto& shot : bosses.takeShots()) {
            aimedAfter |= shot.target.has_value();
        }
    }
    CHECK(aimedAfter);
}

TEST_CASE("Plague's lingering eye injury does not extend the opening's full-party damage",
          "[game][legend][plague][native-assets][multiplayer]") {
    const auto root = test::assetOrSkip("CRITTER/PBOSS.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.players = 2;
    bosses.open(device, root, nullptr, scales, 'K');
    REQUIRE(bosses.spawn(38, Vec3{0}, 0, 100));
    REQUIRE(bosses.bringLegend(0));
    const std::vector<EnemyView> party{playerAt({0, 0, 40}), playerAt({1, 0, 40}, 1)};
    for (s32 frame = 0; frame < 900 && !bosses.legend().thrown(); ++frame) {
        bosses.update(kTicks, kStep, party);
    }
    REQUIRE(bosses.legend().thrown());
    const f32 initial = bosses.view().health;
    bosses.landLegend();
    // Native PBOSS armor is two; opening damage does not yet take the
    // two-player half share (CritterDamage, states 1..4).
    CHECK(initial - bosses.view().health == Approx(initial * 0.1f - 2));
    for (s32 frame = 0; frame < 600 && bosses.legend().darkens(); ++frame) {
        bosses.update(kTicks, kStep, party);
    }
    REQUIRE_FALSE(bosses.legend().darkens());
    bosses.update(kTicks, kStep, party); // world state 5, still well before recovery
    REQUIRE(bosses.legend().running());
    REQUIRE(bosses.blinded());
    const f32 before = bosses.view().health;
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 100;
    bosses.hurt(hit);
    CHECK(before - bosses.view().health == Approx((100 - 2) * 0.5f));
}

TEST_CASE("the genie selects projectile attacks and launches them from its animated body",
          "[game][boss-projectiles][assets]") {
    const auto root = test::assetOrSkip("CRITTER/DJINN.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/DJINN/ANIM.PS2");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'C');
    REQUIRE(bosses.spawn(36, Vec3{0}, 0, 100));
    REQUIRE(bosses.cameraBase() == Vec3{0, 7, 0});
    REQUIRE(bosses.cameraOffset() == Vec3{0, 17, 0});
    const std::vector<EnemyView> party{playerAt(Vec3{0, 0, 40})};
    std::vector<CombatShot> shots;
    for (s32 frame = 0; frame < 900 && shots.empty(); ++frame) {
        bosses.update(kTicks, kStep, party);
        shots = bosses.takeShots();
    }
    REQUIRE_FALSE(shots.empty());
    for (const CombatShot& shot : shots) {
        REQUIRE(shot.data != nullptr);
        REQUIRE(shot.data->name() == "DJINN");
        REQUIRE(shot.data->damage(shot.damageIndex)->type == AttackDefinition::kProjectile);
        REQUIRE(shot.realm == 'C');
        REQUIRE(shot.target == Vec3{0, 3, 40});
        REQUIRE(shot.origin.y > 0);
    }
    bosses.close();
    REQUIRE(bosses.takeShots().empty());
    REQUIRE_FALSE(bosses.cameraBase().has_value());
}

TEST_CASE("anchored bosses retain local territories and the lich and spider can pursue",
          "[game][boss-movement][assets]") {
    struct Expected {
        const char* name;
        f32 radius;
        bool pursuit;
    };
    for (const auto& expected :
         {Expected{"DRAGON", 3, false}, Expected{"CHIMERA", 12, false}, Expected{"DJINN", 0, false},
          Expected{"PBOSS", 6, false}, Expected{"YETI", 5, false}, Expected{"WRAITH", 3, false},
          Expected{"LICH", 25, true}, Expected{"DRIDER", 22, true}}) {
        DYNAMIC_SECTION(expected.name) {
            const auto path = test::assetOrSkip(std::string("critter/") + expected.name + ".WAD");
            CritterData data;
            REQUIRE(data.load(path));
            REQUIRE(data.movement().roamRadius == expected.radius);
            bool pursues = false;
            bool hasNearAttack = false;
            bool hasRangedAttack = false;
            for (const MoveDefinition& move : data.moves()) {
                if (move.type == MoveDefinition::kWalk || move.type == 134) {
                    pursues = pursues || move.speed > 0;
                }
                if (move.attack() && move.harms()) {
                    hasNearAttack = hasNearAttack ||
                                    (move.target.maxDistance > 0 && move.target.maxDistance <= 40);
                    const AttackDefinition* harm = data.damage(move.damage0);
                    hasRangedAttack =
                        hasRangedAttack ||
                        (harm != nullptr && harm->type == AttackDefinition::kProjectile);
                }
            }
            REQUIRE(pursues == expected.pursuit);
            // Chimera's separate head move tables are not its root body's attacks.
            if (std::string_view(expected.name) != "CHIMERA") {
                REQUIRE(hasNearAttack);
                REQUIRE(hasRangedAttack);
            }
        }
    }
}

TEST_CASE("every retail boss can enter animate draw take damage and die",
          "[game][boss-roster][assets]") {
    for (s32 kind = 34; kind <= 44; ++kind) {
        const std::string name{bossNameOf(kind)};
        DYNAMIC_SECTION(name) {
            const auto root =
                test::assetOrSkip("critter/" + name + ".WAD").parent_path().parent_path();
            test::assetOrSkip("MONSTERS/" + name + "/ANIM.PS2");
            test::FakeRenderDevice device;
            Bosses bosses;
            bosses.open(device, root, nullptr, {}, 'G');
            REQUIRE(bosses.spawn(kind, Vec3{0}, 0, 100));
            REQUIRE(bosses.view().alive);
            REQUIRE(bosses.view().maxHealth > 0);
            // Twenty from wherever it stands: one homed away from the origin, the Spider Queen,
            // is drawn into its territory while it sleeps.
            for (s32 tick = 0; tick < 180; ++tick) {
                const std::vector<EnemyView> party{playerAt(*bosses.position() + Vec3{0, 0, 20})};
                bosses.update(kTicks, kStep, party);
            }
            REQUIRE(bosses.view().awake);
            REQUIRE_FALSE(bosses.moveName().empty());
            bosses.draw(device, Mat4{1}, {});
            REQUIRE_FALSE(device.draws.empty());
            EnemyHit hit;
            hit.player = 0;
            hit.damage = bosses.view().maxHealth * 2;
            bosses.hurt(hit);
            REQUIRE_FALSE(bosses.view().alive);
            REQUIRE_FALSE(bosses.takeLosses().empty());
        }
    }
}

TEST_CASE("only the plague fiend's preload prepares the javelin's eye, a profile's eye if named",
          "[game][bosses][legend-show][boss-kinds]") {
    using Keys = std::vector<std::pair<std::string, std::string>>;
    test::FakeRenderDevice device;
    const auto prepared = [&](s32 kind, const LegendProfile* profile) {
        const auto root = eyedBossAssets(std::string{bossNameOf(kind)});
        Bosses bosses;
        bosses.open(device, root, nullptr, {}, 'K', {}, profile);
        const CombatantAssets* stock = bosses.preload(kind);
        REQUIRE(stock != nullptr);
        Keys keys;
        for (const auto& entry : stock->replacementModels) {
            keys.push_back(entry.first);
        }
        bosses.close();
        return keys;
    };
    const Keys eye{{"EYEBALL", "PBOSSQEYEBALL"}};
    CHECK(prepared(38, nullptr) == eye);
    CHECK(prepared(39, nullptr).empty());
    LegendProfile named;
    named.eyeReplacement = "PBOSSQEYEBALL";
    CHECK(prepared(38, &named) == eye);
    CHECK(prepared(39, &named).empty());
    const LegendProfile keepsOwn;
    CHECK(prepared(38, &keepsOwn).empty());
}

TEST_CASE("a boss arms only once its activation signal comes, stays armed after it goes, and "
          "by default wakes two seconds after placement",
          "[game][bosses][boss-activation]") {
    test::FakeRenderDevice device;
    const auto root = eyedBossAssets("LICH");
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'G');
    REQUIRE(bosses.spawn(41, Vec3{0}, 0, 20));
    // Half-second steps add up to the two-second arming time exactly. CritterBossAI sets the
    // deadline on the frame that arms it and tests it from the next on (0x80039bac-0x80039bfc),
    // so the boss wakes on the update after the one that completes its two seconds.
    constexpr f32 kHalfSecond = 0.5f;
    const std::vector<EnemyView> near{playerAt(Vec3{0, 0, 10})};
    // An ordinary boss is engaged only once it wakes (BossActivate at 0x80039ca4), however long
    // it has been armed.
    const auto asleep = [&bosses] {
        REQUIRE_FALSE(bosses.view().awake);
        REQUIRE_FALSE(bosses.view().engaged);
    };
    const auto updates = [&](s32 count, bool activationReady) {
        for (s32 update = 0; update < count; ++update) {
            asleep();
            bosses.update(kTicks, kHalfSecond, near, activationReady);
        }
    };
    SECTION("the default signal keeps the GameCube's two seconds") {
        for (s32 update = 0; update < 4; ++update) {
            bosses.update(kTicks, kHalfSecond, near);
            asleep();
        }
        bosses.update(kTicks, kHalfSecond, near);
        CHECK(bosses.view().awake);
    }
    SECTION("without the signal it never wakes, however near the party stands") {
        for (s32 update = 0; update < 120; ++update) {
            bosses.arm(kHalfSecond, false);
            bosses.update(kTicks, kHalfSecond, near, false);
        }
        CHECK_FALSE(bosses.view().awake);
        updates(5, true); // the signal arriving at last starts the full two seconds
        CHECK(bosses.view().awake);
    }
    SECTION("an update's signal keeps it armed through updates without one") {
        updates(1, true);
        updates(3, false);
        asleep();
        bosses.update(kTicks, kHalfSecond, near, false);
        CHECK(bosses.view().awake);
    }
    SECTION("the held entrance arms it by the same rule") {
        bosses.arm(kHalfSecond, false);
        bosses.arm(kHalfSecond, true);
        bosses.arm(kHalfSecond, false);
        bosses.arm(kHalfSecond, false);
        bosses.update(kTicks, 0, near, false); // a second and a half armed
        asleep();
        bosses.update(kTicks, kHalfSecond, near, false); // two seconds, tested on the next
        asleep();
        bosses.update(kTicks, kHalfSecond, near, false);
        CHECK(bosses.view().awake);
    }
    // Awake, it is engaged.
    CHECK(bosses.view().engaged);
}

TEST_CASE("a boss wakes by its farthest player measured along the ground, strictly inside its "
          "range, and never for nobody",
          "[game][bosses][boss-activation]") {
    test::FakeRenderDevice device;
    const auto root = eyedBossAssets("LICH");
    // Four half seconds complete its two-second deadline, and the fifth update is the first to
    // test it.
    const auto woken = [&](std::span<const EnemyView> party, f32 range = 20) {
        Bosses bosses;
        bosses.open(device, root, nullptr, {}, 'G');
        REQUIRE(bosses.spawn(41, Vec3{0}, 0, range));
        for (s32 update = 0; update < 5; ++update) {
            bosses.update(kTicks, 0.5f, party);
        }
        return bosses.view().awake;
    };
    // Fifteen over it and 19.5 out: 24.9 away, but inside its twenty along the ground.
    CHECK(woken(std::array{playerAt({0, 15, 19.5f})}));
    CHECK(woken(std::array{playerAt({0, 0, 5}), playerAt({12, -9, 15}, 1)}));
    CHECK_FALSE(woken(std::array{playerAt({0, 0, 20})})); // on the edge of its range
    CHECK_FALSE(woken(std::array{playerAt({0, 0, 5}), playerAt({0, 0, 25}, 1)}));
    CHECK_FALSE(woken(std::array{playerAt({0, 5, 0})})); // straight over it, nought out
    CHECK_FALSE(woken({}));
    CHECK(woken({}, 0)); // a range of nought (the table's, here) wakes it with nobody about
}

TEST_CASE("asleep, a boss outside its territory is held inside it from its first update, its heads "
          "with it, and the party is measured from there",
          "[game][bosses][boss-movement][boss-activation]") {
    // DRIDER's home and roam, and D5's mark 24.874 from it by the original's measure.
    const Vec3 home{-30, 0, -112};
    const Vec3 mark{-52.75f, 0, -121.0f};
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, test::headedBodyAssets(R"(,"roamRadius":22,"defaultPos":[-30,0,-112],
                                          "wakeThreshold":20)"),
                nullptr, {}, 'A');
    REQUIRE(bosses.spawn(boss_type::kChimera, mark, 0));
    const Combatant& body = bosses.fighter();
    REQUIRE(body.childCount() == 2);
    const std::array far{playerAt({0, 0, 500})};
    bosses.update(kTicks, kStep, far);
    REQUIRE_FALSE(bosses.view().awake);
    const Vec3 offset = mark - home;
    const Vec3 held = home + offset * (22 / approximateFlatLength(offset));
    CHECK(bosses.position()->x == Approx(held.x));
    CHECK(bosses.position()->z == Approx(held.z));
    for (s32 part = 1; part <= 2; ++part) {
        CAPTURE(part);
        REQUIRE(body.child(part) != nullptr);
        CHECK(body.child(part)->position().x == Approx(held.x));
        CHECK(body.child(part)->position().z == Approx(held.z));
    }
    // 19.9 out from where it is held, beyond the mark: 22.7 from the mark itself.
    const Vec3 away = glm::normalize(Vec3{held.x - mark.x, 0, held.z - mark.z});
    const std::array near{playerAt(held + away * 19.9f)};
    REQUIRE(glm::length(Vec2{near[0].position.x - mark.x, near[0].position.z - mark.z}) > 22);
    s32 updates = 1;
    while (!bosses.view().awake && updates < 120) {
        bosses.update(kTicks, kStep, near);
        ++updates;
    }
    CHECK(bosses.view().awake);
    CHECK(updates == 62); // the update after its two seconds
}

TEST_CASE("an imported rite that wakes its boss at the deadline does so wherever the party "
          "stands, and only while the rite runs",
          "[game][bosses][boss-activation][legend]") {
    test::FakeRenderDevice device;
    const auto root = eyedBossAssets("LICH");
    const std::array far{playerAt({0, 0, 500})};
    const auto woken = [&](const LegendProfile* profile, bool borne) {
        Bosses bosses;
        bosses.open(device, root, nullptr, {}, 'G', {}, profile);
        REQUIRE(bosses.spawn(41, Vec3{0}, 0, 20));
        if (borne) {
            REQUIRE(bosses.bringLegend(0));
        }
        for (s32 update = 0; update < 4; ++update) {
            bosses.update(kTicks, 0.5f, far);
            REQUIRE_FALSE(bosses.view().awake); // never before its deadline
        }
        bosses.update(kTicks, 0.5f, far);
        return bosses.view().awake;
    };
    LegendProfile wakes;
    wakes.wakesOnDeadline = true;
    CHECK(woken(&wakes, true));
    CHECK_FALSE(woken(&wakes, false)); // nobody bears the item: no rite
    const LegendProfile waits;
    CHECK_FALSE(woken(&waits, true));
    CHECK_FALSE(woken(nullptr, true)); // the base game's rite waits for the party
}

TEST_CASE("a boss that waits for its floor arms on the update after a trigger first moves the "
          "floor, wakes two seconds later and stays armed",
          "[game][bosses][boss-activation][boss-support]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    const f32 step = 1.0f / static_cast<f32>(fps);
    const s32 ticks = std::max(1, Combatant::kTicksPerSecond / fps);
    // f32 sums of 1/30, 1/60 and 1/120 from nought first reach two seconds at 61, 121 and 241
    // additions (60, 120 and 240 give 1.9999994, 1.9999988 and 1.9999985). The arming update
    // counts the first, and the test on the update after the last passes.
    const s32 waited = fps == 30 ? 61 : (fps == 60 ? 121 : 241);
    const auto subtype = GENERATE(26, 29);
    CAPTURE(subtype);
    LiftFixture lift(subtype);
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, eyedBossAssets("CHIMERA", kChimeraTypeFlags), &lift.collision, {}, 'A');
    // On the deck hung from the lift, or on the lift's own face where it stays solid as it moves
    // (subtype 29, flags 0x0A; subtype 26 leaves its own triangles behind while it moves).
    const Vec3 mark = subtype == 26 ? LiftFixture::kOnDeck : LiftFixture::kOnLiftFace;
    REQUIRE(bosses.spawn(boss_type::kChimera, mark, 0, 20));
    REQUIRE(bosses.position()->y == Approx(mark.y));
    const auto near = [&] { return std::array{playerAt(*bosses.position() + Vec3{0, 0, 10})}; };
    const std::array far{playerAt(Vec3{0, 0, 500})};
    const std::array onPad{TriggerVisitor{.position = LiftFixture::kPad}};
    // PlayScene's order: the boss in the opponents' turn, then the triggers and the collision.
    const auto frame = [&](std::span<const EnemyView> party,
                           std::span<const TriggerVisitor> visitors = {}) {
        bosses.update(ticks, step, party, true, &lift.triggers);
        lift.step(step, visitors);
    };
    // The held entrance arms nothing: no trigger moves while it lasts. Nor do updates with the
    // party all about it while the pad lies idle.
    for (s32 update = 0; update < 30; ++update) {
        bosses.arm(step, true, &lift.triggers);
    }
    for (s32 update = 0; update < 3 * fps; ++update) {
        frame(near());
        REQUIRE_FALSE(bosses.view().engaged);
    }
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(lift.descent() == 0);
    SECTION("contact arms it on the next update, which the wake follows two seconds on") {
        // Update P: the boss's turn finds its floor still; the pad then starts the lift.
        frame(near(), onPad);
        CHECK_FALSE(bosses.view().engaged);
        CHECK(lift.descent() > 0);
        // P+1: its probe finds the floor moving, and the encounter begins.
        frame(near());
        CHECK(bosses.view().engaged);
        CHECK_FALSE(bosses.view().awake);
        for (s32 update = 2; update <= waited; ++update) {
            frame(near());
            CAPTURE(update);
            REQUIRE_FALSE(bosses.view().awake);
        }
        frame(near()); // P + waited + 1: P+62 at 30 a second
        CHECK(bosses.view().awake);
        CHECK(bosses.view().engaged);
        CHECK_FALSE(lift.triggers.settled(LiftFixture::kLift)); // it wakes on the way down
        CHECK(bosses.position()->y < mark.y - 5);
    }
    SECTION("the floor settling does not disarm it") {
        frame(far, onPad);
        while (!lift.triggers.settled(LiftFixture::kLift)) {
            frame(far);
        }
        CHECK(lift.descent() == Approx(LiftFixture::kTravel));
        for (s32 update = 0; update < fps; ++update) {
            frame(far);
            REQUIRE(bosses.view().engaged);
        }
        CHECK_FALSE(bosses.view().awake);
        frame(near());
        CHECK(bosses.view().awake);
    }
}

TEST_CASE("a boss that waits for its floor never arms on a lift held up by a pressure plate, and "
          "any other boss arms at once whatever the pad",
          "[game][bosses][boss-activation][boss-support]") {
    const s32 fps = GENERATE(30, 60, 120);
    CAPTURE(fps);
    const f32 step = 1.0f / static_cast<f32>(fps);
    const s32 ticks = std::max(1, Combatant::kTicksPerSecond / fps);
    const std::array onPad{TriggerVisitor{.position = LiftFixture::kPad}};
    test::FakeRenderDevice device;
    SECTION("a pressure plate keeps no trigger state, so the lift it moves arms nothing") {
        LiftFixture lift(21);
        Bosses bosses;
        bosses.open(device, eyedBossAssets("CHIMERA", kChimeraTypeFlags), &lift.collision, {}, 'A');
        REQUIRE(bosses.spawn(boss_type::kChimera, LiftFixture::kOnDeck, 0, 20));
        for (s32 update = 0; update < 4 * fps; ++update) {
            const std::array near{playerAt(*bosses.position() + Vec3{0, 0, 10})};
            bosses.update(ticks, step, near, true, &lift.triggers);
            lift.step(step, onPad);
            REQUIRE_FALSE(bosses.view().engaged);
        }
        CHECK(lift.descent() > 5);
        CHECK_FALSE(bosses.view().awake);
    }
    SECTION("an ordinary boss wakes on the 62nd update after placement at 30 a second") {
        // CritterBossAI sets an ordinary boss's deadline on its first frame and first passes
        // its test 61 frames on (0x80039bd8-0x80039bfc): the 62nd update, the 122nd at 60 a
        // second and the 242nd at 120. Pressing the pad on the way changes nothing.
        const s32 wakes = fps == 30 ? 62 : (fps == 60 ? 122 : 242);
        LiftFixture lift(26);
        Bosses bosses;
        bosses.open(device, eyedBossAssets("CHIMERA"), &lift.collision, {}, 'A');
        REQUIRE(bosses.spawn(boss_type::kChimera, LiftFixture::kOnDeck, 0, 20));
        for (s32 update = 1; update <= wakes; ++update) {
            CAPTURE(update);
            REQUIRE_FALSE(bosses.view().engaged);
            const std::array near{playerAt(*bosses.position() + Vec3{0, 0, 10})};
            bosses.update(ticks, step, near, true, &lift.triggers);
            lift.step(step, update == 10 ? std::span<const TriggerVisitor>{onPad}
                                         : std::span<const TriggerVisitor>{});
            CHECK(bosses.view().awake == (update == wakes));
            CHECK(bosses.view().engaged == bosses.view().awake);
        }
    }
}

} // namespace
