#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/party/PartyMotion.h"
#include "game/party/PlayerHealth.h"
#include "game/party/PlayerRuntimeCheckpoint.h"
#include "game/party/ScriptedMotion.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

struct Fixture {
    std::array<PlayerRuntime, 1> party;
    ClassStats stats;
    PlayerHealth health;
    WorldCollision collision;
    std::vector<std::string> cues;
    std::vector<PartyMotion::Action> actions;
    PlayerRuntimeCheckpoint owner{party[0], &stats};
    Fixture() {
        CharacterSave save;
        save.name = "TEST";
        save.progress().health = 1000;
        party[0].actor.spawn(0, save, &stats, Vec3{0}, 0);
        party[0].entrySave = save;
        collision.build({
            {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}},
            {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}},
        });
    }
    PlayerRuntime& player() { return party[0]; }
    void tick(s32 tick) {
        auto& runtime = player();
        const PlayerHealth::Events healthEvents{
            .sound = [this](std::string_view cue) { cues.emplace_back(cue); },
            .cry = [this](std::string_view cue) { cues.emplace_back(cue); },
            .named = [this](std::string_view cue, f32) { cues.emplace_back(cue); }};
        // Exercise rounded health, queued reactions and the party-wide cry RNG,
        // not an independently reimplemented damage formula.
        if (tick % 7 == 0) {
            health.hurt(runtime, 7.25f, HurtKind::Blow, true, false, 1, healthEvents,
                        {damage::kKnockBack, Vec3{0, 0, 1}}, false, &stats);
        }
        if (tick % 11 == 0) {
            auto& progress = runtime.actor.save().progress();
            ++runtime.actor.save().gold;
            progress.experience += 9;
            ++progress.lifetime.goldFound;
            runtime.streak.record(tick % 22 == 0);
        }
        const auto praise = runtime.streak.step(kStep);
        if (!praise.empty()) {
            cues.emplace_back(praise);
        }
        std::array<PlayInput, 1> input{};
        input[0].move = {Vec2{tick % 40 < 20 ? 0.6f : -0.6f, 0.4f}, 0.72f};
        const test::MotionScript events{
            .perform = [this](usize, PartyMotion::Action action) { actions.push_back(action); },
            .select = [](usize, const SelectorInput&, s32) {},
            .advanceTurbo = [](usize, s32, f32) {}};
        test::ScriptedMotion motionLevel{events};
        PartyMotion::step(party, input, false, 0, 1, kStep, collision, motionLevel);
        runtime.hitSoundGap = std::max(runtime.hitSoundGap - 1, 0);
        PlayerHealth::heartbeat(party, 1, false);
    }
};

TEST_CASE(
    "runtime checkpoint retains congratulated gargoyle records without accepting corrupt counts",
    "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    for (auto& progress : fixture.player().actor.save().classes) {
        progress.relics.gargoylePieces.fill(-1);
    }
    fixture.player().entrySave = fixture.player().actor.save();
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    fixture.player().actor.save().progress().relics.gargoylePieces.fill(0);
    REQUIRE(fixture.owner.restore(*state));
    CHECK(fixture.owner.checkpoint() == state);
    for (const s32 invalid : {-2, std::numeric_limits<s32>::min()}) {
        auto corrupt = *state;
        corrupt.save.classes.front().relics.gargoylePieces.front() = invalid;
        CHECK_FALSE(fixture.owner.restore(corrupt));
        CHECK(fixture.owner.checkpoint() == state);
        corrupt = *state;
        corrupt.entrySave.classes.front().relics.gargoylePieces.front() = invalid;
        CHECK_FALSE(fixture.owner.restore(corrupt));
        CHECK(fixture.owner.checkpoint() == state);
    }
}

TEST_CASE("runtime checkpoint restores native damage movement rewards and random cue continuation",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    for (s32 tick = 0; tick < 19; ++tick) {
        fixture.tick(tick);
    }
    const auto start = fixture.owner.checkpoint();
    REQUIRE(start);
    const auto health = fixture.health.checkpoint();
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    fixture.cues.clear();
    fixture.actions.clear();
    for (s32 tick = 19; tick < 180; ++tick) {
        fixture.tick(tick);
        const auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        expected.push_back(*state);
    }
    const auto cues = fixture.cues;
    const auto actions = fixture.actions;
    REQUIRE_FALSE(cues.empty());
    REQUIRE(expected.back().actor.position != start->actor.position);
    REQUIRE(expected.back().save.gold > start->save.gold);
    REQUIRE(expected.back().save.health() < start->save.health());
    REQUIRE(fixture.owner.restore(*start));
    REQUIRE(fixture.health.restore(health));
    CHECK(fixture.cues == cues); // Restoring is not another damage/cue dispatch.
    fixture.cues.clear();
    fixture.actions.clear();
    for (s32 tick = 19; tick < 180; ++tick) {
        CAPTURE(tick);
        fixture.tick(tick);
        const auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        CHECK(*state == expected[static_cast<usize>(tick - 19)]);
    }
    CHECK(fixture.cues == cues);
    CHECK(fixture.actions == actions);
}

