#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/bosses/Bosses.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/MissileTarget.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/MultiplayerMode.h"
#include "game/critters/CombatEvents.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/enemies/DeathTestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/Generators.h"
#include "game/hud/HelpMessages.h"
#include "game/level/BossKinds.h"
#include "game/level/ImportedLevel.h"
#include "game/level/ImportedTargets.h"
#include "game/level/ItemFigure.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/mechanics/BlowRules.h"
#include "game/party/AttackTargetId.h"
#include "game/party/PlayerAttacksFixture.h"
#include "game/party/PlayerAttacksTestPeer.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::turboAssets;

/** The ways a party blow lands, each through its own dispatcher. */
enum class BlowPath : u8 {
    Swing,      ///< a close attack (melee)
    Shot,       ///< a thrown weapon's impact (dispatchProjectileImpact)
    Area,       ///< a turbo strike's, a breath's or a shield's touch (strikeTarget)
    PotionWave, ///< a wave of magic (updatePotions)
    ShieldRing, ///< a potion's ring about its bearer (updateShields)
    Combo,      ///< a flier or charger of the combo (comboImpact)
    Charge,     ///< the charge (ramBarrels)
};

constexpr f32 kGeneratorHealth = 500;
constexpr u32 kFireBlow = 1; ///< an element, so that a great one's or boss's hit is marked
/** The potion of the cases' waves: fire, whose element a wave carries to what it reaches. */
constexpr s32 kWavePotion = 1;

/** Rules that answer whether a generator takes a blow as they are told to, and keep what they
 * were asked. */
struct RecordingBlowRules final : BlowRules {
    bool takes = true;
    mutable std::vector<BlowSource> asked;
    mutable std::vector<BlowReport> reports;
    std::optional<RunEffect> runEffect(const BlowReport& blow) const override {
        reports.push_back(blow);
        return std::nullopt;
    }
    bool generatorTakes(BlowSource source) const override {
        asked.push_back(source);
        return takes;
    }
};

/** turboAssets' warrior, player 3 at the origin facing +z, and its grunt; the opponents open
 * over the same archives once whatever world a case wants is in place. */
struct BlowScene : test::PlayerAttacksFixture {
    std::filesystem::path root = turboAssets();
    BlowScene() {
        REQUIRE(classes.load(root / "pdata"));
        players[0].figure =
            PlayerFigure::load(device, root, players[0].actor.save(), Entrance::Skip);
        REQUIRE(players[0].figure);
    }
    void open() {
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        opponents.enemies().open(device, root, nullptr, 8, EnemyScales{.health = 100}, 1);
        REQUIRE(opponents.enemies().loadKind(kGruntKind));
    }
    void close() {
        targets.blows = nullptr;
        attacks.clear();
        fixtures.clear();
        opponents.close();
    }
    PlayerActor& striker() { return players[0].actor; }
    /** Stands the striker a tenth short of the near side of a body `radius` round at `at`,
     * facing it: within a swing, the charge's and the combo's touch and a ring of ten. */
    void standBefore(const Vec3& at, f32 radius) {
        constexpr f32 kGap = 0.1f;
        striker().place(at - Vec3{0, 0, radius + striker().radius() + kGap});
    }
};

/** A generator of kGeneratorHealth, six tall and two either side of `at`: target 2000. */
void placeGenerator(BlowScene& f, const Vec3& at) {
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = static_cast<s32>(kGeneratorHealth);
    info.height = 6;
    info.xSize = 2;
    info.zSize = 2;
    Mat4 placement{1};
    placement[3] = Vec4{at, 1};
    REQUIRE(f.opponents.generators().placeBoss(f.device, info, f.weapons, f.opponents.enemies(),
                                               kGruntKind, placement, nullptr));
}

/** Stands the striker before generator 0, as standBefore does. */
void standBeforeGenerator(BlowScene& f) {
    const Generators& generators = f.opponents.generators();
    const Obstacle& box = generators.boxOf(0);
    f.standBefore(generators.positionOf(0), std::max(box.halfAcross, box.halfAlong));
}

/** One blow of `path` by the striker on `target`, which stands where the path reaches it. A
 * swing, a combo and a charge find their target and deal their own harm; the rest deal
 * `damage` with `flags`, the shot landing at `impactAt` on the target's collision part (as a
 * flight that meets it carries the part) and the wave going off at the target's feet. */
