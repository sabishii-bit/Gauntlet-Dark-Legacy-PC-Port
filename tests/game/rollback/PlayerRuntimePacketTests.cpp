#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/ByteReader.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/party/PartyMotion.h"
#include "game/players/PowerupEffects.h"
#include "game/rollback/PlayerHealthCheckpoint.h"
#include "game/rollback/PlayerRuntimePacket.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void word(std::vector<u8>& bytes, usize offset, u32 value) {
    REQUIRE(offset + 4 <= bytes.size());
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
void resizeHeader(std::vector<u8>& bytes) {
    word(bytes, 8, static_cast<u32>(bytes.size()));
}

PlayerRuntimeCheckpoint::State sample() {
    PlayerRuntimeCheckpoint::State value;
    value.player = 3;
    value.save.name = "TEST";
    value.save.character = 6;
    value.save.color = 2;
    value.save.classUnlock = 0xA55A;
    value.save.gold = 12345;
    value.save.levelTotal = 29;
    value.save.autoAttack = false;
    value.save.helpSeen = {7, 2, 7};
    value.save.moviesSeen = {"G1", "B2", "G1", std::string{"A\0B", 3}};
    for (usize index = 0; index < value.save.classes.size(); ++index) {
        const auto number = static_cast<s32>(index);
        auto& progress = value.save.classes[index];
        progress.experience = 2000 + number;
        progress.gold = 11 * number;
        progress.promotedLevel = number;
        progress.health = 1000 + number;
        progress.fightAdd = 1.25f + static_cast<f32>(index);
        progress.armorAdd = 2.5f;
        progress.magicAdd = -3.25f;
        progress.speedAdd = 4.125f;
        for (usize realm = 0; realm < progress.crystals.size(); ++realm) {
            progress.crystals[realm] = number + static_cast<s32>(realm);
        }
        progress.unlocked = 0x2AAA;
        progress.inventory.keys = number % 10;
        progress.inventory.potions = {4, 0, 1, 3, 0};
        for (usize slot = 0; slot < progress.inventory.powerups.size(); ++slot) {
            progress.inventory.powerups[slot] = {
                slot == 5 ? 0.0f : -1.0f, 5 + static_cast<s32>(slot % 5),
                static_cast<f32>(slot) + 0.125f, 0x81230000U + static_cast<u32>(slot),
                slot % 2 == 0};
        }
        progress.inventory.permanentPojo = index % 2 != 0;
        progress.relics = {0x1AAA, 0xBEEF, 0xBCDE, 0x000A, 0x000C, 0x001F, {1, 2, number + 3}};
        progress.levels.beaten.fill(static_cast<u8>(0x80U + index));
        progress.levels.runeLevels = {0x0123, 0x1ABC};
        progress.levels.legendLevels = {0xA123, 0xB456};
        progress.levels.bossDeaths = {0xC789, 0xDEF0};
        progress.lifetime = {number + 12, number + 34, number + 56,
                             123456.125 + static_cast<f64>(index)};
        progress.legends[0] = {u64{1} << index, 2, 4, 8, {"mount1", "town4"}};
        progress.legends[1] = {16, 32, 64, u64{1} << (63 - index), {"levela1", "levels6"}};
    }
    value.entrySave = value.save;
    value.entrySave.name = "ENTRY";
    value.entrySave.gold = 987;
    value.entrySave.classes[2].inventory.powerups[5].on = true;
    value.actor.position = {567.125f, 2, -3};
    value.actor.yaw = -0.25f;
    value.actor.speed = 8;
    value.actor.paceBonus = 2;
    value.actor.radius = 0.625f;
    value.actor.height = 4.5f;
    value.actor.followHeight = 2.25f;
    value.actor.attentionHeight = 3;
    value.actor.moving = true;
    value.actor.contacts = {{7, {1, 2, 3}}, {-1, {-4, -5, -6}}};
    value.cursorAiming = true;
    value.meleeFacing = Vec3{1, 0, -1};
    value.attackStep = PlayerRuntime::AttackStep{{1, 2, 3}, {4, 5, 6}};
    value.previous = {{7, 8, 9}, 0.25f, true, 0x123456789ABCULL};
    value.capture = {glm::translate(Mat4{1}, Vec3{1, 3, 2}),
                     {1, 2, 3},
                     0.25f,
                     12.5f,
                     false,
                     7,
                     true,
                     {4.25f, -7.5f},
                     true};
    value.combo = {ComboRole::Thrown, 1, 0, false, true, 80, 7, 0.75f, {2, 3, 4}};
    value.transport = {{9, 8, 7}, 16, true};
    value.mikey = {{3, 4, 5}, 0.375, 17, 8, 0xAABBCCDD};
    value.slot = 65535;
    value.resultsCheckpoint = LevelResults::Checkpoint{11, 12, 13};
    value.levelKills = 24;
    value.life = PlayerLife::Dying;
    value.towerPrompt = true;
    value.departed = true;
    value.healthFraction = -0.25f;
    value.painOwed = 5.25f;
    value.hitSoundGap = 29;
    value.heartbeatTicks = 17;
    value.fixtureSpot = Vec3{3, 4, 5};
    value.surfaceGap = 0.125f;
    value.hitFlashTicks = 3;
    value.itTicks = 31;
    value.gesture = PlayerDeed::Pick;
    value.gagSeconds = 0.4f;
    value.shockGaps = {{8, 0.3f}, {3, 0.4f}};
    value.deathHeld = 8;
    value.deathDrainTicks = -12;
    value.deathHaloHeard = true;
    value.reaction = PlayerDeed::Flinch;
    Knockback impulse;
    impulse.queue({0, 0, 1}, damage::kKnockBack, 30);
    value.knockback = impulse.checkpoint();
    TurboMeter meter;
    meter.add(73);
    value.turbo = meter.checkpoint();
    value.move.pending = {1, 4};
    value.move.all = {1, 4, 7};
    value.move.volleysShot = {2, 0, 3};
    value.move.owed = TurboMeter::kStrongCost;
    value.move.named = true;
    value.move.weaponHidden = true;
    value.pojoTurbo = true;
    value.helpHeard = {13, 2, 13};
    value.rammed = {4, 2, 9};
    value.blockLeft = 0.15f;
    value.blocked = true;
    value.cloudGap = 0.3f;
    value.breathGap = 0.4f;
    value.effectGap = 0.5f;
    value.floor = {12, 0x81234567, glm::translate(Mat4{1}, Vec3{1, 3, 2})};
    value.gem = {true, true, HeadGem::Shown::HealthVamp};
    value.glow = 1.3f;
    value.weaponGlow = {3, 0};
    value.streak = {31, 0.35f, true};
    value.exitWait = {417};
    value.nameTicks = 22;
    value.wornSpecial = 0x81234567;
    REQUIRE(value.valid());
    return value;
}