TEST_CASE("runtime checkpoint preserves inactive inventory slots all classes and ordered residue",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    auto& runtime = fixture.player();
    auto& save = runtime.actor.save();
    save.helpSeen = {7, 2, 7};
    save.moviesSeen = {"G1", "B2", "G1"};
    save.classUnlock = 0xA55A;
    for (usize i = 0; i < save.classes.size(); ++i) {
        auto& progress = save.classes[i];
        progress.gold = static_cast<s32>(i) * 13;
        progress.promotedLevel = static_cast<s32>(i);
        progress.inventory.powerups[5] = {0, 5, 2.5f, 0x100, false};
        progress.inventory.permanentPojo = (i & 1) != 0;
        progress.lifetime.playSeconds = 91.125 + static_cast<f64>(i);
    }
    runtime.entrySave = save;
    runtime.cursorAiming = true;
    runtime.meleeFacing = Vec3{1, 0, -1};
    runtime.attackStep = PlayerRuntime::AttackStep{{1, 2, 3}, {4, 5, 6}};
    runtime.previous = {{7, 8, 9}, 0.25f, true, 17};
    runtime.combo = {ComboRole::Thrown, 1, 0, false, true, 80, 7, 0.75f, {2, 3, 4}};
    REQUIRE(runtime.transport.begin({9, 8, 7}));
    runtime.transport.update(7);
    runtime.slot = 3;
    runtime.resultsCheckpoint = LevelResults::Checkpoint{11, 12, 13};
    runtime.levelKills = 24;
    runtime.towerPrompt = true;
    runtime.healthFraction = -0.25f;
    runtime.painOwed = 5.25f;
    runtime.hitSoundGap = 29;
    runtime.heartbeatTicks = 17;
    runtime.fixtureSpot = Vec3{3, 4, 5};
    runtime.surfaceGap = 0.125f;
    runtime.hitFlashTicks = 3;
    runtime.itTicks = 31;
    runtime.gesture = PlayerDeed::Pick;
    runtime.gagSeconds = 0.4f;
    runtime.shockGaps = {{8, 0.3f}, {3, 0.4f}};
    runtime.deathHeld = 8;
    runtime.deathDrainTicks = 1;
    runtime.deathHaloHeard = true;
    runtime.reaction = PlayerDeed::Flinch;
    runtime.knockback.queue({0, 0, 1}, damage::kKnockBack, 30);
    runtime.turbo.add(73);
    runtime.pojoTurbo = true;
    runtime.helpHeard = {13, 2, 13};
    runtime.rammed = {4, 2, 9};
    runtime.blockLeft = 0.15f;
    runtime.blocked = true;
    runtime.cloudGap = 0.3f;
    runtime.breathGap = 0.4f;
    runtime.effectGap = 0.5f;
    runtime.floor = {12, 0x400, glm::translate(Mat4{1}, Vec3{1, 3, 2})};
    REQUIRE(runtime.gem.update(powerup::kHandOfDeath));
    REQUIRE(runtime.gem.update(powerup::kHealthVamp));
    runtime.glow.raise(1.3f);
    runtime.streak.record(true);
    runtime.exitWait.step(true, 417);
    runtime.nameTicks = 22;
    runtime.wornSpecial = 0x1234;
    const auto state = fixture.owner.checkpoint();
    REQUIRE(state);
    runtime.actor.save() = CharacterSave{};
    runtime.combo = {};
    runtime.transport.clear();
    runtime.gem.update(0);
    runtime.shockGaps.clear();
    REQUIRE(fixture.owner.restore(*state));
    CHECK(fixture.owner.checkpoint() == state);
    CHECK_FALSE(runtime.gem.update(powerup::kHandOfDeath));
    CHECK(runtime.gem.shown() == HeadGem::kHealthVamp); // Both appearance latches survive.
}