void land(BlowScene& f, BlowPath path, const MissileTarget& target, f32 damage, u32 flags,
          const Vec3& impactAt) {
    const s32 striker = f.striker().player();
    switch (path) {
    case BlowPath::Swing: f.attacks.melee(0, f.players, f.targets); break;
    case BlowPath::Shot: {
        MissileImpact impact;
        impact.position = impactAt;
        impact.owner = striker;
        impact.damage = damage;
        impact.flags = flags;
        impact.target = target.id;
        impact.node = target.node;
        impact.direction = f.striker().facing();
        PlayerAttacksTestPeer::projectile(f.attacks, impact, f.players, f.targets);
        break;
    }
    case BlowPath::Area:
        PlayerAttacksTestPeer::strike(f.attacks, target, damage, flags, f.players, f.targets,
                                      BlowSource::Turbo);
        break;
    case BlowPath::PotionWave: {
        MissileImpact impact;
        impact.position = target.base;
        impact.owner = striker;
        impact.potion = kWavePotion;
        impact.potency = 20;
        impact.damage = damage;
        impact.flags = flags;
        PlayerAttacksTestPeer::burst(f.attacks, impact);
        PlayerAttacksTestPeer::potions(f.attacks, f.players, f.targets);
        break;
    }
    case BlowPath::ShieldRing:
        PlayerAttacksTestPeer::ring(f.attacks, EnemyHit::kMagic | flags, 10, damage);
        f.attacks.updateShields(1.0f / 60, f.players, f.targets);
        break;
    case BlowPath::Combo: (void)f.attacks.comboImpact(0, 1, damage, f.players, f.targets); break;
    case BlowPath::Charge: f.attacks.ramBarrels(0, f.players, f.targets); break;
    }
}

TEST_CASE("a swing never asks whether a generator takes it, and of the rest only a shot and an "
          "area blow count",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path =
        GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area, BlowPath::PotionWave,
                 BlowPath::ShieldRing, BlowPath::Combo, BlowPath::Charge);
    const bool takes = GENERATE(false, true);
    const auto way = static_cast<s32>(path);
    CAPTURE(way, takes);
    BlowScene f;
    f.open();
    RecordingBlowRules rules;
    rules.takes = takes;
    f.targets.blows = &rules;
    placeGenerator(f, Vec3{0, 0, 6});
    standBeforeGenerator(f);
    const Generators& generators = f.opponents.generators();
    land(f, path, generators.target(0, 2000), 30, 0, generators.positionOf(0));
    struct Answer {
        std::vector<BlowSource> asked;
        bool harmed = false;
        bool counted = false;
    };
    Answer expected;
    switch (path) {
    case BlowPath::Swing: expected = {{}, true, true}; break;
    case BlowPath::Shot: expected = {{BlowSource::Shot}, takes, takes}; break;
    case BlowPath::Area: expected = {{BlowSource::Turbo}, takes, takes}; break;
    case BlowPath::PotionWave:
    case BlowPath::ShieldRing: expected = {{BlowSource::Potion}, takes, false}; break;
    case BlowPath::Combo: expected = {{BlowSource::Charge}, takes, false}; break;
    case BlowPath::Charge: expected = {{}, false, false}; break; // it never meets a generator
    }
    CHECK(rules.asked == expected.asked);
    CHECK((generators.healthOf(0) < kGeneratorHealth) == expected.harmed);
    const auto counted = std::ranges::count_if(
        rules.reports, [](const BlowReport& blow) { return blow.target == BlowTarget::Generator; });
    CHECK(counted == (expected.counted ? 1 : 0));
    f.close();
}