TEST_CASE("participant packet preserves every class and native runtime field canonically",
          "[rollback][player-runtime-packet]") {
    const auto state = sample();
    const auto encoded = PlayerRuntimePacket::encode(state);
    REQUIRE(encoded);
    CHECK(encoded->size() <= PlayerRuntimePacket::kMaxBytes);
    CHECK(readU32LE(*encoded, 0) == fourcc("PRTS"));
    CHECK(readU32LE(*encoded, 4) == PlayerRuntimePacket::kVersion);
    CHECK(readU32LE(*encoded, 8) == encoded->size());
    CHECK(readU32LE(*encoded, 12) == 0);
    CHECK(readU32LE(*encoded, 16) == 3);
    const auto decoded = PlayerRuntimePacket::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == state);
    CHECK(PlayerRuntimePacket::encode(*decoded) == encoded);
    CHECK(decoded->save.classes[2].inventory.powerups[5].strength == 0);
    CHECK(decoded->save.classes[2].inventory.powerups[5].flags != 0);
    CHECK_FALSE(decoded->save.classes[2].inventory.powerups[5].on);
    CHECK(decoded->entrySave.classes[2].inventory.powerups[5].on);
    CHECK(decoded->save.progress().inventory.potions == std::vector<s32>{4, 0, 1, 3, 0});
    CHECK(decoded->save.moviesSeen.back() == std::string{"A\0B", 3});
    auto changed = state;
    changed.save.classes[2].legends[1].legendItems ^= 1;
    CHECK_FALSE(changed == state);
    changed = state;
    changed.entrySave.classes[2].legends[0].completedMaps.pop_back();
    CHECK_FALSE(changed == state);
}