TEST_CASE("runtime preparation is atomic single use and bound to participant lifetime",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    Fixture other;
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    auto target = *baseline;
    target.save.gold = 4321;
    auto prepared = fixture.owner.prepareRestore(target);
    auto sibling = fixture.owner.prepareRestore(*baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.owner.canCommit(*prepared));
    CHECK_FALSE(other.owner.commitRestore(std::move(*prepared)));
    CHECK(fixture.owner.checkpoint() == baseline);
    REQUIRE(fixture.owner.canCommit(*prepared));
    REQUIRE(fixture.owner.commitRestore(std::move(*prepared)));
    CHECK(fixture.owner.checkpoint() == target);
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*prepared)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*sibling)));
    CHECK(fixture.owner.checkpoint() == target);
    auto fresh = fixture.owner.prepareRestore(target);
    REQUIRE(fresh);
    REQUIRE(fixture.owner.commitRestore(std::move(*fresh)));
    for (s32 invalid = 0; invalid < 7; ++invalid) {
        CAPTURE(invalid);
        auto bad = target;
        switch (invalid) {
        case 0: bad.save.character = kClassCount; break;
        case 1:
            bad.entrySave.classes[3].inventory.powerups[1].charge =
                std::numeric_limits<f32>::quiet_NaN();
            break;
        case 2: bad.healthFraction = 2; break;
        case 3: bad.combo.role = static_cast<ComboRole>(99); break;
        case 4: bad.floor.placement = Mat4{std::numeric_limits<f32>::infinity()}; break;
        case 5: bad.helpHeard.resize(PlayerRuntimeCheckpoint::State::kMaxRecords + 1); break;
        case 6: bad.weaponGlow.effect = 7; break;
        default: break;
        }
        CHECK_FALSE(fixture.owner.prepareRestore(bad));
        CHECK_FALSE(fixture.owner.restore(bad));
        CHECK(fixture.owner.checkpoint() == target);
    }
    fixture.player().deathHeldEffect = 27;
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.restore(target));
    CHECK(fixture.player().deathHeldEffect == 27);
    fixture.player().deathHeldEffect = 0;
    fixture.player().mikeyFigure = std::make_unique<MikeyFigure>();
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.restore(target));
}

TEST_CASE("runtime retains native unspecified potions without resolving their shared cycle",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    auto& inventory = fixture.player().actor.save().progress().inventory;
    REQUIRE(inventory.addPotions(2, 1) == 1);
    REQUIRE(inventory.addPotions(0, 2) == 2);
    REQUIRE(inventory.addPotions(4, 1) == 1);
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    const std::vector<s32> expected{2, 0, 0, 4};
    CHECK(baseline->save.progress().inventory.potions == expected);
    CHECK(inventory.takePotion() == 4);
    CHECK(inventory.takePotion() == 0);
    REQUIRE(fixture.owner.restore(*baseline));
    CHECK(fixture.owner.checkpoint() == baseline);
    // Zero remains an unspecified potion for the separately checkpointed Arsenal
    // cycle to resolve; runtime restore must not select a color or consume it.
    CHECK(fixture.player().actor.save().progress().inventory.potions == expected);
    auto bad = *baseline;
    bad.save.progress().inventory.potions.front() = -1;
    CHECK_FALSE(fixture.owner.restore(bad));
    bad.save.progress().inventory.potions.front() = 5;
    CHECK_FALSE(fixture.owner.restore(bad));
    CHECK(fixture.owner.checkpoint() == baseline);
}

TEST_CASE("runtime preserves native transport crossing and decoy remainder without redispatch",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    auto& runtime = fixture.player();
    REQUIRE(runtime.transport.begin({5, 6, 7}));
    CHECK_FALSE(runtime.transport.update(14));
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, 60);
    REQUIRE_FALSE(inventory.powerups[0].on);
    inventory.powerups[0].on = true; // Native selector activation; pickup alone never deploys it.
    runtime.mikey.update(5.0f / 60, inventory, Vec3{2, 0, 4});
    REQUIRE(runtime.mikey.shown());
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    std::vector<std::optional<Vec3>> teleports;
    for (s32 tick = 0; tick < 75; ++tick) {
        teleports.push_back(runtime.transport.update(1));
        runtime.mikey.update(kStep, inventory, Vec3{9, 0, 9});
        expected.push_back(*fixture.owner.checkpoint());
    }
    REQUIRE(fixture.owner.restore(*baseline));
    for (s32 tick = 0; tick < 75; ++tick) {
        CAPTURE(tick);
        CHECK(runtime.transport.update(1) == teleports[static_cast<usize>(tick)]);
        runtime.mikey.update(kStep, runtime.actor.save().progress().inventory, Vec3{9, 0, 9});
        CHECK(fixture.owner.checkpoint() == expected[static_cast<usize>(tick)]);
    }
}