/** Loads G1 of a synthetic town, meant for characters of `playerLevel`. */
void loadPlace(BlowScene& f, f32 playerLevel) {
    const auto level = f.root / "LEVELS/LEVELG1";
    std::filesystem::create_directories(level);
    std::filesystem::create_directories(f.root / "wdata");
    writeTextFile(f.root / "wdata/TOWN.json",
                  std::format(R"({{"realm":7,"prefix":"levelG","levels":[
        {{"name":"G1","tuning":{{"playerLevel":{}}}}}]}})",
                              playerLevel));
    writeTextFile(level / "world.json", R"({"objects":[
        {"name":"FLOOR","object":0,"parent":-1,"position":[0,0,0]}]})");
    writeTextFile(level / "objects.json",
                  R"({"objects":[{"name":"FLOOR","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(level / "mesh.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(level / "skin.png", test::kTinyPng);
    writeTextFile(level / "textures.json", R"({"bitmaps":[
        {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(level);
    LevelCatalog catalog;
    REQUIRE(catalog.load(f.root));
    const auto ref = catalog.byName("G1");
    REQUIRE(ref.has_value());
    REQUIRE(f.world.load(f.device, f.root, *ref));
    REQUIRE(f.world.level() != nullptr);
    REQUIRE(f.world.level()->tuning.playerLevel == playerLevel);
}

TEST_CASE("a shield ring's blow on a generator alone goes unscaled by the striker's level",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path = GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area,
                                   BlowPath::PotionWave, BlowPath::ShieldRing, BlowPath::Combo);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    loadPlace(f, 10.0f);
    f.open();
    // Ten levels over the place's: a scaled blow does twice its harm (fn_8005C1DC's ramp).
    f.striker().save().progress().experience = levelExperience(20);
    placeGenerator(f, Vec3{0, 0, 6});
    standBeforeGenerator(f);
    const Generators& generators = f.opponents.generators();
    const f32 swing = f.attacks.meleeDamageOf(0, f.players);
    land(f, path, generators.target(0, 2000), 30, 0, generators.positionOf(0));
    const f32 taken = kGeneratorHealth - generators.healthOf(0);
    switch (path) {
    case BlowPath::Swing: CHECK(taken == std::round(2 * swing)); break;
    case BlowPath::Shot:
    case BlowPath::Area:
    case BlowPath::Combo: CHECK(taken == 60); break;
    case BlowPath::PotionWave: CHECK(taken == 59); break;
    case BlowPath::ShieldRing: CHECK(taken == 30); break;
    case BlowPath::Charge: break;
    }
    f.close();
}

TEST_CASE("an area blow heals nothing even when it carries the healing bit, as a shot heals",
          "[game][player-attacks][blow-paths][healing-magic]") {
    const BlowPath path = GENERATE(BlowPath::Shot, BlowPath::Area);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    f.open();
    std::vector<s32> helps;
    f.fixtureEvents.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    CharacterSave& save = f.striker().save();
    save.progress().experience = levelExperience(80);
    save.progress().health = 100;
    Enemies& enemies = f.opponents.enemies();
    const auto grunt =
        enemies.spawn({.kind = kGruntKind, .position = {0, 0, 5}, .placed = true}, {});
    REQUIRE(grunt);
    REQUIRE(enemies.targets().size() == 1);
    MissileTarget target = enemies.targets().front();
    target.id += 1000;
    const f32 whole = enemies.healthOf(*grunt);
    land(f, path, target, 20, damage::kHeal | kFireBlow, target.base);
    REQUIRE(enemies.healthOf(*grunt) < whole);
    const bool heals = path == BlowPath::Shot;
    CHECK(save.health() == (heals ? 104 : 100));
    CHECK((std::ranges::find(helps, HelpMessages::kHealingMagic) != helps.end()) == heals);
    f.close();
}

/** Garm's brood in turboAssets' shape: the one swarm kind tall enough (a reach of four or
 * more) that a blow's own landing point marks it. */
void addGarmBrood(const std::filesystem::path& root) {
    const auto dir = root / "MONSTERS/GRM";
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"GRM1",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":60,"rate":30}]}]})");
}

TEST_CASE("where each blow path tells the swarm a blow landed, and whether it was close",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path =
        GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area, BlowPath::PotionWave,
                 BlowPath::ShieldRing, BlowPath::Combo, BlowPath::Charge);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    addGarmBrood(f.root);
    f.open();
    Enemies& enemies = f.opponents.enemies();
    REQUIRE(enemies.loadKind(kGarmBroodKind));
    const Vec3 at{0, 0, 8};
    REQUIRE(enemies.spawn({.kind = kGarmBroodKind, .position = at, .placed = true}, {}));
    REQUIRE(enemies.targets().size() == 1);
    MissileTarget target = enemies.targets().front();
    target.id += 1000;
    f.standBefore(at, target.radius);
    const Vec3 landed{1, 4, 6};
    land(f, path, target, 30, 0, landed);
    const auto marks = enemies.takeFeedback();
    REQUIRE(marks.size() == 1);
    // The swing's is the middle of the body it chose; a blow without a point of its own marks
    // the body at its attention height.
    const Vec3 middle = target.base + Vec3{0, target.height * 0.5f, 0};
    const Vec3 attention = at + Vec3{0, enemyKind(kGarmBroodKind).attentionHeight, 0};
    bool close = false;
    Vec3 where{0};
    switch (path) {
    case BlowPath::Swing:
        close = true;
        where = middle;
        break;
    case BlowPath::Shot: where = landed; break;
    case BlowPath::Area:
    case BlowPath::PotionWave:
    case BlowPath::ShieldRing: where = attention; break;
    case BlowPath::Combo:
    case BlowPath::Charge:
        close = true;
        where = target.base;
        break;
    }
    CHECK(marks.front().close == close);
    CHECK(marks.front().position.x == where.x);
    CHECK(marks.front().position.y == where.y);
    CHECK(marks.front().position.z == where.z);
    f.close();
}

/** A great one in turboAssets' shape: a golem a hundred strong whose one collision part, about
 * its BODY node, is spent by a point of harm, and which marks a close blow CLOSEHIT and the
 * rest FARHIT where the blow says it landed, as an elemental blow bursts there. */
