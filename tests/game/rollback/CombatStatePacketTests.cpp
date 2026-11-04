#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/characters/Progression.h"
#include "game/hud/HelpMessages.h"
#include "game/party/AttackCheckpointFixture.h"
#include "game/rollback/CombatStatePacket.h"
#include "game/rollback/PlayerHealthCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Packet = CombatStatePacket;

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (usize index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

Packet::State emptyState() {
    Packet::State state;
    state.combat.health = Mt19937{0x12345678}.checkpoint();
    REQUIRE(state.valid());
    return state;
}

struct NativeFixture {
    test::AttackCheckpointFixture native;
    PlayerRuntimeCheckpoint runtime{native.players[0], native.classes.stats(0)};
    std::array<PlayerRuntimeCheckpoint*, 1> players{&runtime};
    PlayerHealth health;
    LevelWatch levels;
    CombatRollbackCheckpoint owner{players,
                                   health,
                                   levels,
                                   native.attacks,
                                   native.arsenal,
                                   native.effects,
                                   {native.missileResources, {}, native.resources}};

    NativeFixture() {
        auto& actor = native.players[0].actor;
        // Native MAGICHEALTH attachments begin at level 75; ordinary lower-level
        // casts still produce potion areas, but have no caster-following effect.
        actor.save().progress().experience = levelExperience(75);
        actor.save().progress().inventory.addPotions(2, 3);
        native.players[0].entrySave = actor.save();
        REQUIRE(experienceLevel(actor.save().experience()) == 75);
        levels.observe(actor.player(), 75);
        native.attacks.usePotion(0, native.players);
        native.attacks.shieldPotion(0, native.players);
        MissileLaunch launch;
        launch.owner = 3;
        launch.position = {0, 4, 0};
        launch.velocity = {0, 2, 4};
        launch.spec = &MissileSpec::of(0);
        launch.damage = 13;
        REQUIRE(native.arsenal.missiles().launch(launch));
    }
    Packet::State state() const {
        const auto current = owner.checkpoint();
        REQUIRE(current);
        Packet::State result;
        result.combat = *current;
        REQUIRE(result.valid());
        return result;
    }
    void step(f32 seconds) {
        native.players[0].figure->animate(0, 1, seconds, PlayerDeed::None);
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        native.attacks.updateStrikes(seconds, native.players, native.targets);
        native.attacks.updateShields(seconds, native.players, native.targets);
        native.effects.update(seconds);
    }
};

ParticleField::Checkpoint particles() {
    ParticleDescriptor descriptor;
    descriptor.delay = 1;
    descriptor.emitFrames = 20;
    descriptor.fadeFrames = 5;
    descriptor.forever = true;
    descriptor.dynamic = true;
    descriptor.angle = 0.25f;
    descriptor.direction = {0, 1, 0};
    descriptor.volume = {1, 2, 3};
    descriptor.rate = {1, 2, 3, 4};
    descriptor.rateRandom = 0.1f;
    descriptor.gravity = -0.01f;
    descriptor.drag = 0.02f;
    descriptor.speed = 0.3f;
    descriptor.maxParticles = 40;
    descriptor.maxDirections = 5;
    descriptor.maxPositions = 6;
    descriptor.particleLife = 30;
    descriptor.particleFade = 7;
    descriptor.red = {255, 210, 180, 30};
    descriptor.green = {210, 180, 140, 20};
    descriptor.blue = {180, 140, 100, 10};
    descriptor.alpha = {255, 240, 230, 0};
    descriptor.width = {0.1f, 0.2f, 0.3f, 0.01f};
    descriptor.texture = "PARTICLE";
    descriptor.additive = true;
    descriptor.sorted = true;
    descriptor.depthTest = false;
    descriptor.depthWrite = false;
    ParticleEmitter emitter;
    emitter.start(descriptor, glm::translate(Mat4{1}, {1, 2, 3}), 123);
    emitter.step(3);
    emitter.setSpriteScale(1.25f);
    emitter.setEmitting(false);
    ParticleField::Checkpoint result;
    result.entries.push_back({emitter.checkpoint(), 7});
    result.frameRemainder = 0.25f;
    result.lastAdvance = 0.5f;
    result.heatFrames = 19.75;
    REQUIRE(result.valid());
    REQUIRE_FALSE(result.entries[0].emitter.particles.empty());
    return result;
}

void decorate(Packet::State& state) {
    auto& combat = state.combat;
    auto& attacks = combat.attacks;
    const u32 effectId = combat.effects.effects[0].id;
    attacks.items.push_back(
        {PlayerAttacksCheckpoint::Item::FireBreath, 0, effectId, 0.2f, 2, true, {5, 7}});
    attacks.attachments.push_back({0, effectId, 0, glm::translate(Mat4{1}, {1, 2, 3}), true});
    attacks.particles.push_back({0, 0, effectId, "BODY", {3, 4, 5}});
    auto& potion = attacks.potions[0];
    potion.elapsed = 0.125f;
    potion.hit = {13, 14};
    potion.blessed = {3};
    potion.struck = true;
    auto& shield = attacks.shields[0];
    shield.marks = {{0, 13, 0}, {1, 17, 0.1f}, {2, 19, 0.2f}, {3, 0, 0.25f}, {4, 23, 0.125f}};
    shield.blessed = {3};
    MoveStrikes::Strike strike;
    strike.id = 11;
    strike.owner = 3;
    strike.flies = true;
    strike.collidesWorld = false;
    strike.position = {4, 5, 6};
    strike.facing = {1, 0, 0};
    strike.speed = 7;
    strike.radius = 8;
    strike.arc = 0.3f;
    strike.damage = 9;
    strike.delayLeft = 0.1f;
    strike.launchIn = 0.2f;
    strike.secondsLeft = 3;
    strike.damageTime = 2;
    attacks.strikes = {{strike}, 13};
    strike.id = 12;
    strike.flies = false;
    strike.expanding = true;
    attacks.strikes.strikes.push_back(strike);
    attacks.strikeEffects.push_back({11, effectId});
    attacks.strikeSources.push_back(
        {11, 0, 2, {{101, 0.2f}, {104, -0.01f}}, MultiplayerMode::Hurt, 0x1234});
    REQUIRE(attacks.valid());

    auto& pool = combat.arsenal.missiles;
    pool.history.clear();
    auto& flight = pool.missiles[0];
    flight.previousPosition = {5, 4, 3};
    flight.previousVelocity = {2, 1, 0};
    flight.previousTumble = 0.7f;
    flight.previousAge = 0.01f;
    flight.tumble = 0.8f;
    flight.age = 0.025f;
    flight.presentationCaptured = true;
    flight.potency = 2.5f;
    flight.scale = 1.5f;
    flight.wallSound = MissileWallSound::Ricochet;
    flight.flags = 0x12345678;
    flight.pierced = {12, 8, 41};
    flight.release = MissileRelease{0, 52, true, 2, 0};
    flight.streakColor = {1, 2, 3, 4};
    flight.streakForward = 2;
    flight.multiplayer = MultiplayerMode::Hurt;
    flight.playerHitGap = 0.4f;
    flight.breaksPotions = false;
    flight.playerContacts = {{3, -0.1f}};
    PlayerMissiles::VisualHistory history;
    history.instance = flight.instance;
    history.owner = 3;
    history.character = 0;
    history.release = flight.release;
    history.flight.birth.position = {0, 4, 0};
    history.flight.birth.velocity = {0, 2, 4};
    history.flight.activeSteps = 3;
    history.flight.totalSteps = 3;
    history.placement = glm::translate(Mat4{1}, {4, 5, 6});
    history.direction = {0, 0, 1};
    pool.history.push_back(history);
    history.instance = 99;
    history.release.reset();
    history.potion = 2;
    history.potionRelease = MissilePotionRelease{55};
    history.flight.totalSteps = 5;
    history.flight.retiring = true;
    pool.history.push_back(history);
    history.instance = 98;
    history.potion = 0;
    history.potionRelease.reset();
    history.release = MissileRelease{0, 56, false, 0, (1U << 16) | 4U};
    pool.history.push_back(history);
    pool.nextInstance = 100;
    pool.ricochetIn = 0.6f;
    PlayerMissiles::Checkpoint::Impact impact;
    impact.position = {1, 2, 3};
    impact.owner = 3;
    impact.potion = 4;
    impact.potency = 5;
    impact.damage = 6;
    impact.target = 101;
    impact.effectResource = 1;
    impact.wallSound = MissileWallSound::FiveWay;
    impact.flags = 0x123;
    impact.node = 2;
    impact.worldObject = 3;
    impact.liquid = true;
    impact.player = 3;
    impact.direction = {1, 0, 0};
    impact.stun = true;
    impact.playerHitGap = 0.75f;
    pool.impacts.push_back(impact);
    REQUIRE(pool.valid());

    auto& visual = combat.effects.effects[0];
    visual.state.trails = particles();
    visual.state.particles.field = particles();
    visual.state.particles.nodes = {{0, 7}};
    visual.state.attachment = glm::translate(Mat4{1}, {7, 8, 9});
    visual.state.flightDirection = Vec3{0, 1, 0};
    visual.state.particleTextureSlot = 9;
    auto& setting = visual.state.setting;
    setting.stretch = {1, 2, 3};
    setting.fadeSeconds = 0.7f;
    setting.yaw = 0.2f;
    setting.velocity = {0.1f, 0.2f, 0.3f};
    setting.persistent = true;
    setting.missingObjectsAreEmpty = true;
    setting.additive = true;
    setting.morphIn = 0.5f;
    setting.holdForMorph = true;
    setting.shrinks = true;
    setting.unlit = true;
    setting.depthWrite = false;
    setting.tint = {8, 16, 32, 64};
    setting.alpha = 0.7f;
    setting.playbackRate = 1.2f;
    setting.light = EffectTrees::Light{{0.1f, 0.3f, 0.5f}, 3, true};
    visual.provenance = EffectTrees::Provenance{3, 77, 5, 6};
    visual.previousTransform = glm::translate(Mat4{1}, {4, 3, 2});
    visual.previousDirection = Vec3{1, 0, 0};
    visual.previousTree = true;
    visual.previousGeneration = 6;
    visual.previousFrame = 3.25f;
    visual.presentationCaptured = true;
    combat.effects.presentationAdvanced = true;
    REQUIRE(combat.effects.valid());

    auto& adapter = state.adapter;
    adapter.hitStreak[3] = 5;
    adapter.lessons = {{HelpMessages::kDestroyGenerators, 3}};
    adapter.generatorRewards = {{3, 41, true}, {3, 17, false}};
    adapter.rockHits = {{5, 7.5f}, {2, 13.25f}};
    adapter.hearers = {{1, 2, 3}, {4, 5, 6}};
    adapter.cueEffects = {effectId};
    adapter.shrink = 0.5f;
    adapter.bagSeed = 0xFEDCBA98;
    adapter.critters.banks = {{1, CombatantKind::Golem, "GOLEM", "GOLEM", {31, 0.25f, 0.5f}}};
    adapter.projectiles.catalog = {{1, CombatantKind::Golem, "GOLEM", {}}};
    adapter.critterExperienceOwed[3] = 0.375f;
    adapter.critterEffects.push_back(
        {effectId, 0, false, {1, 2, 3}, "ROOT", {4, 5, 6}, true, {0.1f, 0.2f}, 3});
    adapter.moveEffects.push_back({effectId, 0, false});
    adapter.barrelRams.push_back({7, 3.5f});
    adapter.carried[0] = 0;
    adapter.carried.back() = 41;
    adapter.bags.push_back({7, {}, {3, 7, 9}, -2.5f, 0.75f, -0.125f, 0.5f, effectId, true});
    adapter.bags.push_back(
        {std::nullopt, "GARGEAGL", {-3, 5, -9}, 17, 0.125f, 0.25f, 1.125f, effectId, false});
    adapter.critters.actors[0].parts[0].actor.moveTimes = {-std::numeric_limits<f32>::infinity(),
                                                           -0.0f, 12.5f};
    adapter.critters.actors[0].parts[0].actor.patternTimes = {0, 9.5f};

    CritterStatues::Placement placement;
    placement.kind = CombatantKind::Golem;
    placement.form = "STONE";
    placement.instance.info = 3;
    placement.instance.minPlayers = 2;
    placement.instance.flags = 7;
    placement.instance.name = "PLACED";
    placement.instance.position = {2, 3, 4};
    placement.instance.rotation = {0.1f, 0.2f, 0.3f};
    placement.instance.params = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    placement.instance.collision.push_back(
        {{0, 1, 0}, {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}}});
    placement.radius = 3;
    placement.height = 5;
    placement.viewRadius = 10;
    placement.sight = 12;
    placement.activeOn = 4;
    placement.onFloor = false;
    placement.carried = 8;
    placement.support = 17;
    placement.supportLocal = glm::translate(Mat4{1}, Vec3{2, 3, 4});
    auto& statues = adapter.statues;
    statues.roster.push_back({1, placement});
    placement.onFloor = true;
    placement.kind = CombatantKind::Unknown;
    placement.enemy = EnemySpawn{.kind = kDeathKind,
                                 .tier = 2,
                                 .algorithm = 3,
                                 .position = {1, 2, 3},
                                 .direction = {1, 0, 0},
                                 .clearance = 0.5f,
                                 .generator = 2,
                                 .placed = true,
                                 .patrolBirth = true,
                                 .allBirthDirections = true,
                                 .asleep = true,
                                 .throwInterval = 1.25f,
                                 .priority = EnemySpawn::Priority::FreeSlotOnly,
                                 .sight = 9};
    statues.roster.push_back({2, placement});
    statues.risen.push_back(placement);
    CritterStatues::Checkpoint::Body statue;
    statue.instance = 1;
    statue.woken = true;
    statue.rising = true;
    statue.ticksLeft = 5;
    auto& figure = statue.figure;
    figure.pose.tree = "STONE";
    figure.pose.nodes = {{"ROOT", -1, {1, 2, 3}}};
    figure.pose.poses = {
        {{0.1f, 0.2f, 0.3f}, {4, 5, 6}, {1, 2, 3}, NodePose::RotationOrder::PitchYawRoll}};
    figure.presentationPose = figure.pose;
    figure.textures = {17, 0.25f, 0.5f};
    figure.particles = visual.state.particles;
    figure.poseSequence = 2;
    figure.poseFrame = 3.5f;
    figure.poseGeneration = 4;
    figure.continuity = 5;
    figure.previousTransform = glm::translate(Mat4{1}, {6, 7, 8});
    figure.previousFrame = 2.5f;
    figure.previousGeneration = 3;
    figure.presentationCaptured = true;
    figure.presentationAdvanced = true;
    figure.textureSequence = 3;
    figure.textureFrame = 4;
    figure.position = {6, 7, 8};
    figure.yaw = 0.4f;
    figure.transform = glm::translate(Mat4{1}, {9, 8, 7});
    figure.placement = glm::translate(Mat4{1}, {6, 5, 4});
    figure.index = CritterStatues::kActiveSequence;
    figure.holdPose = true;
    statues.statues.push_back(statue);
    statues.contactGridOrigin = {-40, 20};
    statues.contactGridInverseWidth = 0.125f;
    statues.nextInstance = 3;
    REQUIRE(statues.valid());
    REQUIRE(state.valid());
}