TEST_CASE("runtime prepared values cannot survive owner address reuse",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    alignas(PlayerRuntimeCheckpoint) std::array<std::byte, sizeof(PlayerRuntimeCheckpoint)>
        storage{};
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* owner = std::construct_at(reinterpret_cast<PlayerRuntimeCheckpoint*>(storage.data()),
                                    fixture.player(), &fixture.stats);
    auto prepared = owner->prepareRestore(*baseline);
    REQUIRE(prepared);
    std::destroy_at(owner);
    // byte view of a trivially copyable buffer
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    owner = std::construct_at(reinterpret_cast<PlayerRuntimeCheckpoint*>(storage.data()),
                              fixture.player(), &fixture.stats);
    CHECK_FALSE(owner->canCommit(*prepared));
    CHECK_FALSE(owner->commitRestore(std::move(*prepared)));
    CHECK(owner->checkpoint() == baseline);
    std::destroy_at(owner);
}

TEST_CASE("runtime figure correction retains exact native attachments and familiar release latches",
          "[rollback][player-runtime-checkpoint][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Fixture fixture;
    fixture.player().actor.save().progress().experience = levelExperience(35);
    fixture.player().figure =
        PlayerFigure::load(device, root, fixture.player().actor.save(), false);
    REQUIRE(fixture.player().figure);
    auto& figure = *fixture.player().figure;
    REQUIRE(figure.familiarTier() > 0);
    for (s32 tick = 0; tick < 11; ++tick) {
        figure.animate(0, 1, kStep, PlayerDeed::Attack);
    }
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    const auto uploads = device.texturesCreated;
    auto first = figure.prepareGameplayRestore(*baseline->figure);
    auto sibling = figure.prepareGameplayRestore(*baseline->figure);
    REQUIRE(first);
    REQUIRE(sibling);
    REQUIRE(figure.commitGameplayRestore(std::move(*first)));
    CHECK_FALSE(figure.canCommitGameplay(*sibling));
    CHECK_FALSE(figure.commitGameplayRestore(std::move(*sibling)));
    CHECK(fixture.owner.checkpoint() == baseline);
    auto fresh = figure.prepareGameplayRestore(*baseline->figure);
    REQUIRE(fresh);
    REQUIRE(figure.commitGameplayRestore(std::move(*fresh)));
    auto invalid = *baseline;
    REQUIRE_FALSE(invalid.figure->transforms.empty());
    invalid.figure->transforms.front()[3].x += 1;
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.owner.checkpoint() == baseline);
    invalid = *baseline;
    invalid.figure->costumeObjects.front() += "_WRONG";
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(fixture.owner.checkpoint() == baseline);
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    std::vector<std::optional<Mat4>> hands;
    std::vector<bool> familiar;
    for (s32 tick = 0; tick < 160; ++tick) {
        const auto deed = tick == 31 ? PlayerDeed::Flinch : PlayerDeed::Attack;
        figure.animate(tick % 25 < 12 ? 0.0f : 1.0f, 1, kStep, deed);
        auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        expected.push_back(*state);
        hands.push_back(figure.handAttachment(Mat4{1}));
        familiar.push_back(figure.familiarReleased());
    }
    REQUIRE(std::ranges::any_of(familiar, [](bool released) { return released; }));
    REQUIRE(fixture.owner.restore(*baseline));
    for (s32 tick = 0; tick < 160; ++tick) {
        CAPTURE(tick);
        const auto deed = tick == 31 ? PlayerDeed::Flinch : PlayerDeed::Attack;
        figure.animate(tick % 25 < 12 ? 0.0f : 1.0f, 1, kStep, deed);
        CHECK(fixture.owner.checkpoint() == expected[static_cast<usize>(tick)]);
        CHECK(figure.handAttachment(Mat4{1}) == hands[static_cast<usize>(tick)]);
        CHECK(figure.familiarReleased() == familiar[static_cast<usize>(tick)]);
    }
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("runtime gameplay restore supports same-binding Phoenix and refuses Pojo or replacement",
          "[rollback][player-runtime-checkpoint][assets]") {
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ItemArchive powerups;
    REQUIRE(powerups.load(root / "POWERUPS"));
    Fixture fixture;
    auto& runtime = fixture.player();
    runtime.figure = PlayerFigure::load(device, root, runtime.actor.save(), false);
    REQUIRE(runtime.figure);
    auto& inventory = runtime.actor.save().progress().inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 30);
    runtime.figure->setCompanionPowerups(device, powerups, inventory);
    REQUIRE(runtime.figure->phoenixActive());
    for (s32 tick = 0; tick < 120 && !runtime.figure->animator().released(); ++tick) {
        runtime.figure->animate(0, 1, kStep, PlayerDeed::Attack);
    }
    REQUIRE(runtime.figure->animator().released());
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    REQUIRE(baseline->figure->familiarPending);
    const auto uploads = device.texturesCreated;
    runtime.figure->animate(0, 1, kStep, PlayerDeed::Attack);
    REQUIRE(runtime.figure->familiarReleased());
    const auto expected = fixture.owner.checkpoint();
    REQUIRE(fixture.owner.restore(*baseline));
    runtime.figure->animate(0, 1, kStep, PlayerDeed::Attack);
    CHECK(fixture.owner.checkpoint() == expected);
    CHECK(device.texturesCreated == uploads);

    auto prepared = fixture.owner.prepareRestore(*baseline);
    REQUIRE(prepared);
    runtime.figure = PlayerFigure::load(device, root, runtime.actor.save(), false);
    REQUIRE(runtime.figure);
    CHECK_FALSE(fixture.owner.canCommit(*prepared));
    CHECK_FALSE(fixture.owner.commitRestore(std::move(*prepared)));
    CHECK_FALSE(fixture.owner.restore(*baseline)); // Existing Phoenix binding is absent.
    inventory.permanentPojo = true;
    runtime.figure->setCompanionPowerups(device, powerups, inventory);
    REQUIRE(runtime.figure->pojoActive());
    CHECK_FALSE(fixture.owner.checkpoint());
    CHECK_FALSE(fixture.owner.restore(*baseline));
}