void addGreatOne(const std::filesystem::path& root) {
    const auto dir = root / "MONSTERS/GOLEM/LEVELG";
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"GOLEM",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":60,"rate":30}]}]})");
    std::filesystem::create_directories(root / "critter");
    writeTextFile(root / "critter/GOLEM.json",
                  std::format(R"({{"descriptors":[{{"prefix":"GOLEM","name":"GOLEM","type":{}}}],
      "types":[{{"moveCount":1,"maxHealth":100,"radius":1.5,"wallRadius":1,
                "originOffset":[0,3,0],"colCount":1,"hitSoundClose":0,"hitSoundFar":1}}],
      "moves":[{{"name":"READY","anim":"READY","type":32}}],
      "nodes":[{{"nodeName":"BODY","position":[0,2,0],"radius":1.5,"healthScale":0.01}}],
      "sounds":[{{"name":"CLOSEHIT"}},{{"name":"FARHIT"}}]}})",
                              static_cast<s32>(CombatantKind::Golem)));
}

/** The great one of addGreatOne, standing at `at` facing +z: critter 0. */
void spawnGreatOne(BlowScene& f, const Vec3& at) {
    Critters& critters = f.opponents.critters();
    critters.open(f.device, f.root, nullptr, {}, 'G');
    REQUIRE(critters.spawn(CombatantKind::Golem, at, 0) == std::optional<s32>{0});
}

/** Whether any of a roster's bodies is the collision part `node`. */
bool offersPart(std::span<const MissileTarget> bodies, s32 node) {
    return std::ranges::any_of(bodies,
                               [node](const MissileTarget& body) { return body.node == node; });
}

TEST_CASE("where each blow path tells a great one a blow landed, whether it was close, and "
          "which pass on the part they struck",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path =
        GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area, BlowPath::PotionWave,
                 BlowPath::ShieldRing, BlowPath::Combo, BlowPath::Charge);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    addGreatOne(f.root);
    f.open();
    const Vec3 at{0, 0, 8};
    spawnGreatOne(f, at);
    Critters& critters = f.opponents.critters();
    // Its BODY part (node 0) is offered first, then the body itself, which has no part.
    const auto bodies = critters.targets();
    REQUIRE(bodies.size() == 2);
    REQUIRE(bodies[0].node == 0);
    REQUIRE(bodies[1].node == -1);
    MissileTarget part = bodies[0];
    part.id += 3000;
    f.standBefore(at, part.radius);
    (void)critters.takeCues();
    const Vec3 landed{1, 4, 6};
    land(f, path, part, 30, 0, landed);
    const std::string_view burst = damage::hitEffect(static_cast<u32>(kWavePotion), false);
    std::vector<CombatCue> marks;
    for (CombatCue& cue : critters.takeCues()) {
        if (cue.tree == "CLOSEHIT" || cue.tree == "FARHIT" || cue.tree == burst) {
            marks.push_back(std::move(cue));
        }
    }
    REQUIRE(marks.size() == 1);
    // A blow passed the part it struck spends it; the rest harm the body as a whole.
    const bool spent = !offersPart(critters.targets(), 0);
    // The swing's is the middle of the part it chose; a blow told no point of its own marks
    // the body at its origin offset.
    const Vec3 middle = part.base + Vec3{0, part.height * 0.5f, 0};
    const Vec3 origin = at + Vec3{0, 3, 0};
    // A wave's element bursts in place of the far mark.
    std::string_view mark = "FARHIT";
    bool passesPart = false;
    Vec3 where{0};
    switch (path) {
    case BlowPath::Swing:
        mark = "CLOSEHIT";
        passesPart = true;
        where = middle;
        break;
    case BlowPath::Shot:
        passesPart = true;
        where = landed;
        break;
    case BlowPath::Area: where = part.base; break;
    case BlowPath::PotionWave:
        mark = burst;
        where = part.base;
        break;
    case BlowPath::ShieldRing: where = origin; break;
    case BlowPath::Combo:
    case BlowPath::Charge:
        mark = "CLOSEHIT";
        where = part.base;
        break;
    }
    CHECK(marks.front().tree == mark);
    CHECK(marks.front().position.x == where.x);
    CHECK(marks.front().position.y == where.y);
    CHECK(marks.front().position.z == where.z);
    CHECK(spent == passesPart);
    f.close();
}

/** The synthetic two-headed boss of HeadedBodyFixture, from `root`, awake at `at`. */
void spawnHeadedBoss(BlowScene& f, const std::filesystem::path& root, const Vec3& at) {
    Bosses& bosses = f.opponents.bosses();
    bosses.open(f.device, root, nullptr, {}, 'A');
    REQUIRE(bosses.spawn(boss_type::kChimera, at, 0));
    bosses.wake();
}

/** HeadedBodyFixture's boss with a collision part of its body's own about its NECK, spent by
 * a point of harm. */
