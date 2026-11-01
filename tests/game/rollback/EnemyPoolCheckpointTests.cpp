#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/rollback/EnemyPoolCheckpoint.h"
#include "game/rollback/RandomCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

EnemyAnimator::Checkpoint animation(usize nodes = 3) {
    EnemyAnimator::Checkpoint value;
    value.tree = "WIRE_ENEMY_TREE";
    value.current = EnemyAction::Throw2;
    value.requested = EnemyAction::HitReact1;
    value.walksIn = true;
    value.struck = true;
    value.powerStruck = true;
    value.threw = true;
    value.idleSeconds = 0.875f;
    value.throwInterval = 1.75f;
    value.idleFraction = 0.625f;
    value.player.sequence = 23;
    value.player.generation = 7;
    value.player.secondsPerFrame = 1.0f / 27;
    value.player.time = 0.010000000000000002;
    value.player.frame = 3.25f;
    value.player.speed = 1.5f;
    value.player.transitionLength = 0.2f;
    value.player.transitionTime = 0.0625f;
    value.player.playing = true;
    value.player.finished = true;
    value.player.held = true;
    value.player.smooth = true;
    value.frameBefore = 2.875f;
    value.generationBefore = 6;
    for (auto* target : {&value.pose, &value.previous, &value.presentationBefore}) {
        target->emplace();
        auto& pose = **target;
        pose.tree = value.tree;
        for (usize index = 0; index < nodes; ++index) {
            pose.nodes.push_back({"WIRE_NODE_" + std::to_string(index),
                                  static_cast<s32>(index) - 1,
                                  {0.5f, -0.25f, 0.75f}});
            pose.poses.push_back({{0.125f, 0.25f, -0.5f},
                                  {0.75f, -1.25f, 1.5f},
                                  {0.875f, 1.125f, 1.25f},
                                  NodePose::RotationOrder::PitchYawRoll});
        }
    }
    value.previous->poses[0].rotation.x = -0.75f;
    value.presentationBefore->poses[0].position.z = -1.75f;
    REQUIRE(value.valid());
    return value;
}