TEST_CASE("participant packet rejects truncated trailing version and noncanonical scalar data",
          "[rollback][player-runtime-packet]") {
    const auto state = sample();
    const auto encoded = PlayerRuntimePacket::encode(state);
    REQUIRE(encoded);
    // Header-length validation and deep reader underflow are independent gates.
    // Sample each section through the entire large all-class record, plus every
    // byte of its header and final variable/runtime fields.
    for (usize size = 0; size < encoded->size(); ++size) {
        if (size > 64 && size + 256 < encoded->size() && size % 37 != 0) {
            continue;
        }
        CAPTURE(size);
        auto truncated =
            std::vector<u8>(encoded->begin(), encoded->begin() + static_cast<std::ptrdiff_t>(size));
        CHECK_FALSE(PlayerRuntimePacket::decode(truncated));
        if (size >= PlayerRuntimePacket::kHeaderBytes) {
            resizeHeader(truncated);
            CHECK_FALSE(PlayerRuntimePacket::decode(truncated));
        }
    }
    for (const usize offset : {usize{0}, usize{4}, usize{8}, usize{12}, usize{16}, usize{20}}) {
        CAPTURE(offset);
        auto bad = *encoded;
        word(bad, offset, 0xFFFFFFFF);
        CHECK_FALSE(PlayerRuntimePacket::decode(bad));
    }
    auto trailing = *encoded;
    trailing.push_back(0);
    resizeHeader(trailing);
    CHECK_FALSE(PlayerRuntimePacket::decode(trailing));
    auto oldVersion = *encoded;
    word(oldVersion, 4, 2);
    CHECK_FALSE(PlayerRuntimePacket::decode(oldVersion));

    const usize characterOffset = 24 + state.save.name.size();
    auto bad = *encoded;
    word(bad, characterOffset, kClassCount);
    CHECK_FALSE(PlayerRuntimePacket::decode(bad));
    bad = *encoded;
    bad[characterOffset + 18] = 2; // autoAttack is one strict boolean byte.
    CHECK_FALSE(PlayerRuntimePacket::decode(bad));
    bad = *encoded;
    word(bad, characterOffset + 19, 0xFFFFFFFF); // Before any help vector allocation.
    CHECK_FALSE(PlayerRuntimePacket::decode(bad));

    std::array<u8, 4> marker{};
    const auto bits = std::bit_cast<u32>(state.actor.position.x);
    for (usize i = 0; i < marker.size(); ++i) {
        marker[i] = static_cast<u8>(bits >> (i * 8));
    }
    const auto at = std::search(encoded->begin(), encoded->end(), marker.begin(), marker.end());
    REQUIRE(at != encoded->end());
    REQUIRE(std::search(at + 1, encoded->end(), marker.begin(), marker.end()) == encoded->end());
    bad = *encoded;
    word(bad, static_cast<usize>(at - encoded->begin()), 0x7FC00000); // Native body NaN.
    CHECK_FALSE(PlayerRuntimePacket::decode(bad));
}