std::filesystem::path partedBossAssets() {
    const auto root = test::scratchDirectory("blow-path-boss");
    std::filesystem::copy(test::headedBodyAssets(), root,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    const auto file = root / "critter/CHIMERA.json";
    auto data = nlohmann::json::parse(readTextFile(file));
    data["types"][0]["colCount"] = 1;
    data["nodes"] =
        nlohmann::json::parse(R"([{"nodeName":"NECK","radius":2,"healthScale":0.001}])");
    writeTextFile(file, data.dump());
    return root;
}

TEST_CASE("where each blow path tells the boss a blow landed, and which pass on the part they "
          "struck",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path = GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area,
                                   BlowPath::PotionWave, BlowPath::ShieldRing);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    f.open();
    const Vec3 anchor{0, 0, 8};
    spawnHeadedBoss(f, partedBossAssets(), anchor);
    Bosses& bosses = f.opponents.bosses();
    // The NECK part (node 0) is offered first, then the body itself and its two heads.
    const auto parts = bosses.targets();
    REQUIRE(parts.size() == 4);
    MissileTarget body = parts.front();
    REQUIRE(body.id == Bosses::kTargetId);
    REQUIRE(body.node == 0);
    const Vec3 middle = body.base + Vec3{0, body.height * 0.5f, 0};
    body.id += 4000;
    // Off the part's own place, so that a blow told its target's place shows it.
    body.base += Vec3{0.5f, 1, 0};
    f.standBefore(anchor, parts[1].radius);
    // The swing's own element, so that it bursts too.
    f.striker().save().progress().inventory.addPowerup(powerup::kWeapon, kFireBlow, 0, 60);
    (void)bosses.takeCues();
    const Vec3 landed{1, 4, 6};
    land(f, path, body, 30, kFireBlow, landed);
    // An elemental blow bursts where the boss is told it landed (CritterDamage), else at the
    // body's own anchor.
    const auto cues = bosses.takeCues();
    const auto burst = std::ranges::find_if(cues, [](const CombatCue& cue) {
        return cue.critter == Bosses::kTargetId && cue.tree == damage::hitEffect(kFireBlow, false);
    });
    REQUIRE(burst != cues.end());
    // A blow passed the part it struck spends it; the rest harm the body as a whole.
    const bool spent = !offersPart(bosses.targets(), 0);
    Vec3 where = anchor;
    bool passesPart = false;
    switch (path) {
    case BlowPath::Swing:
        where = middle;
        passesPart = true;
        break;
    case BlowPath::Shot:
        where = landed;
        passesPart = true;
        break;
    case BlowPath::Area: where = body.base; break;
    case BlowPath::PotionWave:
    case BlowPath::ShieldRing:
    case BlowPath::Combo:
    case BlowPath::Charge: break;
    }
    CHECK(burst->position.x == where.x);
    CHECK(burst->position.y == where.y);
    CHECK(burst->position.z == where.z);
    CHECK(spent == passesPart);
    f.close();
}

/** One of each fixture a party blow reaches, far apart: a secret wall facing -z at z 20, a
 * barrel at x 20, a safe rock at x -20, a shot switch at z -20, a shut chest at (-20, -20)
 * and a statue of Death at (20, -20); the fixtures bound over it, with a POT_RED bottle to
 * be placed. */