Enemies::Checkpoint varied() {
    Enemies::Checkpoint state;
    state.nextInstance = 0x1020304050607080ULL;
    Mt19937 random(17);
    for (s32 index = 0; index < 713; ++index) {
        (void)random();
    }
    state.random = random.checkpoint();
    state.stocks = {{kGruntKind, {17, 0.125f, 0.5f}}, {kRatKind, {29, 0.625f, 1.5f}}};
    std::ranges::sort(state.stocks, {}, &Enemies::Checkpoint::Stock::kind);
    state.scales = {1.125f, 1.25f, 1.5f, 1.75f, 0.75f, 0.5f, 9, true, 3};
    state.lookouts.points = {{-7, 2, 3}, {4, -5, 6}};
    state.lookouts.next = {1, 27}; // Native out-of-retained-range route remains representable.
    state.contactGridOrigin = {-31, 47};
    state.contactGridInverseWidth = 0.25f;
    state.bomber = 7;
    ViewVolume eye;
    eye.position = {1, 2, 3};
    eye.right = {1, 0, 0};
    eye.up = {0, 1, 0};
    eye.forward = {0, 0, 1};
    eye.sinX = 0.125f;
    eye.cosX = 0.25f;
    eye.sinY = 0.5f;
    eye.cosY = 0.75f;
    state.view = ViewSet::of(eye, Vec3{4, 5, 6});
    state.inView = 19;
    state.frame = 0x91827364;
    state.pendingTicks = 1;
    state.pendingSeconds = kStep;
    state.shrink = 0.5f;
    state.combatants = {{81, {2, 3, 5}, 0.75f, 1.25f}};
    for (usize index = 0; index < state.enemies.size(); ++index) {
        auto& body = state.enemies[index];
        body.instance = index + 1;
        body.state = static_cast<Enemies::State>(index % 4);
        body.kind = kGruntKind;
        body.tier = static_cast<s32>(index % 3) + 1;
        body.algorithm = static_cast<s32>(index % 32); // the ways stop at 31
        body.variant = 5;
        body.affinity = static_cast<s32>(index % 5);
        body.generator = 7;
        body.bred = true;
        body.birth = Enemies::Birth::Patrol;
        body.health = -0.5f;
        body.fullHealth = 111.25f;
        body.sight = 23.5f;
        body.radius = 1.25f;
        body.height = 7.5f;
        body.reach = 3.75f;
        body.position = {2, 3, 4};
        body.birthPosition = {-5, 6, -7};
        body.presentationPosition = {8, -9, 10};
        body.presentationYaw = 0.25f;
        body.presentationState = Enemies::State::Asleep;
        body.floor = Enemies::BodyState::Floor{31, {1, 2, 3}};
        body.yaw = 0.5f;
        body.mind = {kWanderWay, 0.125f, -0.25f, 0.375f, -2,   3,
                     4,          5,      6,      7,      8,    9,
                     true,       true,   true,   13,     true, {14, -15, 16, 17, 18}};
        body.push = {1, -2, 3};
        body.pushMagnitude = 3.5f;
        body.pushes = 2;
        body.target = 1;
        body.targetBefore = 2;
        body.targetDistance = 4.5f;
        body.weightedDistance = 6.5f;
        body.recognized = true;
        body.contact = 3;
        body.attackIndex = 2;
        body.attackCount = 19;
        body.stunTicks = 20;
        body.drainTicks = 21;
        body.endurance = 22;
        body.draining = true;
        body.bumpedWall = true;
        body.bumpedOther = true;
        body.blocked = true;
        body.otherSide = -1;
        body.expired = true;
        body.hurtPending = 2.5f;
        body.hurtFlags = 0x98765432;
        body.hurtDirection = {-1, 2, -3};
        body.hurtBy = 0;
        body.killed = true;
        body.onScreen = false;
        body.hitCount = 23;
        body.veil = 127.5f;
        body.veilClock = -24;
        body.flashSeconds = 0.125f;
        body.burnGap = 0.375f;
        body.deathSeconds = 0.625f;
        body.deathSkinFrames = 25;
        body.deathSkin = "DEATHLIGHT";
        body.animator = animation();
    }
    state.enemies.back().animator->dead = true;
    state.enemies.back().animator->current = EnemyAction::Dying;
    state.blows = {
        {1, kGruntKind, 2, 3.5f, true, 0x87654321, {1, 2, 3}, EnemyMeleeWard::HealthVamp}};
    state.losses = {{2, kRatKind, 3, 1, 37, true, {3, 2, 1}, 4}};
    state.bursts = {{{4, 5, 6}, 7.5f, 3, kGruntKind, true, 91}};
    state.births = {{{-8, 9, -10}, kGruntKind, 3}, {{11, -12, 13}, kRatKind, 1}};
    state.cues = {{EnemyCue::Kind::Yell, kRatKind, {7, 8, 9}}};
    state.feedback = {
        {kGruntKind, 3, 17, true, true, 0xABCDEF12, {-1, -2, -3}, 0.375f, 2.5f, 3, 92}};
    state.deathEvents = {{DeathEvent::Kind::Unmoved, 4, 2, DeathForm::Black, 8.5f, {2, 4, 6}, 93}};
    state.deathShots = {{5, kGarmBroodKind, {1, 3, 5}, {-2, -4, -6}}};
    state.tagged = {2, 1, 3};
    state.generatorEvents = {{11, EnemyGeneratorEvent::Kind::PatrolDetached}};
    REQUIRE(state.valid());
    return state;
}

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<u8>(value >> shift);
    }
}

usize findText(const std::vector<u8>& bytes, std::string_view text) {
    const auto found = std::search(bytes.begin(), bytes.end(), text.begin(), text.end());
    REQUIRE(found != bytes.end());
    return static_cast<usize>(found - bytes.begin());
}