TEST_CASE("participant packet has bounded vectors strings and aggregate allocation",
          "[rollback][player-runtime-packet]") {
    auto state = sample();
    state.helpHeard.assign(PlayerRuntimePacket::kMaxRecords, 3);
    state.rammed.assign(PlayerRuntimePacket::kMaxRecords, 7);
    state.shockGaps.assign(PlayerRuntimePacket::kMaxRecords, {4, 0.25f});
    state.save.helpSeen.assign(PlayerRuntimePacket::kMaxRecords, 2);
    state.entrySave.helpSeen.assign(PlayerRuntimePacket::kMaxRecords, 9);
    state.save.moviesSeen.assign(PlayerRuntimePacket::kMaxMovies, "G1");
    state.actor.contacts.clear();
    for (usize index = 0; index < PlayerActor::Checkpoint::kMaxContacts; ++index) {
        state.actor.contacts.push_back({static_cast<s32>(index), {2, 3, 4}});
    }
    REQUIRE(state.valid());
    const auto encoded = PlayerRuntimePacket::encode(state);
    REQUIRE(encoded);
    CHECK(PlayerRuntimePacket::decode(*encoded) == state);
    const auto kind = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(kind);
    auto bad = state;
    switch (kind) {
    case 0: bad.helpHeard.push_back(1); break;
    case 1: bad.rammed.push_back(2); break;
    case 2: bad.shockGaps.push_back({1, 0}); break;
    case 3: bad.save.helpSeen.push_back(1); break;
    case 4: bad.save.moviesSeen.emplace_back("G2"); break;
    case 5:
        // Each string is locally valid and individually transport-bounded;
        // together the payload exceeds the aggregate transport byte limit.
        bad.save.moviesSeen.assign(PlayerRuntimePacket::kMaxMovies,
                                   std::string(PlayerRuntimePacket::kMaxString, 'A'));
        break;
    default: break;
    }
    REQUIRE(bad.valid());
    CHECK_FALSE(PlayerRuntimePacket::encode(bad));
    auto invalid = sample();
    invalid.entrySave.classes[7].inventory.powerups[4].charge =
        std::numeric_limits<f32>::infinity();
    CHECK_FALSE(PlayerRuntimePacket::encode(invalid));
    invalid = sample();
    invalid.weaponGlow.effect = 99; // Uncaptured external handles stay unsupported.
    CHECK_FALSE(PlayerRuntimePacket::encode(invalid));
}

TEST_CASE("participant packet restores exact native damage motion and random cue continuation",
          "[rollback][player-runtime-packet]") {
    constexpr f32 kStepSeconds = 1.0f / 60;
    std::array<PlayerRuntime, 1> players;
    ClassStats stats;
    CharacterSave save;
    save.name = "TEST";
    save.progress().health = 220;
    players[0].actor.spawn(2, save, &stats, Vec3{0}, 0);
    players[0].entrySave = save;
    PlayerRuntimeCheckpoint owner(players[0], &stats);
    PlayerHealth health;
    WorldCollision collision;
    collision.build({{{0, 1, 0}, {{{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}}}},
                     {{0, 1, 0}, {{{-100, 0, -100}, {100, 0, 100}, {-100, 0, 100}}}}});
    std::vector<std::string> cues;
    const PlayerHealth::Events healthEvents{
        .sound = [&](std::string_view cue) { cues.emplace_back(cue); },
        .cry = [&](std::string_view cue) { cues.emplace_back(cue); },
        .named = [&](std::string_view cue, f32) { cues.emplace_back(cue); }};
    const auto tick = [&](s32 time) {
        if (time % 7 == 0) {
            health.hurt(players[0], 9.25f, HurtKind::Blow, true, false, 1, healthEvents,
                        {damage::kKnockBack, Vec3{0, 0, 1}}, false, &stats);
        }
        std::array<PlayInput, 1> inputs{};
        inputs[0].move = {Vec2{0.4f, 0.6f}, 0.72f};
        PartyMotion::Events events;
        events.perform = [](usize, PartyMotion::Action) {};
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
        PartyMotion::step(players, inputs, false, 0, 1, kStepSeconds, collision, events);
        players[0].hitSoundGap = std::max(players[0].hitSoundGap - 1, 0);
    };
    for (s32 time = 0; time < 19; ++time) {
        tick(time);
    }
    const auto initial = owner.checkpoint();
    REQUIRE(initial);
    const auto packet = PlayerRuntimePacket::encode(*initial);
    const auto rngPacket = PlayerHealthCheckpoint::encode(health.checkpoint());
    REQUIRE(packet);
    REQUIRE(rngPacket);
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    cues.clear();
    for (s32 time = 19; time < 100; ++time) {
        tick(time);
        const auto next = owner.checkpoint();
        REQUIRE(next);
        expected.push_back(*next);
    }
    const auto expectedCues = cues;
    REQUIRE_FALSE(expectedCues.empty());
    const auto decoded = PlayerRuntimePacket::decode(*packet);
    const auto rng = PlayerHealthCheckpoint::decode(*rngPacket);
    REQUIRE(decoded);
    REQUIRE(rng);
    REQUIRE(owner.restore(*decoded));
    REQUIRE(health.restore(*rng));
    CHECK(cues == expectedCues);
    cues.clear();
    for (s32 time = 19; time < 100; ++time) {
        CAPTURE(time);
        tick(time);
        CHECK(owner.checkpoint() == expected[static_cast<usize>(time - 19)]);
    }
    CHECK(cues == expectedCues);
}