TEST_CASE("combat packet round trips complete nonempty native owners and adapter values",
          "[netplay][combat-state-packet]") {
    const NativeFixture fixture;
    auto state = fixture.state();
    decorate(state);
    const auto encoded = Packet::encode(state);
    REQUIRE(encoded);
    REQUIRE(encoded->size() < Packet::kMaxBytes);
    CHECK((*encoded)[0] == 'C');
    CHECK((*encoded)[1] == 'B');
    CHECK((*encoded)[2] == 'S');
    CHECK((*encoded)[3] == 'T');
    CHECK((*encoded)[4] == Packet::kVersion);
    const auto decoded = Packet::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(Packet::encode(*decoded) == encoded);
    for (u32 previous = 1; previous < Packet::kVersion; ++previous) {
        auto obsolete = *encoded;
        word(obsolete, 4, previous);
        CHECK_FALSE(Packet::decode(obsolete));
    }
    // Neither serialization nor decoding touches the actual party/pool owners.
    CHECK(fixture.state() != state);
    CHECK(fixture.native.helps.empty());
}

TEST_CASE("decoded combat packet restores real attack continuation without replaying effects",
          "[netplay][combat-state-packet]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    NativeFixture fixture;
    for (s32 tick = 0; tick < 5; ++tick) {
        fixture.step(seconds);
    }
    const auto baseline = fixture.state();
    REQUIRE_FALSE(baseline.combat.attacks.potions.empty());
    REQUIRE_FALSE(baseline.combat.attacks.shields.empty());
    REQUIRE_FALSE(baseline.combat.arsenal.casts.empty());
    REQUIRE_FALSE(baseline.combat.arsenal.missiles.missiles.empty());
    const auto bytes = Packet::encode(baseline);
    REQUIRE(bytes);
    const auto decoded = Packet::decode(*bytes);
    REQUIRE(decoded);
    std::vector<CombatRollbackCheckpoint::State> expected;
    for (s32 tick = 0; tick < 40; ++tick) {
        fixture.step(seconds);
        expected.push_back(fixture.state().combat);
    }
    const auto uploads = fixture.native.device.texturesCreated;
    const auto helps = fixture.native.helps;
    REQUIRE(fixture.owner.restore(decoded->combat));
    CHECK(fixture.state() == baseline);
    CHECK(fixture.native.helps == helps);
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick, seconds);
        fixture.step(seconds);
        CHECK(fixture.state().combat == expected[tick]);
    }
    CHECK(fixture.native.device.texturesCreated == uploads);
}