TEST_CASE("enemy pool wire preserves every slot field queue clock and all three exact poses",
          "[netplay][rollback][enemy-pool-codec]") {
    const auto state = varied();
    const auto encoded = EnemyPoolCheckpoint::encode(state);
    REQUIRE(encoded);
    REQUIRE(encoded->size() <= EnemyPoolCheckpoint::kMaxBytes);
    const auto decoded = EnemyPoolCheckpoint::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(EnemyPoolCheckpoint::encode(*decoded) == encoded);
    // Pin little-endian version, fixed native slot count, MT words and a full
    // 64-bit lifetime counter independently of the decoder under test.
    CHECK(std::vector<u8>(encoded->begin(), encoded->begin() + 8) ==
          std::vector<u8>{static_cast<u8>(EnemyPoolCheckpoint::kVersion), 0, 0, 0,
                          static_cast<u8>(Enemies::kMost), 0, 0, 0}); // the slots
    const auto random = RandomCheckpoint::encode(state.random);
    REQUIRE(random);
    CHECK(std::equal(random->begin(), random->end(), encoded->begin() + 8));
    const usize instance = 8 + RandomCheckpoint::kBytes;
    CHECK(std::vector<u8>(encoded->begin() + static_cast<s64>(instance),
                          encoded->begin() + static_cast<s64>(instance + 8)) ==
          std::vector<u8>{0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10});
    CHECK(decoded->enemies[0].animator->player.time == state.enemies[0].animator->player.time);

    auto invisible = state;
    auto& savedAnimation = *invisible.enemies[0].animator;
    savedAnimation.tree = ""; // Legitimate native zero-node IT stance.
    savedAnimation.pose.reset();
    savedAnimation.previous.reset();
    savedAnimation.presentationBefore.reset();
    REQUIRE(invisible.valid());
    const auto zeroNodes = EnemyPoolCheckpoint::encode(invisible);
    REQUIRE(zeroNodes);
    CHECK(EnemyPoolCheckpoint::decode(*zeroNodes) == invisible);

    Enemies::Checkpoint smaller;
    smaller.most = 9;
    const auto restricted = EnemyPoolCheckpoint::encode(smaller);
    REQUIRE(restricted);
    CHECK(EnemyPoolCheckpoint::decode(*restricted) == smaller);
}