TEST_CASE("participant packet retains trusted native figure poses and resource bindings",
          "[rollback][player-runtime-packet][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    const auto character = GENERATE(0, 2, 6);
    CharacterSave save;
    save.name = "TEST";
    save.character = character;
    save.progress().health = 1000;
    PlayerRuntime player;
    player.actor.spawn(1, save, nullptr, Vec3{1, 0, 2}, 0.2f);
    player.entrySave = save;
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->animate(0.7f, 1, 1.0f / 60);
    player.figure->animate(0.7f, 1, 1.0f / 60, PlayerDeed::Attack);
    PlayerRuntimeCheckpoint owner(player, nullptr);
    const auto state = owner.checkpoint();
    REQUIRE(state);
    REQUIRE(state->figure);
    REQUIRE_FALSE(state->figure->transforms.empty());
    const auto encoded = PlayerRuntimePacket::encode(*state);
    REQUIRE(encoded);
    const auto decoded = PlayerRuntimePacket::decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == *state);
    CHECK(PlayerRuntimePacket::encode(*decoded) == encoded);
    const auto uploads = device.texturesCreated;
    const auto updates = device.textureUpdates;
    player.figure->animate(0.2f, 1, 1.0f / 60);
    REQUIRE(owner.restore(*decoded));
    CHECK(owner.checkpoint() == state);
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == updates);
    auto decorated = *decoded;
    decorated.figure->familiarPending = true;
    decorated.figure->familiarReleased = true;
    decorated.figure->phoenixSide = 0.25f;
    // Raw requested tier can survive failed optional art binding, while the
    // public tier remains zero. Complete child state now enforces that relation.
    decorated.figure->familiar.tier = 2;
    decorated.figure->companion = PowerupCompanion::Kind::Wings;
    decorated.figure->powerupCompanion.kind = PowerupCompanion::Kind::Wings;
    decorated.figure->handHeld = !decorated.figure->handHeld;
    decorated.figure->armHeld = !decorated.figure->armHeld;
    decorated.figure->staysInHand = !decorated.figure->staysInHand;
    decorated.figure->handObjects = {"LEFT", "RIGHT", "LEFT"};
    decorated.figure->armObjects = {"ARM", "SHIELD"};
    REQUIRE(decorated.valid());
    const auto decoratedPacket = PlayerRuntimePacket::encode(decorated);
    REQUIRE(decoratedPacket);
    CHECK(PlayerRuntimePacket::decode(*decoratedPacket) == decorated);
    CHECK_FALSE(owner.restore(decorated)); // Scalar validity never grants new native bindings.
    CHECK(owner.checkpoint() == state);
    auto foreign = *decoded;
    foreign.figure->costume += "_OTHER";
    REQUIRE(foreign.valid());
    const auto foreignPacket = PlayerRuntimePacket::encode(foreign);
    REQUIRE(foreignPacket);
    const auto foreignValue = PlayerRuntimePacket::decode(*foreignPacket);
    REQUIRE(foreignValue);
    CHECK_FALSE(owner.restore(*foreignValue));
    CHECK(owner.checkpoint() == state);
}
} // namespace