void loadFixtureWorld(BlowScene& f) {
    std::filesystem::copy(test::deathArchive() / "MONSTERS", f.root / "MONSTERS",
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    const auto bottles = f.root / LevelWorld::kPowerups;
    std::filesystem::create_directories(bottles);
    std::filesystem::copy(f.root / "MONSTERS/GRU", bottles,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    writeTextFile(bottles / "animations.json", R"({"trees":[{"name":"POT_RED",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"IDLE","frames":60,"rate":30}]}]})");
    writeTextFile(f.root / "PLAYERS/WAR/YEL/world.json", R"({"objects":[
      {"name":"BODY","position":[-100,0,0]}],
      "itemInfos":[
       {"type":10,"subtype":42,"hitPoints":25,"armor":1},
       {"type":10,"subtype":43,"name":"BARREL","radius":0.5,"height":3,
        "collisionOffset":[0,1.5,0],"armor":0},
       {"type":10,"subtype":41,"name":"ROCK","collisionType":1,"radius":2.3,"height":5,
        "hitPoints":40,"armor":10},
       {"type":5,"subtype":31,"radius":1,"height":6,"collisionOffset":[0,2,0],"armor":0},
       {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2},
       {"type":4,"name":"DEATH","radius":1,"height":2}],
      "itemInstances":[
       {"info":0,"name":"BODY","position":[0,0,20],
        "collision":[{"normal":[0,0,-1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]},
       {"info":1,"position":[20,0,0]},
       {"info":2,"position":[-20,0,0],"params":[41,0,3,0,0,0,0,0,0,0,0,0]},
       {"info":3,"position":[0,0,-20],"params":[0,0,8,0,0,0,1,0,0,0,0,0]},
       {"info":4,"position":[-20,0,-20]},
       {"info":5,"position":[20,0,-20],"params":[0,0,3,0,0,0,0,65,0,0,0,0]}]})");
    LevelRef level;
    level.directory = "PLAYERS/WAR/YEL";
    REQUIRE(f.world.load(f.device, f.root, level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
    REQUIRE(f.world.walls().size() == 1);
    REQUIRE(f.fixtures.barrels().size() == 1);
    REQUIRE(f.fixtures.safeRocks().size() == 1);
    REQUIRE(f.world.triggers().size() == 1);
    REQUIRE(f.fixtures.inertStops().size() == 1);
}

/** Each target's family and index, in the roster's order. */
std::vector<std::pair<TargetFamily, s32>> familiesOf(std::span<const MissileTarget> roster) {
    std::vector<std::pair<TargetFamily, s32>> out;
    for (const MissileTarget& target : roster) {
        const AttackTargetId id = AttackTargetId::fromId(target.id);
        out.emplace_back(id.family(), id.index());
    }
    return out;
}

TEST_CASE("each party roster offers the families its blows land on, in its own order",
          "[game][player-attacks][blow-paths][target-rosters]") {
    // A wave of magic and every area blow read the strike roster; a swing, the acquisition
    // roster, then a partner in hurt mode; a thrown weapon's flight, the strike roster and
    // then the bottles lying about and the inert stops (the next case). An imported level's
    // fixtures follow the walls (the imported cases below).
    BlowScene f;
    addGreatOne(f.root);
    loadFixtureWorld(f);
    f.open();
    REQUIRE(f.opponents.enemies().spawn(
        {.kind = kGruntKind, .position = {20, 0, 20}, .placed = true}, {}));
    spawnGreatOne(f, Vec3{40, 0, 0});
    placeGenerator(f, Vec3{-20, 0, 20});
    spawnHeadedBoss(f, test::headedBodyAssets(), Vec3{0, 0, -40});
    REQUIRE(f.opponents.statues().count() == 1);
    using enum TargetFamily;
    // The great one twice: its BODY part, then the body itself.
    const std::vector<std::pair<TargetFamily, s32>> shot{
        {Wall, 0}, {Barrel, 0}, {Enemy, 0},  {Critter, 0},   {Critter, 0}, {Boss, 0},
        {Boss, 1}, {Boss, 2},   {Statue, 0}, {Generator, 0}, {SafeRock, 0}};
    const auto offered = PlayerAttacksTestPeer::targets(f.attacks, f.targets);
    CHECK(familiesOf(offered) == shot);
    std::vector<s32> nodes;
    nodes.reserve(offered.size());
    for (const MissileTarget& body : offered) {
        nodes.push_back(body.node);
    }
    CHECK(nodes == std::vector<s32>{-1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1});
    auto struck = shot;
    struck.emplace_back(Switch, 0);
    CHECK(familiesOf(PlayerAttacksTestPeer::strikeTargets(f.attacks, f.targets)) == struck);
    auto acquired = struck;
    std::erase(acquired, std::pair{SafeRock, 0});
    CHECK(familiesOf(PlayerAttacksTestPeer::acquisitionTargets(f.attacks, f.targets)) == acquired);
    // The swing's partner, in hurt mode only.
    std::array<PlayerRuntime, 2> party;
    party[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 2.5f}, 0);
    f.targets.players = party;
    const Vec3 ahead{0, 0, 1};
    CHECK_FALSE(PlayerAttacksTestPeer::meleePlayer(party[0].actor, f.targets, 4, ahead));
    f.targets.multiplayer = MultiplayerMode::Hurt;
    const auto partner = PlayerAttacksTestPeer::meleePlayer(party[0].actor, f.targets, 4, ahead);
    REQUIRE(partner);
    CHECK(AttackTargetId::fromId(partner->id).family() == Player);
    CHECK(AttackTargetId::fromId(partner->id).index() == 1);
    f.targets.players = {};
    f.close();
}

TEST_CASE("a charge's and a combo's ledger keys what they run into by its target id",
          "[game][player-attacks][blow-paths][target-rosters]") {
    BlowScene f;
    addGreatOne(f.root);
    loadFixtureWorld(f);
    f.open();
    REQUIRE(f.opponents.enemies().spawn(
        {.kind = kGruntKind, .position = {20, 0, 20}, .placed = true}, {}));
    spawnGreatOne(f, Vec3{40, 0, 0});
    placeGenerator(f, Vec3{-20, 0, 20});
    struct Touch {
        Vec3 at;
        f32 radius;
        BlowPath path;
        AttackTargetId key;
    };
    const std::array<Touch, 8> touches{{
        {{0, 0, 20}, 0.0f, BlowPath::Charge, AttackTargetId::of(TargetFamily::Wall, 0)},
        {{20, 0, 0}, 0.5f, BlowPath::Charge, AttackTargetId::of(TargetFamily::Barrel, 0)},
        {{-20, 0, 0}, 2.3f, BlowPath::Charge, AttackTargetId::of(TargetFamily::SafeRock, 0)},
        {{20, 0, 20}, 1.5f, BlowPath::Charge, AttackTargetId::of(TargetFamily::Enemy, 0)},
        {{20, 0, 20}, 1.5f, BlowPath::Combo, AttackTargetId::of(TargetFamily::Enemy, 0)},
        {{-20, 0, 20}, 1.0f, BlowPath::Combo, AttackTargetId::of(TargetFamily::Generator, 0)},
        {{40, 0, 0}, 1.5f, BlowPath::Charge, AttackTargetId::of(TargetFamily::Critter, 0)},
        {{40, 0, 0}, 1.5f, BlowPath::Combo, AttackTargetId::of(TargetFamily::Critter, 0)},
    }};
    for (const Touch& touch : touches) {
        CAPTURE(touch.key.id());
        f.players[0].rammed.clear();
        f.standBefore(touch.at, touch.radius);
        land(f, touch.path, {}, 25, 0, touch.at);
        CHECK(f.players[0].rammed == std::vector<usize>{touch.key.key()});
    }
    f.close();
}

TEST_CASE("a thrown weapon's flight meets the bottles lying about and the inert stops too, and "
          "carries the part it meets",
          "[game][player-attacks][blow-paths][target-rosters]") {
    BlowScene f;
    addGreatOne(f.root);
    loadFixtureWorld(f);
    f.open();
    spawnGreatOne(f, Vec3{40, 0, 0});
    const usize bottle = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "POT_RED", Vec3{40, 0, 40}));
    REQUIRE(f.world.placedItems().shootablePotions() == std::vector<usize>{bottle});
    struct Throw {
        Vec3 at;
        TargetFamily family;
        s32 index;
        s32 node;
    };
    // The chest is the first inert stop, the bottle a potion by its place among the items,
    // and the great one is met at its BODY part.
    const std::array<Throw, 3> throws{{
        {{-20, 0, -20}, TargetFamily::ItemStop, 0, -1},
        {{40, 0, 40}, TargetFamily::Potion, static_cast<s32>(bottle), -1},
        {{40, 0, 0}, TargetFamily::Critter, 0, 0},
    }};
    MissileSpec spec;
    spec.weight = 0;
    spec.radius = 0.25f;
    for (const Throw& shot : throws) {
        const auto family = static_cast<s32>(shot.family);
        CAPTURE(family);
        MissileLaunch launch;
        launch.owner = f.striker().player();
        launch.position = shot.at + Vec3{0, 1, -6};
        launch.velocity = Vec3{0, 0, 20};
        launch.spec = &spec;
        launch.damage = 10;
        REQUIRE(f.arsenal.missiles().launch(launch));
        PlayerAttacksTestPeer::flights(f.attacks, 0.5f, f.players, f.targets);
        const auto impacts = f.arsenal.missiles().takeImpacts();
        REQUIRE(impacts.size() == 1);
        const AttackTargetId met = AttackTargetId::fromId(impacts.front().target);
        CHECK(met.family() == shot.family);
        CHECK(met.index() == shot.index);
        CHECK(impacts.front().node == shot.node);
        if (shot.family == TargetFamily::Potion) {
            // The shot that met it breaks it.
            PlayerAttacksTestPeer::projectile(f.attacks, impacts.front(), f.players, f.targets);
            CHECK(f.world.placedItems().item(bottle).taken);
        }
    }
    f.close();
}

TEST_CASE("every blow path that reaches a statue wakes it by its own index",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path = GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area,
                                   BlowPath::PotionWave, BlowPath::ShieldRing);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    loadFixtureWorld(f);
    f.open();
    const CritterStatues& statues = f.opponents.statues();
    REQUIRE(statues.count() == 1);
    MissileTarget statue = statues.targets().front();
    statue.id += 9500;
    f.standBefore(statue.base, statue.radius);
    REQUIRE_FALSE(statues.woken(0));
    land(f, path, statue, 30, 0, statue.base);
    CHECK(statues.woken(0));
    f.close();
}