TEST_CASE("enemy pool wire rejects malformed lengths enums booleans RNG and floats",
          "[netplay][rollback][enemy-pool-codec]") {
    const auto plain = EnemyPoolCheckpoint::encode(Enemies::Checkpoint{});
    REQUIRE(plain);
    CHECK(plain->size() == 2648 + 364 * static_cast<usize>(Enemies::kMost)); // 364 a slot
    constexpr usize kPool = 8 + RandomCheckpoint::kBytes;
    constexpr usize kView = kPool + 84;
    constexpr usize kStocks = kView + 4;
    constexpr usize kLookouts = kStocks + 4;
    constexpr usize kCombatants = kLookouts + 4;
    constexpr usize kBlows = kCombatants + 4;
    constexpr usize kBody = kBlows + static_cast<usize>(10 * 4);
    const auto reject = [&](usize offset, u32 value) {
        auto bytes = *plain;
        word(bytes, offset, value);
        CHECK_FALSE(EnemyPoolCheckpoint::decode(bytes));
    };
    reject(0, EnemyPoolCheckpoint::kVersion + 1);
    reject(0, 1); // the previous schema omitted the native birth queue
    reject(0, 2); // cue source lifetimes were not represented
    reject(4, Enemies::kMost + 1);
    reject(8 + 624 * 4, 625);       // invalid MT cursor
    reject(kPool + 8, 0);           // no native slots
    reject(kPool + 12, 0x7FC00000); // health scale NaN
    reject(kPool + 40, 2);          // noncanonical boss boolean
    reject(kView, 2);
    reject(kStocks, 0xFFFFFFFF);
    reject(kLookouts, LookoutRoute::kMost + 1);
    reject(kCombatants, EnemyPoolCheckpoint::kMaxCombatants + 1);
    reject(kBlows, EnemyPoolCheckpoint::kMaxEvents + 1);
    reject(kBlows + static_cast<usize>(3 * 4), EnemyPoolCheckpoint::kMaxEvents + 1);
    reject(kBody + 8, 4);           // state past Dying
    reject(kBody + 8, 256);         // do not narrow an invalid u32 into enum zero
    reject(kBody + 28, 5);          // element past acid
    reject(kBody + 36, 2);          // bred boolean
    reject(kBody + 40, 3);          // birth past Patrol
    reject(kBody + 44, 0x7F800000); // infinite health
    reject(kBody + 112, 2);         // floor presence
    auto zeroRandom = *plain;
    std::fill(zeroRandom.begin() + 8, zeroRandom.begin() + 8 + static_cast<std::ptrdiff_t>(624 * 4),
              u8{0});
    CHECK_FALSE(EnemyPoolCheckpoint::decode(zeroRandom));

    const auto rich = EnemyPoolCheckpoint::encode(varied());
    REQUIRE(rich);
    const usize tree = findText(*rich, "WIRE_ENEMY_TREE");
    const usize animationFields = tree + std::string_view("WIRE_ENEMY_TREE").size();
    const usize node = findText(*rich, "WIRE_NODE_0");
    const auto rejectRich = [&](usize offset, u32 value) {
        auto bytes = *rich;
        word(bytes, offset, value);
        CHECK_FALSE(EnemyPoolCheckpoint::decode(bytes));
    };
    rejectRich(tree - 4, 65);
    rejectRich(animationFields, kEnemyActionCount);
    rejectRich(animationFields + 8, 2);        // walksIn
    rejectRich(animationFields + 40 + 40, 16); // animation reserved flags
    rejectRich(animationFields + 96, 2);       // three-pose presence
    rejectRich(animationFields + 100, EnemyPoolCheckpoint::kMaxNodes + 1);
    rejectRich(node - 4, 0xFFFFFFFF);
    rejectRich(node + std::string_view("WIRE_NODE_0").size(), 0); // first node cannot parent itself
    auto embeddedNull = *rich;
    embeddedNull[tree + 2] = 0;
    CHECK_FALSE(EnemyPoolCheckpoint::decode(embeddedNull));
    auto trailing = *rich;
    trailing.push_back(0);
    CHECK_FALSE(EnemyPoolCheckpoint::decode(trailing));
    CHECK_FALSE(EnemyPoolCheckpoint::decode(std::vector<u8>(EnemyPoolCheckpoint::kMaxBytes + 1)));
}

TEST_CASE("enemy birth wire validates every row and preserves the exhausted ordered queue",
          "[netplay][rollback][enemy-pool-codec]") {
    Enemies::Checkpoint state;
    state.births = {{{1, 2, 3}, kGruntKind, 3}};
    const auto bytes = EnemyPoolCheckpoint::encode(state);
    REQUIRE(bytes);
    CHECK(EnemyPoolCheckpoint::decode(*bytes) == state);
    constexpr usize kBirths =
        8 + RandomCheckpoint::kBytes + 84 + 4 + 4 + 4 + 4 + static_cast<usize>(3 * 4);
    for (const auto [offset, value] : std::array{
             std::pair{kBirths, u32{EnemyPoolCheckpoint::kMaxEvents + 1}},
             std::pair{kBirths + 4, u32{0x7FC00000}}, std::pair{kBirths + 16, u32{kEnemyKindCount}},
             std::pair{kBirths + 20, u32{0}}, std::pair{kBirths + 20, u32{4}}}) {
        auto bad = *bytes;
        word(bad, offset, value);
        CHECK_FALSE(EnemyPoolCheckpoint::decode(bad));
    }
    state.births.resize(EnemyPoolCheckpoint::kMaxEvents + 1, state.births.front());
    REQUIRE(state.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(state));
}

