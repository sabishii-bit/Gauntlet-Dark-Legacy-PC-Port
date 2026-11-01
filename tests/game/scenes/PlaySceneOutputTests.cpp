#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Ledger = PlayScene::OutputLedger;
constexpr f64 kStep = 1.0 / 60;
constexpr u64 kEpoch = 7;

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog levels;
    LevelWorld world;
    GameContext context;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    std::vector<PlayScene::RumbleOutput> vibrations;
    std::vector<u16> unlocks;
    PlayScene scene;

    Fixture(std::string_view level, bool hammer = false, bool audio = false) {
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", level));
        REQUIRE(levels.load(root));
        const auto ref = levels.byName(level);
        REQUIRE(ref);
        REQUIRE(world.load(device, root, *ref));
        context.config = &config;
        context.levels = &levels;
        context.unpackedRoot = root;
        context.sounds = audio ? &sounds : nullptr;
        context.vibrate = [&](s32 player, s32 frames, ControlFeedback feedback) {
            vibrations.push_back({player, frames, feedback});
        };
        context.unlockClasses = [&](u16 mask) { unlocks.push_back(mask); };
        CharacterSave save;
        save.name = "OUTPUT";
        save.progress().health = 5000;
        if (hammer) {
            save.progress().inventory.addPowerup(powerup::kWeapon, damage::kHammer, 10, -1);
            save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
        }
        const std::array party{PartyMember{3, save}};
        PlayOptions options;
        options.welcome = false;
        options.position =
            ref->isSecret() ? world.startPoint(0)->position : Vec3{18.8f, 0.2f, 0.7f};
        REQUIRE(scene.open(device, context, world, party, options));
        for (s32 tick = 0; tick < 480 && entering(); ++tick) {
            REQUIRE(scene.update(kStep, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
        vibrations.clear();
        unlocks.clear();
    }
    bool entering() const {
        return scene.spawning() || (scene.animator(3) != nullptr && scene.animator(3)->entering());
    }
    PlayerActor& player() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native contact fixture
        return *const_cast<PlayerActor*>(scene.actor(3));
    }
    PlayScene::RecordedStep step(u64 tick, const PlayScene::Inputs& inputs = {}) {
        auto result = scene.recordStep(kEpoch, tick, kStep, inputs);
        REQUIRE(result.outcome == PlayOutcome::Running);
        return result;
    }
    void dispatch(const Ledger::Result& result) {
        REQUIRE(result);
        for (const auto& action : result.actions) {
            REQUIRE(scene.dispatchOutput(action));
        }
    }
};

TEST_CASE("recorded native output rejects invalid frame labels before advancing",
          "[play-scene-output]") {
    PlayScene scene;
    REQUIRE_THROWS_AS(scene.recordStep(0, 1, kStep, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(scene.recordStep(1, 0, kStep, {}), std::invalid_argument);
    const auto closed = scene.recordStep(1, 1, kStep, {});
    CHECK(closed.outcome == PlayOutcome::Running);
    CHECK(closed.journal.events.empty());
    CHECK_FALSE(scene.dispatchOutput({Ledger::ActionKind::Commit,
                                      {{1, 1, {2, 0, 1, FrameEventEmitter::kNoSeat}, 1, 0},
                                       Ledger::Policy::Confirmed,
                                       PlayScene::UnlockClassesOutput{1},
                                       {}}}));
}

TEST_CASE("native scene audio capture and correction never mutate playback inside replay",
          "[play-scene-audio][play-scene-output][assets]") {
    auto f = std::make_unique<Fixture>("G1", false, true);
    // The spawn used here has no ambient emitter in range. Exercise an actual
    // persistent scene loop explicitly, alongside the attack one-shots.
    f->player().save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0,
                                                       20);
    REQUIRE(f->scene.beginRecordedAudio(kEpoch));
    CHECK_FALSE(f->scene.beginRecordedAudio(kEpoch));
    REQUIRE_THROWS_AS(f->scene.update(kStep, {}), std::logic_error);
    usize physicalCalls = 0;
    const SoundPlayer::Capture watch(&f->sounds, [&](const auto&) { ++physicalCalls; });
    usize finiteCues = 0;
    usize loopRequests = 0;
    const auto input = [](u64 tick) {
        PlayScene::Inputs controls;
        controls[3].attack = true;
        controls[3].attackPressed = tick == 1;
        controls[3].aimDirection = Vec3{1, 0, 0};
        return controls;
    };
    for (u64 first = 1; first <= 192; first += 8) {
        CAPTURE(first);
        PlayScene::LocalSnapshot saved;
        REQUIRE(f->scene.saveLocal(saved));
        std::vector<SceneSoundEvents::Frame> original;
        const auto before = physicalCalls;
        const auto voices = f->sounds.voiceCount();
        for (u64 tick = first; tick < first + 8; ++tick) {
            const auto step = f->step(tick, input(tick));
            REQUIRE(step.audio);
            finiteCues += step.audio->finite.events.size();
            loopRequests += step.audio->loops.size();
            original.push_back(*step.audio);
        }
        const auto native = f->scene.nativeAgreementDigest();
        REQUIRE(native);
        CHECK(physicalCalls == before);
        CHECK(f->sounds.voiceCount() == voices);
        const auto accepted = f->scene.reconcileRecordedAudio(first, original);
        REQUIRE(accepted);
        f->scene.dispatchRecordedAudio(accepted);
        const auto presented = physicalCalls;
        REQUIRE(f->scene.restoreLocal(saved));
        for (u64 tick = first; tick < first + 8; ++tick) {
            f->step(tick); // Different predicted attack decisions; no playback.
        }
        REQUIRE(f->scene.restoreLocal(saved));
        std::vector<SceneSoundEvents::Frame> corrected;
        for (u64 tick = first; tick < first + 8; ++tick) {
            const auto step = f->step(tick, input(tick));
            REQUIRE(step.audio);
            corrected.push_back(*step.audio);
        }
        CHECK(f->scene.nativeAgreementDigest() == native);
        CHECK(corrected == original);
        CHECK(physicalCalls == presented);
        const auto replay = f->scene.reconcileRecordedAudio(first, corrected);
        REQUIRE(replay);
        CHECK(replay.actions.empty());
        f->scene.dispatchRecordedAudio(replay);
        const auto confirmed = f->scene.confirmRecordedAudio(first + 7);
        REQUIRE(confirmed);
        f->scene.dispatchRecordedAudio(confirmed);
        CHECK(physicalCalls == presented);
        f->scene.serviceRecordedAudio(static_cast<f32>(kStep * 8));
    }
    CHECK(finiteCues > 0);
    CHECK(loopRequests > 0);
    CHECK(physicalCalls > 0);
}

TEST_CASE("recorded scene audio refuses legacy player loops before changing its epoch",
          "[play-scene-audio][assets]") {
    auto f = std::make_unique<Fixture>("G1");
    // A nonzero legacy handle is enough to refuse attachment. No backend polling
    // or stop operation may be needed to determine simulation eligibility.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native binding fixture
    auto& player = const_cast<PlayerRuntime&>(f->scene.participants().front());
    for (auto* handle : {&player.deathHeldCry, &player.deathHeldSuck}) {
        *handle = 1;
        CHECK_FALSE(f->scene.beginRecordedAudio(kEpoch));
        CHECK(f->scene.recordedAudioEpoch() == 0);
        CHECK(*handle == 1);
        *handle = kNoSound;
    }
    REQUIRE(f->scene.beginRecordedAudio(kEpoch));
    f->scene.endRecordedAudio();
    REQUIRE(f->scene.beginRecordedAudio(kEpoch + 1));
    const auto step = f->scene.recordStep(kEpoch + 1, 1, kStep, {});
    REQUIRE(step.audio);
    f->scene.endRecordedAudio();
}

TEST_CASE("confirmed challenge suspension ends audio history before external travel cues",
          "[play-scene-audio][play-scene-output][assets]") {
    auto f = std::make_unique<Fixture>("B2", false, true);
    REQUIRE(f->scene.beginRecordedAudio(kEpoch));
    REQUIRE(f->scene.recordedAudioEpoch() == kEpoch);
    PlayScene::LocalSnapshot before;
    REQUIRE(f->scene.saveLocal(before));
    f->player().place({-36.3671875f, 90.1640625f, -85.0234375f});
    PlayOutcome outcome = PlayOutcome::Running;
    for (u64 tick = 1; tick <= 240 && outcome == PlayOutcome::Running; ++tick) {
        const auto step = f->scene.recordStep(kEpoch, tick, kStep, {});
        REQUIRE(step.audio);
        const std::array frames{*step.audio};
        const auto accepted = f->scene.reconcileRecordedAudio(tick, frames);
        REQUIRE(accepted);
        f->scene.dispatchRecordedAudio(accepted);
        const auto confirmed = f->scene.confirmRecordedAudio(tick);
        REQUIRE(confirmed);
        f->scene.dispatchRecordedAudio(confirmed);
        outcome = step.outcome;
    }
    REQUIRE(outcome == PlayOutcome::Travel);
    REQUIRE(f->scene.secretTravel());
    CHECK(f->scene.destination().name == "S4");
    usize travelPlays = 0;
    const SoundPlayer::Capture listen(&f->sounds, [&](const auto& event) {
        travelPlays += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
    });
    REQUIRE_NOTHROW(f->scene.suspendForChallenge());
    CHECK(travelPlays == 1);
    CHECK(f->scene.recordedAudioEpoch() == 0);
    CHECK_FALSE(f->scene.canRestoreLocal(before));
    CHECK_FALSE(f->scene.restoreLocal(before));
    // A suspended stage resumes using a fresh epoch and fresh local snapshots.
    const auto party = f->scene.party();
    f->scene.resumeFromChallenge(party);
    REQUIRE(f->scene.beginRecordedAudio(kEpoch + 1));
    const auto resumed = f->scene.recordStep(kEpoch + 1, 1, kStep, {});
    REQUIRE(resumed.audio);
    CHECK(resumed.outcome == PlayOutcome::Running);
    f->scene.endRecordedAudio();
    CHECK(f->scene.recordedAudioEpoch() == 0);
}

TEST_CASE("native Thunder Hammer outputs replay without vibrating twice",
          "[play-scene-output][play-scene-output-rumble][assets]") {
    auto f = std::make_unique<Fixture>("G1", true);
    const auto beforeInvalid = f->scene.nativeAgreementDigest();
    REQUIRE(beforeInvalid);
    REQUIRE_THROWS_AS(f->scene.recordStep(0, 1, kStep, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(f->scene.recordStep(kEpoch, 0, kStep, {}), std::invalid_argument);
    CHECK(f->scene.nativeAgreementDigest() == beforeInvalid);
    PlayScene::Inputs strike;
    strike[3].attack = true;
    PlayScene::LocalSnapshot beforeImpact;
    Ledger::Journal impact;
    u64 impactTick = 0;
    for (u64 tick = 1; tick <= 240 && impactTick == 0; ++tick) {
        REQUIRE(f->scene.saveLocal(beforeImpact));
        strike[3].attackPressed = tick == 1;
        const auto output = f->step(tick, strike);
        for (const auto& event : output.journal.events) {
            if (const auto* rumble = std::get_if<PlayScene::RumbleOutput>(&event.payload);
                rumble != nullptr && rumble->feedback == ControlFeedback::ThunderHammer) {
                impactTick = tick;
                CHECK(rumble->player == 3);
                CHECK(rumble->frames == 0);
                CHECK(event.id.epoch == kEpoch);
                CHECK(event.id.tick == tick);
                CHECK(event.id.emitter.seat == 3);
                CHECK(event.id.occurrence == 0);
                CHECK(event.policy == Ledger::Policy::OneShot);
            }
        }
        if (impactTick != 0) {
            impact = output.journal;
        }
        REQUIRE(f->vibrations.empty());
    }
    REQUIRE(impactTick != 0);
    REQUIRE(f->scene.restoreLocal(beforeImpact));
    std::array<Ledger::Journal, 8> expected;
    for (usize index = 0; index < expected.size(); ++index) {
        expected[index] = f->step(impactTick + index, strike).journal;
    }
    CHECK(expected.front() == impact);
    REQUIRE(f->vibrations.empty());

    for (usize distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(f->scene.restoreLocal(beforeImpact));
        // A genuinely different branch disables the item before the release.
        for (auto& slot : f->player().save().progress().inventory.powerups) {
            slot.on = false;
        }
        for (usize index = 0; index < distance; ++index) {
            f->step(impactTick + index, {});
        }
        REQUIRE(f->scene.restoreLocal(beforeImpact));
        std::vector<Ledger::Journal> replay;
        for (usize index = 0; index < distance; ++index) {
            replay.push_back(f->step(impactTick + index, strike).journal);
            CHECK(replay.back() == expected[index]);
        }
        CHECK(f->vibrations.empty());
        Ledger ledger;
        REQUIRE(ledger.reset(kEpoch, impactTick));
        const auto first = ledger.replace(kEpoch, impactTick, std::span(expected).first(distance));
        f->dispatch(first);
        REQUIRE(std::ranges::any_of(f->vibrations, [](const auto& rumble) {
            return rumble.player == 3 && rumble.feedback == ControlFeedback::ThunderHammer;
        }));
        const auto count = f->vibrations.size();
        f->dispatch(ledger.replace(kEpoch, impactTick, replay));
        f->dispatch(ledger.confirm(kEpoch, impactTick + distance - 1));
        CHECK(f->vibrations.size() == count);
        f->vibrations.clear();
    }
    // The traditional update still dispatches the exact fresh cue after stepping.
    REQUIRE(f->scene.restoreLocal(beforeImpact));
    REQUIRE(f->scene.update(kStep, strike) == PlayOutcome::Running);
    CHECK(std::ranges::any_of(f->vibrations, [](const auto& rumble) {
        return rumble.player == 3 && rumble.feedback == ControlFeedback::ThunderHammer;
    }));
}

TEST_CASE("authored trap damage emits a value cue instead of calling the controller",
          "[play-scene-output][play-scene-output-damage][assets]") {
    auto f = std::make_unique<Fixture>("G1");
    const auto& traps = f->scene.traps();
    usize spike = traps.size();
    for (usize index = 0; index < traps.size(); ++index) {
        const auto& trap = traps.trap(index);
        if (trap.shown && !trap.gone && trap.subtype == Traps::kSpikes && trap.damage > 0) {
            spike = index;
            break;
        }
    }
    REQUIRE(spike < traps.size());
    f->player().place(traps.trap(spike).figure.position());
    const auto health = f->player().save().health();
    std::optional<Ledger::Event> damage;
    for (u64 tick = 1; tick <= 1800 && !damage; ++tick) {
        const auto result = f->step(tick);
        for (const auto& event : result.journal.events) {
            if (const auto* rumble = std::get_if<PlayScene::RumbleOutput>(&event.payload);
                rumble != nullptr && rumble->feedback == ControlFeedback::Damage) {
                damage = event;
            }
        }
        REQUIRE(f->vibrations.empty());
    }
    REQUIRE(damage);
    CHECK(f->player().save().health() < health);
    const auto& rumble = std::get<PlayScene::RumbleOutput>(damage->payload);
    CHECK(rumble.player == 3);
    CHECK(rumble.frames > 0);
    CHECK_FALSE(f->scene.dispatchOutput({Ledger::ActionKind::Commit, *damage}));
    CHECK(f->vibrations.empty());
    REQUIRE(f->scene.dispatchOutput({Ledger::ActionKind::Present, *damage}));
    CHECK(f->vibrations == std::vector{rumble});
}

TEST_CASE("native secret reward rewinds without persisting until its output is confirmed",
          "[play-scene-output][play-scene-output-unlock][assets]") {
    auto f = std::make_unique<Fixture>("S4");
    std::vector<usize> coins;
    for (usize index = 0; index < f->world.placedItems().size(); ++index) {
        const auto& item = f->world.placedItems().item(index);
        if (item.subtype == 1 && item.visible && !item.taken) {
            coins.push_back(index);
        }
    }
    REQUIRE_FALSE(coins.empty());
    PlayScene::LocalSnapshot beforeReward;
    Ledger::Journal reward;
    u64 rewardTick = 0;
    u64 tick = 0;
    Vec3 lastPosition{0};
    for (const usize index : coins) {
        if (f->world.placedItems().item(index).taken) {
            continue;
        }
        lastPosition = f->world.placedItems().item(index).position;
        f->player().place(lastPosition);
        REQUIRE(f->scene.saveLocal(beforeReward));
        const auto result = f->step(++tick);
        REQUIRE(f->unlocks.empty());
        if (f->scene.challenge().state() == SecretChallenge::State::Won) {
            rewardTick = tick;
            reward = result.journal;
            break;
        }
    }
    REQUIRE(rewardTick != 0);
    REQUIRE(f->scene.challenge().coinsLeft() == 0);
    const u16 mask = f->scene.challenge().rewardMask();
    REQUIRE(mask != 0);
    CHECK((f->player().save().classUnlock & mask) == mask);
    const auto found = std::ranges::find_if(reward.events, [](const auto& event) {
        return std::holds_alternative<PlayScene::UnlockClassesOutput>(event.payload);
    });
    REQUIRE(found != reward.events.end());
    CHECK(found->policy == Ledger::Policy::Confirmed);
    CHECK(found->id.emitter.seat == FrameEventEmitter::kNoSeat);
    CHECK_FALSE(f->scene.dispatchOutput({Ledger::ActionKind::Present, *found}));
    REQUIRE(f->unlocks.empty());
    Ledger ledger;
    REQUIRE(ledger.reset(kEpoch, rewardTick));
    const std::array published{reward};
    f->dispatch(ledger.replace(kEpoch, rewardTick, published));
    REQUIRE(f->unlocks.empty());

    REQUIRE(f->scene.restoreLocal(beforeReward));
    CHECK((f->player().save().classUnlock & mask) == 0);
    CHECK(f->scene.challenge().state() == SecretChallenge::State::Collecting);
    // Replaying from the preserved native contact produces the same immutable
    // reward, including identity, but no persistence callback.
    CHECK(f->player().position() == lastPosition);
    const auto replay = f->step(rewardTick);
    CHECK(replay.journal == reward);
    REQUIRE(f->unlocks.empty());
    const std::array replayed{replay.journal};
    f->dispatch(ledger.replace(kEpoch, rewardTick, replayed));
    REQUIRE(f->unlocks.empty());
    f->dispatch(ledger.confirm(kEpoch, rewardTick));
    REQUIRE(f->unlocks == std::vector<u16>{mask});
    f->dispatch(ledger.confirm(kEpoch, rewardTick));
    f->dispatch(ledger.replace(kEpoch, rewardTick, replayed));
    CHECK(f->unlocks.size() == 1);

    // Offline behavior retains its immediate post-step shared unlock callback.
    REQUIRE(f->scene.restoreLocal(beforeReward));
    f->unlocks.clear();
    REQUIRE(f->scene.update(kStep, {}) == PlayOutcome::Running);
    CHECK(f->unlocks == std::vector<u16>{mask});
}
} // namespace