/** An imported level's fixtures, each struck by its own id: an ordinary one, one of an arena's
 * and a piece of scenery, far apart. */
class ImportedFixtures final : public ImportedLevel {
public:
    std::vector<s32> struck;
    std::vector<ImportedTarget> bodies{{0, {0, 0, 6}, 1, 3, true, false},
                                       {kImportedArenaBase, {20, 0, 0}, 1, 3, true, false},
                                       {kImportedSceneryBase, {-20, 0, 0}, 1, 3, true, false}};
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::vector<ImportedTarget> targets() const override { return bodies; }
    bool strike(s32 id, f32 /*power*/, u32 /*flags*/, s32 /*byPlayer*/) override {
        struck.push_back(id);
        return true;
    }
};

/** Hosts `level` as an imported level over a flat floor, the fixtures bound over it. */
void loadImportedWorld(BlowScene& f, std::shared_ptr<ImportedFixtures> level) {
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
         {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "imported-fixtures";
    const bool loaded = f.world.loadImported(
        f.device, f.root, ref, WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 30, 100}),
        std::move(collision), std::move(level));
    REQUIRE(loaded);
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio});
}

TEST_CASE("an imported level's fixtures are offered by their own ids, and no aim picks an "
          "arena's",
          "[game][player-attacks][blow-paths][target-rosters]") {
    BlowScene f;
    loadImportedWorld(f, std::make_shared<ImportedFixtures>());
    f.open();
    using enum TargetFamily;
    const std::vector<std::pair<TargetFamily, s32>> offered{
        {Imported, 0}, {Imported, kImportedArenaBase}, {Imported, kImportedSceneryBase}};
    CHECK(familiesOf(PlayerAttacksTestPeer::targets(f.attacks, f.targets)) == offered);
    CHECK(familiesOf(PlayerAttacksTestPeer::strikeTargets(f.attacks, f.targets)) == offered);
    auto aimed = offered;
    std::erase(aimed, std::pair{Imported, kImportedArenaBase});
    CHECK(familiesOf(PlayerAttacksTestPeer::acquisitionTargets(f.attacks, f.targets)) == aimed);
    f.close();
}