TEST_CASE("enemy cue diagnostics retain source lifetimes across a recycled slot",
          "[enemy-pool-codec][native-combat-audio]") {
    Enemies::Checkpoint state;
    state.nextInstance = 3;
    state.enemies[0].instance = 2;
    state.enemies[0].kind = kRatKind;
    state.cues.push_back({EnemyCue::Kind::Yell, kGruntKind, {1, 2, 3}, 0, 1});
    REQUIRE(state.valid()); // the previous occupant emitted this before its slot was reused
    const auto bytes = EnemyPoolCheckpoint::encode(state);
    REQUIRE(bytes);
    CHECK(EnemyPoolCheckpoint::decode(*bytes) == state);
    auto invalid = state;
    invalid.cues.front().instance = 0;
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.cues.front().enemy = -1;
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.cues.front().instance = state.nextInstance;
    CHECK_FALSE(invalid.valid());
    invalid = state;
    invalid.cues.front().instance = 2; // current source kind must agree
    CHECK_FALSE(invalid.valid());
    invalid.cues.front().enemyKind = kRatKind;
    REQUIRE(invalid.valid());
    CHECK(EnemyPoolCheckpoint::encode(invalid) != bytes);
}

TEST_CASE("enemy pool wire refuses every truncated prefix without partial state",
          "[netplay][rollback][enemy-pool-codec]") {
    auto state = Enemies::Checkpoint{};
    state.enemies[0].animator = animation();
    const auto bytes = EnemyPoolCheckpoint::encode(state);
    REQUIRE(bytes);
    for (usize size = 0; size < bytes->size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(EnemyPoolCheckpoint::decode(std::span{*bytes}.first(size)));
    }
    CHECK(EnemyPoolCheckpoint::decode(*bytes) == state);
}

TEST_CASE("enemy hit death and bomber diagnostics preserve extinct source identities",
          "[enemy-pool-codec][native-combat-audio]") {
    Enemies::Checkpoint state;
    state.nextInstance = 3;
    state.enemies[0].instance = 2;
    state.enemies[0].kind = kRatKind;
    state.feedback.push_back({.kind = kGruntKind, .enemy = 0, .instance = 1});
    state.deathEvents.push_back({.kind = DeathEvent::Kind::Killed, .enemy = 0, .instance = 1});
    state.bursts.push_back({.enemy = 0, .kind = kGruntKind, .instance = 1});
    REQUIRE(state.valid());
    const auto encoded = EnemyPoolCheckpoint::encode(state);
    REQUIRE(encoded);
    CHECK(EnemyPoolCheckpoint::decode(*encoded) == state);
    for (const s32 queue : {0, 1, 2}) {
        CAPTURE(queue);
        auto malformed = state;
        const auto edit = [&](s32 slot, u64 instance) {
            if (queue == 0) {
                malformed.feedback[0].enemy = slot;
                malformed.feedback[0].instance = instance;
            } else if (queue == 1) {
                malformed.deathEvents[0].enemy = slot;
                malformed.deathEvents[0].instance = instance;
            } else {
                malformed.bursts[0].enemy = slot;
                malformed.bursts[0].instance = instance;
            }
        };
        edit(-1, 1);
        CHECK_FALSE(malformed.valid());
        edit(Enemies::kMost, 1);
        CHECK_FALSE(malformed.valid());
        edit(0, state.nextInstance);
        CHECK_FALSE(malformed.valid());
        edit(0, 2); // a matching live identity cannot claim a different species
        CHECK_FALSE(malformed.valid());
        edit(0, 0); // legacy offline synthetic events remain representable
        REQUIRE(malformed.valid());
        CHECK(EnemyPoolCheckpoint::encode(malformed) != encoded);
    }
}