TEST_CASE("combat loot fields enforce canonical indices scalars strings and allocation bounds",
          "[netplay][combat-state-packet][opponent-loot-checkpoint]") {
    auto state = emptyState();
    state.combat.effects.nextId = 8;
    state.adapter.carried.front() = 0;
    state.adapter.carried.back() = 65535;
    state.adapter.bags.push_back(
        {42, {}, {731.125f, -113.25f, 91.5f}, -17.75f, 0.8125f, -0.375f, 0.5625f, 7, true});
    state.adapter.bags.push_back({std::nullopt, "GARGEAGL", {}, 20, 0, 0, 0.5f, 6, false});
    const auto encoded = Packet::encode(state);
    REQUIRE(encoded);
    REQUIRE(Packet::decode(*encoded) == state);
    const auto locate = [&](std::initializer_list<u32> words) {
        std::vector<u8> pattern(words.size() * 4);
        usize offset = 0;
        for (const u32 value : words) {
            word(pattern, offset, value);
            offset += 4;
        }
        const auto found =
            std::search(encoded->begin(), encoded->end(), pattern.begin(), pattern.end());
        REQUIRE(found != encoded->end());
        REQUIRE(std::search(found + 1, encoded->end(), pattern.begin(), pattern.end()) ==
                encoded->end());
        return static_cast<usize>(found - encoded->begin());
    };
    SECTION("nonfinite flight scalar") {
        auto bytes = *encoded;
        word(bytes, locate({std::bit_cast<u32>(731.125f), std::bit_cast<u32>(-113.25f)}),
             0x7FC00000);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("noncanonical landed boolean") {
        auto bytes = *encoded;
        const usize location = locate({std::bit_cast<u32>(0.5625f), 7});
        bytes[location + 8] = 2;
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("unknown effect identifier") {
        auto bytes = *encoded;
        word(bytes, locate({std::bit_cast<u32>(0.5625f), 7}) + 4, state.combat.effects.nextId);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("bounded reward name") {
        state.adapter.bags.back().name.assign(65, 'G');
        CHECK_FALSE(Packet::encode(state));
        state.adapter.bags.back().name = std::string{"GARG\0EAGL", 9};
        CHECK_FALSE(Packet::encode(state));
    }
    SECTION("wire record cap never truncates a valid native bag queue") {
        state.adapter.bags.resize(Packet::kMaxRecords + 1, state.adapter.bags.front());
        REQUIRE(state.adapter.valid());
        CHECK_FALSE(Packet::encode(state));
    }
    SECTION("truncated final bag never becomes an empty queue") {
        for (usize tail = 1; tail <= 40; ++tail) {
            const usize size = encoded->size() - tail;
            auto bytes = std::vector<u8>{encoded->begin(),
                                         encoded->begin() + static_cast<std::ptrdiff_t>(size)};
            word(bytes, 8, static_cast<u32>(size));
            CHECK_FALSE(Packet::decode(bytes));
        }
    }
}

TEST_CASE("combat packet rejects every truncation trailing data and header disagreement",
          "[netplay][combat-state-packet]") {
    const auto encoded = Packet::encode(emptyState());
    REQUIRE(encoded);
    for (usize length = 0; length < encoded->size(); ++length) {
        CAPTURE(length);
        CHECK_FALSE(Packet::decode(std::span{*encoded}.first(length)));
        if (length >= Packet::kHeaderBytes) {
            std::vector<u8> shortened(encoded->begin(),
                                      encoded->begin() + static_cast<std::ptrdiff_t>(length));
            word(shortened, 8, static_cast<u32>(length));
            CHECK_FALSE(Packet::decode(shortened));
        }
    }
    for (const usize offset : {usize{0}, usize{4}, usize{8}, usize{12}}) {
        CAPTURE(offset);
        auto bytes = *encoded;
        word(bytes, offset, 0xFFFFFFFF);
        CHECK_FALSE(Packet::decode(bytes));
    }
    auto extra = *encoded;
    extra.push_back(0);
    CHECK_FALSE(Packet::decode(extra));
    word(extra, 8, static_cast<u32>(extra.size()));
    CHECK_FALSE(Packet::decode(extra));
}

TEST_CASE("combat packet bounds allocations before reading nested records",
          "[netplay][combat-state-packet]") {
    const auto encoded = Packet::encode(emptyState());
    REQUIRE(encoded);
    // No player leaves the fixed RNG immediately after the first vector count.
    constexpr usize kLevels = Packet::kHeaderBytes + 4 + PlayerHealthCheckpoint::kBytes;
    const usize offset = GENERATE_COPY(Packet::kHeaderBytes, kLevels, kLevels + 8, kLevels + 12);
    auto bytes = *encoded;
    word(bytes, offset, 0xFFFFFFFF);
    CHECK_FALSE(Packet::decode(bytes));
    bytes = *encoded;
    word(bytes, kLevels + 4, 2); // Canonical attack-owner boolean, not a bitmask.
    CHECK_FALSE(Packet::decode(bytes));
    bytes = *encoded;
    word(bytes, Packet::kHeaderBytes, 5); // Smaller than global cap, still too many players.
    CHECK_FALSE(Packet::decode(bytes));
}

TEST_CASE("combat packet rejects malformed or cross-party state without omission",
          "[netplay][combat-state-packet]") {
    const NativeFixture fixture;
    auto baseline = fixture.state();
    decorate(baseline);
    auto bad = baseline;
    const s32 mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                                  19, 20, 21, 22, 23);
    CAPTURE(mutation);
    switch (mutation) {
    case 0: bad.adapter.generatorRewards[0].player = 0; break;
    case 1: bad.adapter.lessons[0].second = 0; break;
    case 2: bad.adapter.hitStreak[0] = 1; break;
    case 3: bad.adapter.cueEffects[0] = bad.combat.effects.nextId; break;
    case 4: bad.combat.health.words.fill(0); break;
    case 5:
        bad.combat.attacks.items[0].item = static_cast<PlayerAttacksCheckpoint::Item>(255);
        break;
    case 6: bad.combat.arsenal.missiles.missiles[0].playerContacts[0].player = 0; break;
    case 7:
        bad.combat.effects.effects[0].state.setting.alpha = std::numeric_limits<f32>::quiet_NaN();
        break;
    case 8: bad.adapter.statues.statues[0].figure.pose.nodes[0].parent = 0; break;
    case 9: bad.adapter.critters.banks[0].kind = CombatantKind::Boss; break;
    case 10:
        bad.combat.arsenal.missiles.missiles[0].wallSound = static_cast<MissileWallSound>(255);
        break;
    case 11:
        bad.combat.effects.effects[0].state.trails.entries[0].emitter.phase =
            static_cast<ParticleEmitter::Phase>(255);
        break;
    case 12:
        bad.combat.effects.effects[0].state.trails.heatFrames =
            std::numeric_limits<f64>::infinity();
        break;
    case 13:
        bad.adapter.statues.statues[0].figure.position.x = std::numeric_limits<f32>::infinity();
        break;
    case 14: bad.adapter.critters.banks.push_back(bad.adapter.critters.banks[0]); break;
    case 15: bad.combat.attacks.shields[0].marks[0].kind = 6; break;
    case 16: bad.combat.attacks.shields[0].marks[0].wait = 0.2501f; break;
    case 17:
        bad.combat.attacks.shields[0].marks.push_back(bad.combat.attacks.shields[0].marks[0]);
        break;
    case 18: bad.adapter.carried[0] = OpponentCombatCheckpoint::State::kMaxRecords + 1; break;
    case 19: bad.adapter.bags[0].effect = bad.combat.effects.nextId; break;
    case 20: bad.adapter.bags[0].name = "GARGEAGL"; break;
    case 21: bad.adapter.bags[1].name.clear(); break;
    case 22: bad.adapter.bags[0].age = 3; break;
    case 23: bad.adapter.bags[1].pitch = std::numeric_limits<f32>::infinity(); break;
    default: break;
    }
    CHECK_FALSE(Packet::encode(bad));
    CHECK(Packet::encode(baseline));
}

TEST_CASE("combat packet rejects forged nested fields and opaque leaf lengths",
          "[netplay][combat-state-packet]") {
    const NativeFixture fixture;
    auto state = fixture.state();
    decorate(state);
    state.combat.arsenal.missiles.missiles[0].damage = 731.125f;
    const auto encoded = Packet::encode(state);
    REQUIRE(encoded);
    const auto unique = [&](std::initializer_list<u32> values) {
        std::vector<u8> pattern(values.size() * 4);
        usize offset = 0;
        for (const u32 value : values) {
            word(pattern, offset, value);
            offset += 4;
        }
        const auto found =
            std::search(encoded->begin(), encoded->end(), pattern.begin(), pattern.end());
        REQUIRE(found != encoded->end());
        REQUIRE(std::search(found + 1, encoded->end(), pattern.begin(), pattern.end()) ==
                encoded->end());
        return static_cast<usize>(found - encoded->begin());
    };
    SECTION("nested nonfinite damage") {
        auto bytes = *encoded;
        word(bytes, unique({std::bit_cast<u32>(731.125f)}), 0x7FC00000);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("wall sound semantic range") {
        auto bytes = *encoded;
        word(bytes, unique({0x12345678, 3, 12, 8, 41}) - 4, 255);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("pierced count checked before allocation") {
        auto bytes = *encoded;
        word(bytes, unique({0x12345678, 3, 12, 8, 41}) + 4, 0xFFFFFFFF);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("runtime leaf length cannot consume sibling pools") {
        auto bytes = *encoded;
        word(bytes, Packet::kHeaderBytes + 4, 0xFFFFFFFF);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("runtime leaf cannot be silently omitted") {
        auto bytes = *encoded;
        word(bytes, Packet::kHeaderBytes + 4, 0);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("shield target family and cooldown are checked after decode") {
        const usize mark = unique({4, 23, std::bit_cast<u32>(0.125f)});
        auto bytes = *encoded;
        word(bytes, mark, 5);
        CHECK_FALSE(Packet::decode(bytes));
        bytes = *encoded;
        word(bytes, mark + 8, std::bit_cast<u32>(0.5f));
        CHECK_FALSE(Packet::decode(bytes));
        bytes = *encoded;
        word(bytes, mark + 8, 0x7FC00000);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("shield mark count is bounded before allocation") {
        auto bytes = *encoded;
        word(bytes, unique({0, 13, 0, 1, 17, std::bit_cast<u32>(0.1f)}) - 4, 0xFFFFFFFF);
        CHECK_FALSE(Packet::decode(bytes));
    }
    SECTION("statue floor flag is a canonical boolean") {
        auto bytes = *encoded;
        // Unique roster record: view radius, sight, activeOn, onFloor=false,
        // carried-present. The other roster entry and risen copy retain true.
        word(bytes, unique({std::bit_cast<u32>(10.0f), std::bit_cast<u32>(12.0f), 4, 0, 1}) + 12,
             2);
        CHECK_FALSE(Packet::decode(bytes));
    }
}

TEST_CASE("combat packet refuses locally valid transport overflow instead of truncating",
          "[netplay][combat-state-packet]") {
    auto state = emptyState();
    const s32 mode = GENERATE(0, 1, 2);
    CAPTURE(mode);
    if (mode == 0) {
        // Repeated queued hits are native values, not a deduplicated contact set.
        state.adapter.rockHits.assign(Packet::kMaxRecords + 1, {1, 1});
    } else {
        EffectTrees::PoolCheckpoint::Entry effect;
        effect.resource = 1;
        effect.state.name = "POOL";
        if (mode == 2) {
            effect.state.trails = particles();
            effect.state.trails.entries[0].emitter.particles.clear();
            effect.state.trails.entries.resize(4, effect.state.trails.entries[0]);
        }
        const usize count = mode == 1 ? Packet::kMaxEffects + 1 : Packet::kMaxEffects;
        for (usize index = 0; index < count; ++index) {
            effect.id = static_cast<u32>(index + 1);
            effect.instance = index + 1;
            state.combat.effects.effects.push_back(effect);
        }
        state.combat.effects.nextId = static_cast<u32>(count + 1);
        state.combat.effects.nextInstance = count + 1;
    }
    REQUIRE(state.valid());
    const auto before = state;
    CHECK_FALSE(Packet::encode(state));
    CHECK(state == before);
    std::vector<u8> oversized(Packet::kMaxBytes + 1);
    CHECK_FALSE(Packet::decode(oversized));
}

TEST_CASE("combat packet retains native exhausted identity counters",
          "[netplay][combat-state-packet]") {
    auto state = emptyState();
    state.combat.arsenal.missiles.nextInstance = 0;
    state.combat.effects.nextId = 0;
    state.combat.effects.nextInstance = 0;
    state.combat.attacks.strikes.next = 0;
    REQUIRE(state.valid());
    const auto bytes = Packet::encode(state);
    REQUIRE(bytes);
    CHECK(Packet::decode(*bytes) == state);
}

TEST_CASE("combat packet preserves native unused cooldown sentinels and portable critter RNG",
          "[netplay][combat-state-packet]") {
    auto state = emptyState();
    auto& actor = state.adapter.critters.actors[0].parts[0].actor;
    actor.moveTimes = {-std::numeric_limits<f32>::infinity(), -0.0f, 731.125f};
    actor.patternTimes = {19.25f};
    Mt19937 random{0x13572468};
    for (s32 draw = 0; draw < 627; ++draw) {
        (void)random();
    }
    state.adapter.projectiles.random = random.checkpoint();
    REQUIRE(state.valid());
    const auto bytes = Packet::encode(state);
    REQUIRE(bytes);
    const auto decoded = Packet::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(std::bit_cast<u32>(decoded->adapter.critters.actors[0].parts[0].actor.moveTimes[1]) ==
          0x80000000U);
    CHECK(Packet::encode(*decoded) == bytes);
    Mt19937 resumed;
    REQUIRE(resumed.restore(decoded->adapter.projectiles.random));
    for (s32 draw = 0; draw < 1000; ++draw) {
        CHECK(resumed() == random());
    }

    std::vector<u8> pattern(16);
    word(pattern, 0, 3);
    word(pattern, 4, 0xFF800000);
    word(pattern, 8, 0x80000000);
    word(pattern, 12, std::bit_cast<u32>(731.125f));
    const auto found = std::search(bytes->begin(), bytes->end(), pattern.begin(), pattern.end());
    REQUIRE(found != bytes->end());
    REQUIRE(std::search(found + 1, bytes->end(), pattern.begin(), pattern.end()) == bytes->end());
    const usize offset = static_cast<usize>(found - bytes->begin());
    for (const u32 bad : {0x7F800000U, 0x7FC00000U}) {
        auto corrupt = *bytes;
        word(corrupt, offset + 4, bad);
        CHECK_FALSE(Packet::decode(corrupt));
    }
    auto corrupt = *bytes;
    word(corrupt, offset, 0xFFFFFFFF);
    CHECK_FALSE(Packet::decode(corrupt));
    state.adapter.projectiles.random.words.fill(0);
    CHECK_FALSE(Packet::encode(state));
}
} // namespace