TEST_CASE("every blow path strikes an imported level's fixture by its own id, and a charge and "
          "a combo key it by its target id",
          "[game][player-attacks][blow-paths]") {
    const BlowPath path =
        GENERATE(BlowPath::Swing, BlowPath::Shot, BlowPath::Area, BlowPath::PotionWave,
                 BlowPath::ShieldRing, BlowPath::Combo, BlowPath::Charge);
    const auto way = static_cast<s32>(path);
    CAPTURE(way);
    BlowScene f;
    const auto level = std::make_shared<ImportedFixtures>();
    loadImportedWorld(f, level);
    f.open();
    const ImportedTarget& native = level->bodies.front();
    const MissileTarget fixture{kImportedTargetBase + native.id, native.base, native.radius,
                                native.height};
    f.standBefore(fixture.base, fixture.radius);
    f.players[0].rammed.clear();
    land(f, path, fixture, 25, 0, fixture.base);
    CHECK(level->struck == std::vector<s32>{native.id});
    const bool keyed = path == BlowPath::Combo || path == BlowPath::Charge;
    const usize key = AttackTargetId::of(TargetFamily::Imported, native.id).key();
    CHECK(f.players[0].rammed == (keyed ? std::vector<usize>{key} : std::vector<usize>{}));
    f.close();
}

TEST_CASE("the attacks' rules over target ids answer each family's first and last ids as they "
          "always have",
          "[game][player-attacks][blow-paths][target-id]") {
    // Over no level: no barrel stands and no imported level is open. A body one unit tall is
    // low enough for any family a swing reckons low.
    struct IdRules {
        s32 id;
        bool creature;    ///< isCreature
        bool switchTakes; ///< strikeSwitch, for a gas blow that sets nothing off
        bool immune;      ///< immuneToMagic
        bool low;         ///< a swing's low reckoning
    };
    constexpr std::array<IdRules, 29> kRules{{
        {-1, false, false, false, true},   {0, false, false, true, true},
        {999, false, false, true, true},   {1000, true, false, false, true},
        {1999, true, false, false, true},  {2000, false, false, false, true},
        {2999, false, false, false, true}, {3000, true, false, false, false},
        {3999, true, false, false, false}, {4000, true, false, false, false},
        {4999, true, false, false, false}, {5000, false, false, true, true},
        {5999, false, false, true, true},  {6000, false, false, true, true},
        {6999, false, false, true, true},  {7000, false, true, true, true},
        {7499, false, true, true, true},   {7500, false, true, true, true},
        {7999, false, true, true, true},   {8000, false, true, true, true},
        {8999, false, true, true, true},   {9000, false, true, true, true},
        {9499, false, true, true, true},   {9500, false, true, true, true},
        {9999, false, true, true, true},   {10000, false, true, true, false},
        {19999, false, true, true, false}, {20000, false, false, true, true},
        {52767, false, false, true, true},
    }};
    test::PlayerAttacksFixture f;
    for (const IdRules& rules : kRules) {
        CAPTURE(rules.id);
        const MissileTarget body{rules.id, Vec3{0, 0, 2}, 1, 1};
        CHECK(PlayerAttacksTestPeer::isCreature(rules.id) == rules.creature);
        CHECK(PlayerAttacksTestPeer::strikeSwitch(f.attacks, rules.id, damage::kGas) ==
              rules.switchTakes);
        CHECK(PlayerAttacksTestPeer::immuneToMagic(f.attacks, rules.id, f.targets) == rules.immune);
        CHECK(PlayerAttacksTestPeer::meleeLow(f.players[0].actor, body) == rules.low);
    }
}

} // namespace