TEST_CASE(
    "enemy pool transport budgets refuse locally valid oversized envelopes without truncation",
    "[netplay][rollback][enemy-pool-codec]") {
    Enemies::Checkpoint state;
    state.blows.resize(EnemyPoolCheckpoint::kMaxEvents);
    state.losses.resize(EnemyPoolCheckpoint::kMaxEvents);
    state.feedback.resize(EnemyPoolCheckpoint::kMaxEvents);
    state.deathEvents.resize(EnemyPoolCheckpoint::kMaxEvents);
    state.combatants.resize(EnemyPoolCheckpoint::kMaxCombatants);
    for (usize index = 0;
         index < EnemyPoolCheckpoint::kMaxTotalNodes / EnemyPoolCheckpoint::kMaxNodes; ++index) {
        auto& enemy = state.enemies[index];
        enemy.animator = animation(EnemyPoolCheckpoint::kMaxNodes);
        auto& animator = *enemy.animator;
        animator.tree = std::string(TreePose::Checkpoint::kMaxName, 'T');
        for (auto* saved : {&animator.pose, &animator.previous, &animator.presentationBefore}) {
            (**saved).tree = animator.tree;
            for (auto& node : (**saved).nodes) {
                node.name = std::string(TreePose::Checkpoint::kMaxName, 'N');
            }
        }
    }
    REQUIRE(state.valid());
    const auto encoded = EnemyPoolCheckpoint::encode(state);
    REQUIRE(encoded);
    CHECK(encoded->size() < EnemyPoolCheckpoint::kMaxBytes);
    CHECK(EnemyPoolCheckpoint::decode(*encoded) == state);
    // Insert a syntactically complete 1025th event in the final tagged queue.
    // Each individual count is legal; only the aggregate pre-allocation budget
    // can reject it without accepting a silently incomplete event history.
    auto tooManyEvents = *encoded;
    constexpr usize kQueues = 8 + RandomCheckpoint::kBytes + 84 + 4 + 4 + 4 + 4 +
                              EnemyPoolCheckpoint::kMaxCombatants * 24;
    constexpr usize kTagged =
        kQueues + static_cast<usize>(8 * 4) + EnemyPoolCheckpoint::kMaxEvents * (40 + 36 + 56 + 40);
    word(tooManyEvents, kTagged, 1);
    tooManyEvents.insert(tooManyEvents.begin() + static_cast<s64>(kTagged + 4), 4, u8{0});
    CHECK_FALSE(EnemyPoolCheckpoint::decode(tooManyEvents));
    auto overflow = state;
    overflow.blows.emplace_back();
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
    overflow = state;
    overflow.tagged.push_back(0);
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
    overflow = state;
    overflow.combatants.emplace_back();
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
    overflow = state;
    overflow.enemies[16].animator = animation(1);
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
    overflow = {};
    overflow.enemies[0].animator = animation(EnemyPoolCheckpoint::kMaxNodes + 1);
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
    overflow = {};
    overflow.enemies[0].animator = animation();
    overflow.enemies[0].animator->previous->nodes[0].rest.x += 1;
    REQUIRE(overflow.valid());
    CHECK_FALSE(EnemyPoolCheckpoint::encode(overflow));
}

struct NativePool {
    test::FakeRenderDevice device;
    Enemies enemies;
    std::array<EnemyView, 2> players;
    std::array<Obstacle, 1> obstacles;

    explicit NativePool(bool reverse) {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        enemies.open(device, root, nullptr, Enemies::kMost, {}, 731);
        std::array kinds{kGruntKind, kRatKind, kItKind};
        if (reverse) {
            std::ranges::reverse(kinds);
        }
        for (const s32 kind : kinds) {
            REQUIRE(enemies.loadKind(kind));
        }
        players[0].player = 0;
        players[0].position = {-8, 0, 0};
        players[1].player = 1;
        players[1].position = {8, 0, 0};
        obstacles[0].centre = {0, 0, 12};
        obstacles[0].halfAcross = 3;
        obstacles[0].halfAlong = 2;
        obstacles[0].height = 6;
        for (s32 index = 0; index < Enemies::kMost; ++index) {
            EnemySpawn spawn;
            spawn.kind = index % 7 == 0 ? kItKind : (index % 2 == 0 ? kGruntKind : kRatKind);
            spawn.placed = true;
            spawn.tier = index % 3 + 1;
            spawn.generator = index % 5;
            spawn.algorithm = index % 2 == 0 ? kWanderWay : kChaseWay;
            spawn.position = {
                static_cast<f32>(index % 5 - 2) * 9, 0,
                // NOLINTNEXTLINE(bugprone-integer-division): integer division picks the grid row
                static_cast<f32>(index / 5 - 2) * 9};
            REQUIRE(enemies.spawn(spawn, {}) == index);
        }
    }