TEST_CASE("runtime resumes a native captured throw and delivers landing damage exactly once",
          "[rollback][player-runtime-checkpoint]") {
    Fixture fixture;
    auto& runtime = fixture.player();
    runtime.capture.attach(7, true, glm::translate(Mat4{1}, Vec3{0, 12, 0}), runtime.actor);
    runtime.capture.release({4, 0, 2}, 70);
    runtime.capture.update(kStep, runtime.actor, fixture.collision);
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    std::vector<std::optional<f32>> damage;
    for (s32 tick = 0; tick < 90; ++tick) {
        damage.push_back(runtime.capture.update(kStep, runtime.actor, fixture.collision));
        const auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        expected.push_back(*state);
    }
    REQUIRE(std::ranges::count_if(damage, [](const auto& hit) { return hit.has_value(); }) == 1);
    REQUIRE(fixture.owner.restore(*baseline));
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick);
        CHECK(runtime.capture.update(kStep, runtime.actor, fixture.collision) ==
              damage[static_cast<usize>(tick)]);
        CHECK(fixture.owner.checkpoint() == expected[static_cast<usize>(tick)]);
    }
}

TEST_CASE("runtime native PartyMotion figure and damage replay are one exact gameplay continuation",
          "[rollback][player-runtime-checkpoint][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Fixture fixture;
    fixture.player().figure =
        PlayerFigure::load(device, root, fixture.player().actor.save(), false);
    REQUIRE(fixture.player().figure);
    for (s32 tick = 0; tick < 25; ++tick) {
        fixture.tick(tick);
    }
    const auto baseline = fixture.owner.checkpoint();
    REQUIRE(baseline);
    const auto health = fixture.health.checkpoint();
    const auto uploads = device.texturesCreated;
    std::vector<PlayerRuntimeCheckpoint::State> expected;
    fixture.cues.clear();
    fixture.actions.clear();
    for (s32 tick = 25; tick < 120; ++tick) {
        fixture.tick(tick);
        const auto state = fixture.owner.checkpoint();
        REQUIRE(state);
        expected.push_back(*state);
    }
    const auto cues = fixture.cues;
    const auto actions = fixture.actions;
    REQUIRE(fixture.owner.restore(*baseline));
    REQUIRE(fixture.health.restore(health));
    fixture.cues.clear();
    fixture.actions.clear();
    for (s32 tick = 25; tick < 120; ++tick) {
        CAPTURE(tick);
        fixture.tick(tick);
        CHECK(fixture.owner.checkpoint() == expected[static_cast<usize>(tick - 25)]);
    }
    CHECK(fixture.cues == cues);
    CHECK(fixture.actions == actions);
    CHECK(device.texturesCreated == uploads);
}
} // namespace