    void step(s32 tick) {
        players[0].position.z = static_cast<f32>(tick % 9) * 0.25f;
        players[1].position.x = 8 - static_cast<f32>(tick % 7) * 0.125f;
        if (tick == 5 || tick == 31) {
            enemies.hurt(2, {.damage = 2,
                             .flags = EnemyHit::kKnockBack,
                             .direction = {1, 0, 0},
                             .player = 0,
                             .where = std::nullopt});
        }
        if (tick == 38) {
            enemies.hurt(
                4, {.damage = 1000, .flags = EnemyHit::kMagic, .player = 1, .where = std::nullopt});
        }
        enemies.setShrink(tick >= 20 && tick < 29 ? 0.5f : 1);
        enemies.update(1, kStep, players, obstacles, nullptr, 1, tick >= 42 && tick < 47);
    }
};

TEST_CASE("transported full native enemy pools restore and continue without loading resources",
          "[netplay][rollback][enemy-pool-codec][assets]") {
    NativePool host(false);
    NativePool guest(true);
    const auto hostUploads = host.device.texturesCreated;
    const auto guestUploads = guest.device.texturesCreated;
    const auto hostUpdates = host.device.textureUpdates;
    const auto guestUpdates = guest.device.textureUpdates;
    for (s32 tick = 0; tick < 17; ++tick) {
        host.step(tick);
    }
    const auto correction = host.enemies.checkpoint();
    REQUIRE(correction);
    REQUIRE(correction->pendingTicks == 1);
    REQUIRE(correction->generatorEvents.size() == Enemies::kMost);
    const auto bytes = EnemyPoolCheckpoint::encode(*correction);
    REQUIRE(bytes);
    const auto decoded = EnemyPoolCheckpoint::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(*decoded == *correction);
    REQUIRE(guest.enemies.restore(*decoded));
    CHECK(guest.enemies.checkpoint() == correction);
    for (s32 tick = 17; tick < 81; ++tick) {
        CAPTURE(tick);
        host.step(tick);
        guest.step(tick);
        CHECK(host.enemies.checkpoint() == guest.enemies.checkpoint());
        if (tick % 9 == 0) {
            const auto state = host.enemies.checkpoint();
            REQUIRE(state);
            const auto refresh = EnemyPoolCheckpoint::encode(*state);
            REQUIRE(refresh);
            const auto received = EnemyPoolCheckpoint::decode(*refresh);
            REQUIRE(received);
            REQUIRE(guest.enemies.restore(*received));
        }
    }
    CHECK(host.device.texturesCreated == hostUploads);
    CHECK(guest.device.texturesCreated == guestUploads);
    CHECK(host.device.textureUpdates == hostUpdates);
    CHECK(guest.device.textureUpdates == guestUpdates);

    // Names are value syntax, never permission to bind an untrusted skeleton.
    auto tampered = *decoded;
    // an array iterator is a pointer only on some libraries
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto animated = std::ranges::find_if(
        tampered.enemies, [](const auto& enemy) { return enemy.animator && enemy.animator->pose; });
    REQUIRE(animated != tampered.enemies.end());
    auto& animator = *animated->animator;
    animator.tree = "UNLOADED_ENEMY";
    animator.pose->tree = animator.tree;
    animator.previous->tree = animator.tree;
    animator.presentationBefore->tree = animator.tree;
    REQUIRE(tampered.valid());
    const auto changedBytes = EnemyPoolCheckpoint::encode(tampered);
    REQUIRE(changedBytes);
    const auto changed = EnemyPoolCheckpoint::decode(*changedBytes);
    REQUIRE(changed);
    const auto before = guest.enemies.checkpoint();
    CHECK_FALSE(guest.enemies.restore(*changed));
    CHECK(guest.enemies.checkpoint() == before);
    CHECK(guest.device.texturesCreated == guestUploads);
}
} // namespace
