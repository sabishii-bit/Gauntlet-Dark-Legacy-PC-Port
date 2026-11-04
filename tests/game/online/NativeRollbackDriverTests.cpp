#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/AudioMixer.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterProfile.h"
#include "game/combat/PowerupFlags.h"
#include "game/online/NativeRollbackDriver.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Status = NativeRollbackDriver::Status;
using Admission = NativeRollbackDriver::Admission;
constexpr u64 kEpoch = 73;
constexpr f64 kStep = 1.0 / 60;

struct Fixture {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog levels;
    StringTable strings;
    LevelWorld world;
    GameServices game;
    MatchContext match;
    std::vector<PartyMember> party;
    test::RecordingHaptics physicalRumbles;
    test::RecordingUnlocks persistentUnlocks;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    PlayScene scene;
    NativeRollbackDriver driver;

    explicit Fixture(MatchOwners owners = {1, 2, 0, 0}, bool warm = true,
                     std::string_view levelName = "G1", bool audio = false) {
        REQUIRE(levels.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level = levels.byName(levelName);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        config.timing.tickRate = 60;
        game.config = &config;
        game.levels = &levels;
        game.strings = &strings;
        game.unpackedRoot = root;
        game.sounds = audio ? &sounds : nullptr;
        match.epoch = kEpoch;
        match.scene = 1;
        match.inputLead = 0;
        match.owners = owners;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] == 0) {
                continue;
            }
            match.grants[seat] = static_cast<u32>(seat + 40);
            CharacterSave save;
            save.name = "TEST";
            save.character = static_cast<s32>(seat);
            save.color = static_cast<s32>(seat);
            save.progress().health = 5000;
            save.progress().inventory.addPotions(1, 5);
            REQUIRE(CharacterProfile::capture(save));
            party.push_back({static_cast<s32>(seat), save});
        }
        open();
        if (warm) {
            for (u64 tick = 1; tick <= 480 &&
                               (scene.spawning() ||
                                std::ranges::any_of(scene.participants(),
                                                    [](const auto& player) {
                                                        return player.figure &&
                                                               player.figure->animator().entering();
                                                    }));
                 ++tick) {
                REQUIRE(scene.recordStep(1, tick, kStep, {}).outcome == PlayOutcome::Running);
            }
            REQUIRE_FALSE(scene.spawning());
            REQUIRE_FALSE(scene.startCamera().active());
        }
    }
    void open() {
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, game, world, party, options,
                           {.haptics = &physicalRumbles, .unlocks = &persistentUnlocks}));
    }
    std::vector<InputCommand> commands(u8 peer, u64 tick,
                                       const SessionInputs::Frame& frame = {}) const {
        std::vector<InputCommand> result;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(match.owners.size()); ++seat) {
            if (match.owners[seat] == peer) {
                result.push_back(
                    SessionInputs::command(frame[seat], kEpoch, tick, match.grants[seat], seat));
            }
        }
        return result;
    }
    void remote(u8 peer, u64 tick, const SessionInputs::Frame& frame = {}) {
        REQUIRE(driver.submit(peer, commands(peer, tick, frame)) == Admission::Accepted);
    }
    void progress(u64 tick) {
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (std::ranges::find(match.owners, peer) != match.owners.end()) {
                const auto result = driver.observePeerTick(peer, kEpoch, tick);
                REQUIRE((result == Admission::Accepted || result == Admission::Duplicate));
            }
        }
    }
    void predict(u64 count) {
        for (u64 tick = 1; tick <= count; ++tick) {
            progress(tick);
            REQUIRE(driver.sample({}));
            REQUIRE(driver.advance().status == Status::Advanced);
        }
    }
};
} // namespace

TEST_CASE("rollback agreement includes all native coordinator control fields",
          "[native-rollback-driver][native-rollback-digest]") {
    const NativeStateDigest::Digest scene{};
    NativeRollbackDriver::Control control;
    const auto initial = NativeRollbackDriver::agreementDigest(scene, control);
    for (usize seat = 0; seat < control.arrivalVotes.size(); ++seat) {
        control.arrivalVotes[seat] = true;
        CHECK(NativeRollbackDriver::agreementDigest(scene, control) != initial);
        control.arrivalVotes[seat] = false;
    }
    control.terminal = NativeRollbackDriver::Terminal{7, PlayOutcome::Travel};
    const auto travel = NativeRollbackDriver::agreementDigest(scene, control);
    CHECK(travel != initial);
    control.terminal->tick = 8;
    CHECK(NativeRollbackDriver::agreementDigest(scene, control) != travel);
    control.terminal->tick = 7;
    control.terminal->outcome = PlayOutcome::Leave;
    CHECK(NativeRollbackDriver::agreementDigest(scene, control) != travel);
}

TEST_CASE("a scene rebuilt from the confirmed journal stands where the live one stands",
          "[native-rollback-driver][journal][assets]") {
    auto live = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0});
    REQUIRE(live->driver.bind(live->scene, live->match, 1));
    SessionInputs::Frame walking;
    walking[0].move = {{0, 1}, 1};
    SessionInputs::Frame attacking;
    attacking[1].attack = true; // the remote peer owns seat one
    constexpr u64 kThrough = 90;
    for (u64 tick = 1; tick <= kThrough; ++tick) {
        live->remote(2, tick, tick >= 30 && tick < 70 ? attacking : SessionInputs::Frame{});
        live->progress(tick);
        REQUIRE(live->driver.sample(tick >= 20 && tick < 60 ? walking : SessionInputs::Frame{}));
        REQUIRE(live->driver.advance().status == Status::Advanced);
    }
    REQUIRE(live->driver.confirmedTick() == kThrough);
    const auto& journal = live->driver.journal();
    REQUIRE(journal.through() == kThrough);
    CHECK(journal.frame(25)->at(0).magnitude == 1);
    CHECK(journal.frame(40)->at(1).held(CommandHeld::Attack));

    auto rebuilt = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0});
    REQUIRE(rebuilt->driver.bind(rebuilt->scene, rebuilt->match, 1));
    CHECK_FALSE(rebuilt->driver.finishRebuild());
    std::vector<InputCommand> commands;
    for (const auto& frame : journal.frames()) {
        commands.push_back(frame[0]);
        commands.push_back(frame[1]);
    }
    REQUIRE(rebuilt->driver.adopt(commands));
    REQUIRE(rebuilt->driver.beginRebuild(kThrough));
    CHECK(rebuilt->driver.rebuilding());
    usize digests = 0;
    while (rebuilt->driver.nextTick() <= kThrough) {
        const auto result = rebuilt->driver.advance();
        REQUIRE(result.status == Status::Advanced);
        digests += result.digests.size();
    }
    // Nothing replayed up to the resync point is reported; agreement restarts past it.
    CHECK(digests == 0);
    REQUIRE(rebuilt->driver.finishRebuild());
    CHECK_FALSE(rebuilt->driver.rebuilding());
    CHECK(rebuilt->driver.confirmedTick() == kThrough);
    CHECK(rebuilt->driver.sampleTick() == kThrough + 1);
    CHECK(rebuilt->driver.journal().through() == kThrough);
    REQUIRE(rebuilt->scene.nativeAgreementDigest());
    CHECK(rebuilt->scene.nativeAgreementDigest() == live->scene.nativeAgreementDigest());
    CHECK(rebuilt->driver.statistics().corrections == 0);
    // Both continue in step from there.
    for (auto* fixture : {live.get(), rebuilt.get()}) {
        fixture->remote(2, kThrough + 1, attacking);
        fixture->progress(kThrough + 1);
        REQUIRE(fixture->driver.sample(walking));
        REQUIRE(fixture->driver.advance().status == Status::Advanced);
    }
    CHECK(rebuilt->scene.nativeAgreementDigest() == live->scene.nativeAgreementDigest());
    CHECK(rebuilt->driver.journal().frames().back()[0].magnitude == 1);
}

TEST_CASE("a bound scene returned to its epoch's start replays the journal where no load can",
          "[native-rollback-driver][native-rollback-rebind][journal][assets]") {
    using Mode = NativeRollbackDriver::AudioMode;
    auto live = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    auto diverged = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    SessionInputs::Frame walking;
    walking[0].move = {{0, 1}, 1};
    constexpr u64 kThrough = 60;
    const auto play = [&](Fixture& fixture, u64 tick) {
        fixture.remote(2, tick);
        fixture.progress(tick);
        REQUIRE(fixture.driver.sample((tick < 40 || tick > kThrough) ? walking
                                                                     : SessionInputs::Frame{}));
        const auto result = fixture.driver.advance();
        REQUIRE(result.status == Status::Advanced);
        REQUIRE(result.audio);
        fixture.scene.dispatchRecordedAudio(*result.audio);
    };
    for (auto* fixture : {live.get(), diverged.get()}) {
        REQUIRE(fixture->scene.beginRecordedAudio(kEpoch));
        REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 1, Mode::Required));
        for (u64 tick = 1; tick <= kThrough; ++tick) {
            play(*fixture, tick);
        }
        REQUIRE(fixture->driver.confirmedTick() == kThrough);
    }
    REQUIRE(diverged->scene.nativeAgreementDigest() == live->scene.nativeAgreementDigest());
    // A divergence planted in one scene, as though its epoch began where no load puts it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): planted divergence
    const_cast<PlayerActor*>(diverged->scene.actor(0))->save().gold += 7;
    REQUIRE(diverged->scene.nativeAgreementDigest() != live->scene.nativeAgreementDigest());
    std::vector<InputCommand> commands;
    for (const auto& frame : diverged->driver.journal().frames()) {
        commands.push_back(frame[0]);
        commands.push_back(frame[1]);
    }
    REQUIRE(diverged->driver.restoreBinding());
    CHECK_FALSE(diverged->driver.restoreBinding());
    // The epoch binds again only through rebind, once its sound is recorded afresh.
    REQUIRE(diverged->scene.restartRecordedAudio(kEpoch));
    CHECK_FALSE(diverged->driver.bind(diverged->scene, diverged->match, 1, Mode::Required));
    REQUIRE(diverged->driver.rebind(diverged->scene, diverged->match, 1, Mode::Required));
    REQUIRE(diverged->driver.adopt(commands));
    REQUIRE(diverged->driver.beginRebuild(kThrough));
    while (diverged->driver.nextTick() <= kThrough) {
        REQUIRE(diverged->driver.advance().status == Status::Advanced);
    }
    REQUIRE(diverged->driver.finishRebuild());
    CHECK(diverged->driver.confirmedTick() == kThrough);
    CHECK(diverged->scene.nativeAgreementDigest() == live->scene.nativeAgreementDigest());
    // Both go on in step, the restored scene recording the same epoch's sound again.
    for (auto* fixture : {live.get(), diverged.get()}) {
        play(*fixture, kThrough + 1);
    }
    CHECK(diverged->scene.nativeAgreementDigest() == live->scene.nativeAgreementDigest());
}

TEST_CASE("a dropped peer's character waits in the tower from its first still tick on every peer",
          "[native-rollback-driver][drop][assets]") {
    // Two survivors of a three-peer room learn that peer three holds still from tick 40: one
    // before it runs that tick, the other only after running past it on a guess.
    constexpr u64 kStill = 40;
    constexpr u64 kLearned = 44;
    constexpr u64 kThrough = 60;
    constexpr s32 kDroppedSeat = 2;
    const MatchOwners owners{1, 2, 3, 0};
    auto early = std::make_unique<Fixture>(owners);
    auto late = std::make_unique<Fixture>(owners);
    for (auto* fixture : {early.get(), late.get()}) {
        REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 1));
    }
    REQUIRE(early->driver.suspend(3, kStill));
    for (u64 tick = 1; tick <= kThrough; ++tick) {
        for (auto* fixture : {early.get(), late.get()}) {
            const bool learnsLate = fixture == late.get();
            CAPTURE(tick, learnsLate);
            fixture->remote(2, tick);
            if (tick < kStill) {
                fixture->remote(3, tick);
            }
            fixture->progress(tick);
            REQUIRE(fixture->driver.sample({}));
            REQUIRE(fixture->driver.advance().status == Status::Advanced);
        }
        if (tick == kLearned) {
            REQUIRE(late->driver.suspend(3, kStill));
        }
    }
    for (auto* fixture : {early.get(), late.get()}) {
        const bool learnsLate = fixture == late.get();
        CAPTURE(learnsLate);
        CHECK(fixture->driver.confirmedTick() == kThrough);
        const auto* dropped = fixture->scene.runtime(kDroppedSeat);
        REQUIRE(dropped != nullptr);
        CHECK(dropped->life == PlayerLife::InTower);
        CHECK_FALSE(dropped->towerPrompt);
        CHECK_FALSE(dropped->departed);
        REQUIRE(fixture->scene.runtime(1) != nullptr);
        CHECK(fixture->scene.runtime(1)->life == PlayerLife::Standing);
    }
    CHECK(early->driver.statistics().corrections == 0);
    CHECK(late->driver.statistics().corrections == 1);
    REQUIRE(early->scene.nativeAgreementDigest());
    CHECK(early->scene.nativeAgreementDigest() == late->scene.nativeAgreementDigest());
}

TEST_CASE("a guest rebuilt after a drop sends the dropped character to the tower when the host did",
          "[native-rollback-driver][drop][journal][assets]") {
    // The rebuild learns of the drop first, as an online rebuild applies drops before the
    // journal, then replays the host's journal through and past the still tick.
    constexpr u64 kStill = 20;
    constexpr u64 kThrough = 40;
    constexpr s32 kDroppedSeat = 2;
    const MatchOwners owners{1, 2, 3, 0};
    auto host = std::make_unique<Fixture>(owners);
    REQUIRE(host->driver.bind(host->scene, host->match, 1));
    REQUIRE(host->driver.suspend(3, kStill));
    for (u64 tick = 1; tick <= kThrough; ++tick) {
        host->remote(2, tick);
        if (tick < kStill) {
            host->remote(3, tick);
        }
        host->progress(tick);
        REQUIRE(host->driver.sample({}));
        REQUIRE(host->driver.advance().status == Status::Advanced);
    }
    REQUIRE(host->driver.confirmedTick() == kThrough);
    auto rebuilt = std::make_unique<Fixture>(owners);
    REQUIRE(rebuilt->driver.bind(rebuilt->scene, rebuilt->match, 2));
    REQUIRE(rebuilt->driver.suspend(3, kStill));
    const auto chunks = JournalPacket::chunks(host->driver.journal(), 1, kThrough, true);
    REQUIRE_FALSE(chunks.empty());
    for (const auto& chunk : chunks) {
        const auto bytes = JournalPacket::encode(chunk);
        REQUIRE(bytes);
        const auto decoded = JournalPacket::decode(*bytes);
        REQUIRE(decoded);
        REQUIRE(rebuilt->driver.adopt(decoded->commands));
    }
    REQUIRE(rebuilt->driver.beginRebuild(kThrough));
    while (rebuilt->driver.nextTick() <= kThrough) {
        REQUIRE(rebuilt->driver.advance().status == Status::Advanced);
    }
    REQUIRE(rebuilt->driver.finishRebuild());
    for (auto* fixture : {host.get(), rebuilt.get()}) {
        const auto* dropped = fixture->scene.runtime(kDroppedSeat);
        REQUIRE(dropped != nullptr);
        CHECK(dropped->life == PlayerLife::InTower);
    }
    REQUIRE(host->scene.nativeAgreementDigest());
    CHECK(rebuilt->scene.nativeAgreementDigest() == host->scene.nativeAgreementDigest());
}

TEST_CASE("late host admitted console input rewinds all peers and survives journal rebuilding once",
          "[native-rollback-driver][online-console][journal][assets]") {
    const u8 peers = GENERATE(u8{2}, u8{4});
    const MatchOwners owners = peers == 2 ? MatchOwners{1, 2, 0, 0} : MatchOwners{1, 2, 3, 4};
    const auto target = static_cast<u8>(peers - 1);
    auto reference = std::make_unique<Fixture>(owners);
    auto delayed = std::make_unique<Fixture>(owners);
    for (auto* fixture : {reference.get(), delayed.get()}) {
        REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 2));
        // A guest may not attach a mutation directly to its locally owned input.
        CHECK_FALSE(fixture->driver.sample({}, ConsoleAction{target, "get gold 321"}));
        CHECK(fixture->driver.failure() == NativeRollbackDriver::Failure::None);
        auto spoof = fixture->commands(2, 1);
        spoof.front().console = ConsoleAction{target, "get gold 999"};
        CHECK(fixture->driver.submit(2, spoof) == Admission::WrongPeer);
    }
    constexpr u64 kActionTick = 3;
    constexpr u64 kThrough = 8;
    usize referenceReplies = 0;
    usize delayedReplies = 0;
    const auto commands = [&](Fixture& fixture, u8 peer, u64 tick) {
        auto input = fixture.commands(peer, tick);
        if (peer == 1 && tick == kActionTick) {
            input.front().console = ConsoleAction{target, "get gold 321"};
        }
        return input;
    };
    for (u64 tick = 1; tick <= kThrough; ++tick) {
        for (auto* fixture : {reference.get(), delayed.get()}) {
            for (u8 peer = 1; peer <= peers; ++peer) {
                if (peer == 2) {
                    continue;
                }
                if (fixture != delayed.get() || peer != 1 || tick != kActionTick) {
                    REQUIRE(fixture->driver.submit(peer, commands(*fixture, peer, tick)) ==
                            Admission::Accepted);
                }
                const auto progress = fixture->driver.observePeerTick(peer, kEpoch, tick);
                REQUIRE((progress == Admission::Accepted || progress == Admission::Duplicate));
            }
            REQUIRE(fixture->driver.sample({}));
            const auto result = fixture->driver.advance();
            REQUIRE(result.status == Status::Advanced);
            if (fixture == reference.get()) {
                referenceReplies += result.consoleReplies.size();
            } else {
                delayedReplies += result.consoleReplies.size();
            }
        }
    }
    CHECK(referenceReplies == 1);
    CHECK(delayedReplies == 0);
    CHECK(delayed->driver.confirmedTick() == kActionTick - 1);
    CHECK(delayed->scene.actor(target)->save().gold == 0);
    CHECK(reference->scene.actor(target)->save().gold == 321);

    const auto missing = commands(*delayed, 1, kActionTick);
    REQUIRE(delayed->driver.submit(1, missing) == Admission::Accepted);
    const auto corrected = delayed->driver.advance(false);
    REQUIRE(corrected.status == Status::Held);
    CHECK(delayed->driver.statistics().corrections == 1);
    CHECK(delayed->driver.statistics().replayTicks == kThrough - kActionTick + 1);
    REQUIRE(corrected.consoleReplies.size() == 1);
    CHECK(corrected.consoleReplies.front().seat == target);
    CHECK_FALSE(corrected.consoleReplies.front().text.empty());
    CHECK(delayed->driver.confirmedTick() == kThrough);
    REQUIRE(reference->scene.nativeAgreementDigest());
    CHECK(delayed->scene.nativeAgreementDigest() == reference->scene.nativeAgreementDigest());
    for (u8 seat = 0; seat < peers; ++seat) {
        CHECK(delayed->scene.actor(seat)->save().gold == (seat == target ? 321 : 0));
    }
    REQUIRE(delayed->driver.submit(1, missing) == Admission::Duplicate);
    CHECK(delayed->driver.advance(false).consoleReplies.empty());
    CHECK(delayed->scene.actor(target)->save().gold == 321);

    // The reliable resync payload must retain the command, but rebuilding an
    // already confirmed tick must not print the command's reply a second time.
    auto rebuilt = std::make_unique<Fixture>(owners);
    REQUIRE(rebuilt->driver.bind(rebuilt->scene, rebuilt->match, 2));
    const auto chunks = JournalPacket::chunks(delayed->driver.journal(), 1, kThrough, true);
    REQUIRE_FALSE(chunks.empty());
    usize consoleCommands = 0;
    for (const auto& chunk : chunks) {
        const auto bytes = JournalPacket::encode(chunk);
        REQUIRE(bytes);
        const auto decoded = JournalPacket::decode(*bytes);
        REQUIRE(decoded);
        for (const auto& command : decoded->commands) {
            consoleCommands += command.console.has_value() ? 1 : 0;
        }
        REQUIRE(rebuilt->driver.adopt(decoded->commands));
    }
    CHECK(consoleCommands == 1);
    REQUIRE(rebuilt->driver.beginRebuild(kThrough));
    while (rebuilt->driver.nextTick() <= kThrough) {
        const auto result = rebuilt->driver.advance();
        REQUIRE(result.status == Status::Advanced);
        CHECK(result.consoleReplies.empty());
    }
    REQUIRE(rebuilt->driver.finishRebuild());
    CHECK(rebuilt->scene.nativeAgreementDigest() == reference->scene.nativeAgreementDigest());
    CHECK(rebuilt->scene.actor(target)->save().gold == 321);
}

TEST_CASE("borrowed console births survive late input correction and a fresh audio journal rebuild",
          "[native-rollback-driver][online-console][journal][native-console-spawn][assets]") {
    test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2");
    auto reference = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    auto delayed = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    const auto source = reference->levels.byName("E1");
    REQUIRE(source);
    WorldLayout borrowedLayout;
    ModelSet borrowedModels;
    REQUIRE(borrowedLayout.load(reference->root / source->directory));
    REQUIRE(borrowedModels.load(reference->root / source->directory));
    const auto object = std::ranges::find_if(borrowedLayout.objects(), [&](const auto& value) {
        return !value.particles() && borrowedModels.find(value.name).has_value();
    });
    REQUIRE(object != borrowedLayout.objects().end());
    const std::array<std::string, 3> actions{"spawn generator special E1",
                                             "spawn object " + object->name + " E1",
                                             "spawn enemy gru 2 1 E1"};
    const auto generatorsBefore = reference->scene.generators().count();
    for (auto* fixture : {reference.get(), delayed.get()}) {
        REQUIRE(fixture->scene.beginRecordedAudio(kEpoch));
        REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 2,
                                     NativeRollbackDriver::AudioMode::Required));
    }
    constexpr u64 kMissing = 2;
    constexpr u64 kThrough = 8;
    const auto commands = [&](Fixture& fixture, u64 tick) {
        auto result = fixture.commands(1, tick);
        if (tick >= 2 && tick <= 6 && tick % 2 == 0) {
            result.front().console = ConsoleAction{1, actions[static_cast<usize>(tick / 2 - 1)]};
        }
        return result;
    };
    usize referenceReplies = 0;
    for (u64 tick = 1; tick <= kThrough; ++tick) {
        for (auto* fixture : {reference.get(), delayed.get()}) {
            if (fixture != delayed.get() || tick != kMissing) {
                REQUIRE(fixture->driver.submit(1, commands(*fixture, tick)) == Admission::Accepted);
            }
            const auto progress = fixture->driver.observePeerTick(1, kEpoch, tick);
            REQUIRE((progress == Admission::Accepted || progress == Admission::Duplicate));
            REQUIRE(fixture->driver.sample({}));
            const auto result = fixture->driver.advance();
            REQUIRE(result.status == Status::Advanced);
            REQUIRE(result.audio);
            if (fixture == reference.get()) {
                referenceReplies += result.consoleReplies.size();
                for (const auto& reply : result.consoleReplies) {
                    CAPTURE(reply.text);
                    CHECK(reply.seat == 1);
                    CHECK_FALSE(reply.text.starts_with("No "));
                    CHECK_FALSE(reply.text.starts_with("Spawned 0 "));
                }
            } else {
                CHECK(result.consoleReplies.empty());
            }
        }
    }
    CHECK(referenceReplies == actions.size());
    CHECK(reference->scene.generators().count() == generatorsBefore + 1);
    CHECK(reference->scene.spawnedObjects().count() == 1);
    CHECK(delayed->scene.generators().count() == generatorsBefore);
    CHECK(delayed->scene.spawnedObjects().count() == 1);
    const auto bindingsBefore = delayed->scene.spawnedObjects().bindingCount();
    REQUIRE(delayed->driver.submit(1, commands(*delayed, kMissing)) == Admission::Accepted);
    const auto corrected = delayed->driver.advance(false);
    REQUIRE(corrected.status == Status::Held);
    REQUIRE(corrected.audio);
    CHECK(corrected.consoleReplies.size() == actions.size());
    CHECK(delayed->driver.statistics().replayTicks == kThrough - kMissing + 1);
    CHECK(delayed->scene.generators().count() == generatorsBefore + 1);
    CHECK(delayed->scene.spawnedObjects().count() == 1);
    CHECK(delayed->scene.spawnedObjects().bindingCount() == bindingsBefore);
    REQUIRE(reference->scene.nativeAgreementDigest());
    CHECK(delayed->scene.nativeAgreementDigest() == reference->scene.nativeAgreementDigest());

    // A new process has none of the abandoned prediction's retained archives or
    // banks. The input journal must recreate the same logical roster and sounds.
    auto rebuilt = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    REQUIRE(rebuilt->scene.beginRecordedAudio(kEpoch));
    REQUIRE(rebuilt->driver.bind(rebuilt->scene, rebuilt->match, 2,
                                 NativeRollbackDriver::AudioMode::Required));
    const auto chunks = JournalPacket::chunks(delayed->driver.journal(), 1, kThrough, true);
    REQUIRE_FALSE(chunks.empty());
    for (const auto& chunk : chunks) {
        const auto encoded = JournalPacket::encode(chunk);
        REQUIRE(encoded);
        const auto decoded = JournalPacket::decode(*encoded);
        REQUIRE(decoded);
        REQUIRE(rebuilt->driver.adopt(decoded->commands));
    }
    REQUIRE(rebuilt->driver.beginRebuild(kThrough));
    while (rebuilt->driver.nextTick() <= kThrough) {
        const auto result = rebuilt->driver.advance();
        REQUIRE(result.status == Status::Advanced);
        CHECK(result.consoleReplies.empty());
    }
    REQUIRE(rebuilt->driver.finishRebuild());
    CHECK(rebuilt->scene.generators().count() == generatorsBefore + 1);
    CHECK(rebuilt->scene.spawnedObjects().count() == 1);
    CHECK(rebuilt->scene.nativeAgreementDigest() == reference->scene.nativeAgreementDigest());
}

TEST_CASE("rollback hashes the corrected saved frame only after every seat confirms it",
          "[native-rollback-driver][native-rollback-digest][assets]") {
    const bool mismatched = GENERATE(false, true);
    auto reference = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 4});
    auto delayed = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 4});
    REQUIRE(reference->driver.bind(reference->scene, reference->match, 1));
    REQUIRE(delayed->driver.bind(delayed->scene, delayed->match, 1));
    std::optional<NativeRollbackDriver::ConfirmedDigest> expected;
    PlayScene::LocalSnapshot predictedThirty;
    std::optional<NativeStateDigest::Digest> predictedHash;
    SessionInputs::Frame thirtieth;
    if (mismatched) {
        thirtieth[3].move = {{1, 0}, 1};
    }
    for (u64 tick = 1; tick <= 37; ++tick) {
        const auto remote = tick == 30 ? thirtieth : SessionInputs::Frame{};
        for (auto* fixture : {reference.get(), delayed.get()}) {
            fixture->remote(2, tick);
            fixture->remote(3, tick);
            if (fixture == reference.get() || tick != 30) {
                fixture->remote(4, tick, remote);
            }
            fixture->progress(tick);
            REQUIRE(fixture->driver.sample({}));
            const auto result = fixture->driver.advance();
            REQUIRE(result.status == Status::Advanced);
            if (fixture == reference.get() && tick == 30) {
                REQUIRE(result.digests.size() == 1);
                expected = result.digests.front();
            } else {
                CHECK(result.digests.empty());
            }
        }
        if (tick == 30) {
            REQUIRE(delayed->scene.saveLocal(predictedThirty));
            predictedHash = delayed->scene.nativeLocalDigest(predictedThirty);
            REQUIRE(predictedHash);
        }
    }
    REQUIRE(expected);
    REQUIRE(expected->tick == 30);
    CHECK(delayed->driver.confirmedTick() == 29);
    CHECK(delayed->driver.nextTick() == 38);
    CHECK(delayed->scene.nativeLocalDigest(predictedThirty) == predictedHash);
    const auto current = delayed->scene.nativeAgreementDigest();
    REQUIRE(current);
    CHECK(*current != *predictedHash);
    delayed->remote(4, 30, thirtieth);
    const auto confirmed = delayed->driver.advance(false);
    REQUIRE(confirmed.status != Status::Failed);
    REQUIRE(confirmed.digests.size() == 1);
    CHECK(confirmed.digests.front() == *expected);
    CHECK(delayed->driver.confirmedTick() == 37);
    CHECK(confirmed.frames.size() == (mismatched ? 8 : 0));
    CHECK(delayed->scene.nativeAgreementDigest() == reference->scene.nativeAgreementDigest());
    CHECK(delayed->driver.advance(false).digests.empty());
}

TEST_CASE("confirmed rollback hashes distinguish pending non-unanimous arrival votes",
          "[native-rollback-driver][native-rollback-digest][native-rollback-votes][assets]") {
    std::array<std::optional<NativeStateDigest::Digest>, 2> scenes;
    std::array<std::optional<NativeRollbackDriver::ConfirmedDigest>, 2> agreements;
    for (usize voted = 0; voted < 2; ++voted) {
        auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, false, "G5");
        for (u64 tick = 1;
             tick <= 100 && fixture->scene.startCamera().ticksLeft() > StartCamera::kSkipBelow;
             ++tick) {
            REQUIRE(fixture->scene.recordStep(1, tick, kStep, {}).outcome == PlayOutcome::Running);
        }
        REQUIRE(fixture->scene.startCamera().phase() == StartCamera::Phase::Hold);
        REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 1));
        for (u64 tick = 1; tick <= 30; ++tick) {
            SessionInputs::Frame local;
            local[0].menu.select = voted != 0 && tick == 1;
            fixture->remote(2, tick);
            fixture->progress(tick);
            REQUIRE(fixture->driver.sample(local));
            const auto result = fixture->driver.advance();
            REQUIRE(result.status == Status::Advanced);
            if (tick == 30) {
                REQUIRE(result.digests.size() == 1);
                agreements[voted] = result.digests.front();
            }
        }
        CHECK(fixture->driver.arrivalVotes()[0] == (voted != 0));
        CHECK_FALSE(fixture->driver.arrivalVotes()[1]);
        REQUIRE(fixture->scene.startCamera().phase() == StartCamera::Phase::Hold);
        scenes[voted] = fixture->scene.nativeAgreementDigest();
        REQUIRE(scenes[voted]);
    }
    REQUIRE(agreements[0]);
    REQUIRE(agreements[1]);
    CHECK(scenes[0] == scenes[1]);
    CHECK(agreements[0]->digest != agreements[1]->digest);
}

TEST_CASE("native rollback binds a frozen loaded scene and refuses stale resource lifetimes",
          "[native-rollback-driver][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 0, 2, 0}, false);
    auto& driver = fixture->driver;
    auto wrong = fixture->match;
    wrong.inputLead = RollbackInputs::kMaxDelay + 1;
    CHECK_FALSE(driver.bind(fixture->scene, wrong, 1));
    wrong = fixture->match;
    wrong.tickRate = 30;
    CHECK_FALSE(driver.bind(fixture->scene, wrong, 1));
    wrong = fixture->match;
    wrong.owners[2] = 0;
    wrong.grants[2] = 0;
    CHECK_FALSE(driver.bind(fixture->scene, wrong, 1));
    CHECK_FALSE(driver.bind(fixture->scene, fixture->match, 3));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& profile = const_cast<PlayerActor*>(fixture->scene.actor(0))->save();
    profile.name = "TOO-LONG";
    CHECK_FALSE(driver.bind(fixture->scene, fixture->match, 1));
    profile.name = "TEST";
    REQUIRE(driver.bind(fixture->scene, fixture->match, 2));
    CHECK_FALSE(driver.bind(fixture->scene, fixture->match, 2));
    SessionInputs::Frame local;
    local[0].move = {{1, 0}, 1};
    local[0].attackPressed = true;
    REQUIRE(driver.sample(local));
    REQUIRE(driver.redundantInputs().size() == 1);
    CHECK(driver.redundantInputs().front().seat == 2);
    CHECK(driver.redundantInputs().front().pressed(CommandPress::Attack));
    fixture->open(); // Same object address, different archive/owner generation.
    const auto cameraTicks = fixture->scene.startCamera().ticksLeft();
    CHECK(driver.advance().status == Status::Failed);
    CHECK(driver.failure() == NativeRollbackDriver::Failure::Binding);
    CHECK(fixture->scene.startCamera().ticksLeft() == cameraTicks);
    CHECK_FALSE(driver.sample({}));
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
}

TEST_CASE("native rollback holds beyond its window and preserves unsampled taps exactly once",
          "[native-rollback-driver][assets]") {
    auto fixture = std::make_unique<Fixture>();
    auto& driver = fixture->driver;
    REQUIRE(driver.bind(fixture->scene, fixture->match, 1));
    fixture->predict(RollbackInputs::kPredictionTicks);
    REQUIRE(driver.nextTick() == 9);
    fixture->progress(9);
    REQUIRE(driver.sample({})); // Tick 9 exists, but the remote window is exhausted.
    PlayScene::LocalSnapshot held;
    REQUIRE(fixture->scene.saveLocal(held));
    for (usize poll = 0; poll < 20; ++poll) {
        SessionInputs::Frame local;
        local[0].attack = local[0].attackPressed = poll == 3;
        local[0].usePotion = poll == 4;
        REQUIRE(driver.sample(local));
        const auto result = driver.advance();
        CHECK(result.status == Status::Held);
        CHECK(result.frames.empty());
        CHECK(result.actions.empty());
        CHECK(fixture->scene.matchesLocalControl(held));
        CHECK(driver.nextTick() == 9);
        CHECK(driver.sampleTick() == 10);
    }
    for (u64 tick = 1; tick <= 11; ++tick) {
        fixture->remote(2, tick);
    }
    REQUIRE(driver.advance(false).status == Status::Held);
    CHECK(driver.confirmedTick() == 8);
    const auto ninth = driver.advance();
    REQUIRE(ninth.frames.size() == 1);
    CHECK_FALSE(ninth.frames.front().input.commands[0].pressed(CommandPress::Attack));
    REQUIRE(driver.sample({}));
    fixture->progress(10);
    const auto tenth = driver.advance();
    REQUIRE(tenth.frames.size() == 1);
    CHECK(tenth.frames.front().input.commands[0].pressed(CommandPress::Attack));
    CHECK(tenth.frames.front().input.commands[0].held(CommandHeld::Attack));
    CHECK(tenth.frames.front().input.commands[0].pressed(CommandPress::UsePotion));
    REQUIRE(driver.sample({}));
    fixture->progress(11);
    const auto eleventh = driver.advance();
    REQUIRE(eleventh.frames.size() == 1);
    CHECK_FALSE(eleventh.frames.front().input.commands[0].pressed(CommandPress::Attack));
    CHECK_FALSE(eleventh.frames.front().input.commands[0].held(CommandHeld::Attack));
    CHECK_FALSE(eleventh.frames.front().input.commands[0].pressed(CommandPress::UsePotion));
    CHECK(driver.confirmedTick() == 11);
    CHECK(driver.statistics().stalls == 20);
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
}

TEST_CASE("native rollback clear retains epoch admission across healthy scene bindings",
          "[native-rollback-driver][native-rollback-rebind][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, false);
    auto& driver = fixture->driver;
    auto next = fixture->match;
    REQUIRE(driver.bind(fixture->scene, next, 1));
    driver.clear();
    CHECK(driver.failure() == NativeRollbackDriver::Failure::None);
    CHECK_FALSE(driver.bind(fixture->scene, next, 1));
    --next.epoch;
    CHECK_FALSE(driver.bind(fixture->scene, next, 1));
    next.epoch = kEpoch + 1;
    REQUIRE(driver.bind(fixture->scene, next, 1));
    CHECK(driver.submit(2, fixture->commands(2, 1)) == Admission::WrongEpoch);
    REQUIRE(driver.sample({}));
    REQUIRE(driver.advance().status == Status::Advanced);
    driver.clear();
    fixture->open();
    CHECK_FALSE(driver.bind(fixture->scene, next, 1));
    ++next.epoch;
    REQUIRE(driver.bind(fixture->scene, next, 1));
}

TEST_CASE("shipping-order sampling admits the captured tick immediately after correction",
          "[native-rollback-driver][native-rollback-pending][assets]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    auto& driver = fixture->driver;
    fixture->match.inputLead = delay;
    REQUIRE(fixture->scene.beginRecordedAudio(kEpoch));
    REQUIRE(
        driver.bind(fixture->scene, fixture->match, 1, NativeRollbackDriver::AudioMode::Required));
    usize physicalAudio = 0;
    const SoundPlayer::Capture watch(&fixture->sounds, [&](const auto&) { ++physicalAudio; });
    fixture->predict(4);

    SessionInputs::Frame remote;
    remote[1].move = {{1, 0}, 1};
    fixture->remote(2, static_cast<u64>(delay) + 1, remote);
    REQUIRE(driver.mismatchTick() == static_cast<u64>(delay) + 1);
    const u64 capturedTick = driver.sampleTick();
    REQUIRE(capturedTick == 5 + static_cast<u64>(delay));
    const auto before = InputPacket::encode(driver.redundantInputs());
    REQUIRE(before);
    SessionInputs::Frame local;
    local[0].attack = local[0].attackPressed = local[0].usePotion = true;
    REQUIRE(driver.sample(local)); // A pending mismatch temporarily blocks admission.
    CHECK(driver.sampleTick() == capturedTick);
    CHECK(InputPacket::encode(driver.redundantInputs()) == before);

    fixture->progress(5);
    const auto corrected = driver.advance();
    REQUIRE(corrected.status == Status::Advanced);
    REQUIRE_FALSE(corrected.frames.empty());
    CHECK(corrected.frames.front().replay);
    CHECK_FALSE(corrected.frames.back().replay);
    CHECK(corrected.frames.back().tick == 5);
    CHECK(driver.sampleTick() == capturedTick + 1);
    REQUIRE(corrected.audio);
    const auto admitted =
        std::ranges::find(driver.redundantInputs(), capturedTick, &InputCommand::tick);
    REQUIRE(admitted != driver.redundantInputs().end());
    CHECK(admitted->pressed(CommandPress::Attack));
    CHECK(admitted->held(CommandHeld::Attack));
    CHECK(admitted->pressed(CommandPress::UsePotion));

    auto applied = corrected.frames.back().input.commands[0];
    for (u64 tick = 6; tick <= capturedTick; ++tick) {
        fixture->progress(tick);
        REQUIRE(driver.sample({}));
        const auto forward = driver.advance();
        REQUIRE(forward.status == Status::Advanced);
        REQUIRE(forward.frames.size() == 1);
        CHECK_FALSE(forward.frames.front().replay);
        applied = forward.frames.front().input.commands[0];
    }
    CHECK(applied.tick == capturedTick);
    CHECK(applied.pressed(CommandPress::Attack));
    CHECK(applied.held(CommandHeld::Attack));
    CHECK(applied.pressed(CommandPress::UsePotion));
    fixture->progress(capturedTick + 1);
    REQUIRE(driver.sample({}));
    const auto next = driver.advance();
    REQUIRE(next.status == Status::Advanced);
    REQUIRE(next.frames.size() == 1);
    CHECK_FALSE(next.frames.front().input.commands[0].pressed(CommandPress::Attack));
    CHECK_FALSE(next.frames.front().input.commands[0].held(CommandHeld::Attack));
    CHECK_FALSE(next.frames.front().input.commands[0].pressed(CommandPress::UsePotion));
    CHECK(driver.statistics().corrections == 1);
    CHECK(physicalAudio == 0);
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
}

TEST_CASE("local pause clears only its seat's unsampled controls without rewriting history",
          "[native-rollback-driver][native-rollback-pending][assets]") {
    NativeRollbackDriver unbound;
    CHECK_FALSE(unbound.clearPendingInput(0));
    CHECK(unbound.failure() == NativeRollbackDriver::Failure::None);
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 1, 0}, true, "G1", true);
    auto& driver = fixture->driver;
    REQUIRE(fixture->scene.beginRecordedAudio(kEpoch));
    REQUIRE(
        driver.bind(fixture->scene, fixture->match, 1, NativeRollbackDriver::AudioMode::Required));
    CHECK(driver.clearPendingInput(0)); // A pause need not have a queued press.
    CHECK(driver.clearPendingInput(2));
    usize physicalAudio = 0;
    const SoundPlayer::Capture watch(&fixture->sounds, [&](const auto&) { ++physicalAudio; });
    fixture->predict(RollbackInputs::kPredictionTicks);
    fixture->progress(9);
    SessionInputs::Frame admitted;
    admitted[0].attack = admitted[0].attackPressed = true;
    REQUIRE(driver.sample(admitted)); // Tick 9 is already immutable actual input.
    REQUIRE(driver.advance().status == Status::Held);
    REQUIRE(driver.sampleTick() == 10);
    const auto before = InputPacket::encode(driver.redundantInputs());
    REQUIRE(before);

    SessionInputs::Frame pending;
    pending[0].move = {{1, 0}, 1};
    pending[0].aimDirection = Vec3{1, 0, 0};
    pending[0].attack = pending[0].attackPressed = pending[0].usePotion = true;
    pending[0].menu.start = true;
    pending[1].attack = pending[1].attackPressed = pending[1].usePotion = true;
    REQUIRE(driver.sample(pending));          // Local index 1 is canonical seat 2.
    CHECK_FALSE(driver.clearPendingInput(1)); // Another peer owns this seat.
    CHECK_FALSE(driver.clearPendingInput(3)); // Unoccupied.
    CHECK_FALSE(driver.clearPendingInput(4)); // Outside the roster.
    CHECK(driver.failure() == NativeRollbackDriver::Failure::None);
    REQUIRE(driver.clearPendingInput(0));
    REQUIRE(driver.clearPendingInput(0));
    CHECK(InputPacket::encode(driver.redundantInputs()) == before);
    CHECK(driver.sampleTick() == 10);

    for (u64 tick = 1; tick <= 11; ++tick) {
        fixture->remote(2, tick);
    }
    REQUIRE(driver.advance(false).status == Status::Held);
    CHECK(driver.sampleTick() == 10); // Confirmation alone does not admit captures.
    const auto ninth = driver.advance();
    REQUIRE(ninth.status == Status::Advanced);
    REQUIRE(ninth.frames.size() == 1);
    CHECK(ninth.frames.front().input.commands[0].pressed(CommandPress::Attack));
    CHECK(ninth.frames.front().input.commands[0].held(CommandHeld::Attack));
    fixture->progress(10);
    const auto tenth = driver.advance(); // Admit the retained capture without a second poll.
    REQUIRE(tenth.status == Status::Advanced);
    REQUIRE(tenth.frames.size() == 1);
    const auto& neutral = tenth.frames.front().input.commands[0];
    CHECK(neutral.tick == 10);
    CHECK(neutral.epoch == kEpoch);
    CHECK(neutral.grant == fixture->match.grants[0]);
    CHECK(neutral.direction == Vec2{0});
    CHECK(neutral.magnitude == 0);
    CHECK_FALSE(neutral.aimDirection);
    CHECK(neutral.heldButtons == 0);
    CHECK(neutral.pressedButtons == 0);
    CHECK(neutral.hintTopic == 0);
    CHECK(neutral.hintRevision == 0);
    const auto& other = tenth.frames.front().input.commands[2];
    CHECK(other.seat == 2);
    CHECK(other.pressed(CommandPress::Attack));
    CHECK(other.held(CommandHeld::Attack));
    CHECK(other.pressed(CommandPress::UsePotion));
    const auto committed = InputPacket::encode(driver.redundantInputs());
    REQUIRE(driver.clearPendingInput(2));
    CHECK(InputPacket::encode(driver.redundantInputs()) == committed);
    fixture->progress(11);
    REQUIRE(driver.sample({}));
    const auto next = driver.advance();
    REQUIRE(next.status == Status::Advanced);
    REQUIRE(next.frames.size() == 1);
    CHECK(next.frames.front().input.commands[0].pressedButtons == 0);
    CHECK(next.frames.front().input.commands[2].pressedButtons == 0);
    CHECK(physicalAudio == 0);
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
}

TEST_CASE("native rollback replaces an earlier terminal without fabricating post-terminal ticks",
          "[native-rollback-driver][native-rollback-terminal][assets]") {
    const bool audio = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 4}, true, "G1", audio);
    auto& scene = fixture->scene;
    auto& driver = fixture->driver;
    usize physicalAudio = 0;
    const SoundPlayer::Capture watch(&fixture->sounds, [&](const auto&) { ++physicalAudio; });
    if (audio) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& owner = const_cast<PlayerRuntime&>(scene.participants()[0]);
        owner.actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0,
                                                           20);
        REQUIRE(scene.beginRecordedAudio(kEpoch));
    }
    // Isolate the real answerTowerPrompts boundary. Only its real input handler
    // mutates departed; other players remain standing so Wait can remove a Quit.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& dead = const_cast<PlayerRuntime&>(scene.participants()[1]);
    dead.life = PlayerLife::InTower;
    dead.towerPrompt = true;
    dead.actor.save().progress().health = 1;
    PlayScene::LocalSnapshot initial;
    PlayScene::LocalSnapshot expectedQuit;
    PlayScene::LocalSnapshot expectedWait;
    REQUIRE(scene.saveLocal(initial));
    SessionInputs::Frame quit;
    SessionInputs::Frame wait;
    quit[1].menu.back = true;
    wait[1].menu.select = true;
    for (u64 tick = 1; tick <= 3; ++tick) {
        scene.recordStep(kEpoch, tick, kStep, tick == 3 ? quit : SessionInputs::Frame{});
    }
    REQUIRE(scene.participants()[1].departed);
    REQUIRE(scene.saveLocal(expectedQuit));
    const auto quitDigest = scene.nativeAgreementDigest();
    REQUIRE(quitDigest);
    REQUIRE(scene.restoreLocal(initial));
    for (u64 tick = 1; tick <= 6; ++tick) {
        scene.recordStep(kEpoch, tick, kStep,
                         tick == 2   ? wait
                         : tick == 3 ? quit
                                     : SessionInputs::Frame{});
    }
    REQUIRE_FALSE(scene.participants()[1].departed);
    REQUIRE(scene.saveLocal(expectedWait));
    const auto waitDigest = scene.nativeAgreementDigest();
    REQUIRE(waitDigest);
    REQUIRE(scene.restoreLocal(initial));
    REQUIRE(driver.bind(scene, fixture->match, 1,
                        audio ? NativeRollbackDriver::AudioMode::Required
                              : NativeRollbackDriver::AudioMode::Disabled));
    fixture->predict(6);
    fixture->remote(2, 3, quit);
    REQUIRE(driver.mismatchTick() == 3);
    const auto corrected = driver.advance(false);
    CHECK(corrected.status == Status::Held);
    CHECK_FALSE(corrected.terminalDigest);
    REQUIRE(corrected.frames.size() == 1);
    CHECK(corrected.frames.front().tick == 3);
    CHECK(corrected.frames.front().replay);
    REQUIRE(corrected.audio.has_value() == audio);
    if (audio) {
        REQUIRE(corrected.frames.front().audio);
        CHECK(corrected.frames.front().audio->loops.empty());
        REQUIRE(std::ranges::any_of(corrected.audio->actions, [](const auto& action) {
            return action.kind == SceneSoundEvents::Ledger::ActionKind::StopLoop;
        }));
        CHECK(physicalAudio == 0);
    }
    REQUIRE(driver.tentativeTerminal());
    CHECK(driver.tentativeTerminal()->tick == 3);
    CHECK(driver.tentativeTerminal()->outcome == PlayOutcome::Leave);
    CHECK(driver.nextTick() == 4);
    CHECK(driver.sampleTick() == 7);
    CHECK(driver.confirmedTick() == 0);
    CHECK(scene.nativeAgreementDigest() == quitDigest);
    CHECK(scene.matchesLocalControl(expectedQuit));
    for (usize poll = 0; poll < 8; ++poll) {
        SessionInputs::Frame local;
        local[0].attack = local[0].attackPressed = poll == 3;
        local[0].usePotion = poll == 4;
        REQUIRE(driver.sample(local));
        CHECK(driver.advance().status == Status::Held);
        CHECK(driver.nextTick() == 4);
        CHECK(driver.sampleTick() == 7);
    }
    SECTION("all four seats confirm the real earlier boundary once") {
        for (u8 peer = 2; peer <= 4; ++peer) {
            for (u64 tick = 1; tick <= 3; ++tick) {
                if ((peer == 2 && tick == 3) || (peer == 4 && tick == 3)) {
                    continue;
                }
                fixture->remote(peer, tick);
            }
        }
        CHECK(driver.advance(false).status == Status::Held);
        CHECK(driver.confirmedTick() == 2);
        fixture->remote(4, 3);
        const auto confirmed = driver.advance(false);
        CHECK(confirmed.status == Status::Leave);
        CHECK(confirmed.frames.empty());
        REQUIRE(confirmed.terminalDigest);
        CHECK(confirmed.terminalDigest->tick == 3);
        CHECK(confirmed.terminalDigest->digest ==
              NativeRollbackDriver::agreementDigest(
                  *quitDigest, {driver.arrivalVotes(), driver.tentativeTerminal()}));
        CHECK(confirmed.digests.empty());
        REQUIRE(confirmed.audio.has_value() == audio);
        CHECK(driver.confirmedTick() == 3);
        CHECK(scene.matchesLocalControl(expectedQuit));
        const auto held = driver.advance();
        CHECK(held.status == Status::Held);
        CHECK_FALSE(held.terminalDigest);
        CHECK(driver.nextTick() == 4);
    }
    SECTION("an earlier Wait correction removes Quit and resumes retained local input") {
        fixture->remote(2, 2, wait);
        REQUIRE(driver.mismatchTick() == 2);
        const auto removed = driver.advance(false);
        CHECK_FALSE(removed.terminalDigest);
        REQUIRE(removed.frames.size() == 2);
        if (audio) {
            REQUIRE(removed.audio);
            REQUIRE(removed.frames.back().audio);
            CHECK_FALSE(removed.frames.back().audio->loops.empty());
            REQUIRE(std::ranges::any_of(removed.audio->actions, [](const auto& action) {
                return action.kind == SceneSoundEvents::Ledger::ActionKind::StartLoop;
            }));
        }
        CHECK(removed.frames.front().tick == 2);
        CHECK(removed.frames.back().tick == 3);
        CHECK_FALSE(driver.tentativeTerminal());
        CHECK(driver.nextTick() == 4);
        CHECK(driver.sampleTick() == 7);
        for (u64 tick = 4; tick <= 6; ++tick) {
            CHECK_FALSE(driver.sampleReady());
            REQUIRE(driver.advance().status == Status::Advanced);
        }
        CHECK(driver.sampleReady());
        CHECK(scene.nativeAgreementDigest() == waitDigest);
        CHECK(scene.matchesLocalControl(expectedWait));
        CHECK_FALSE(scene.participants()[1].departed);
        CHECK_FALSE(scene.participants()[1].towerPrompt);
        for (u8 peer = 2; peer <= 4; ++peer) {
            for (u64 tick = 1; tick <= 6; ++tick) {
                if (peer == 2 && (tick == 2 || tick == 3)) {
                    continue;
                }
                fixture->remote(peer, tick);
            }
        }
        const auto confirmedWait = driver.advance(false);
        CHECK(confirmedWait.status == Status::Held);
        CHECK_FALSE(confirmedWait.terminalDigest);
        CHECK(driver.confirmedTick() == 6);
        CHECK_FALSE(driver.tentativeTerminal());
        for (u64 tick = 7; tick <= 8; ++tick) {
            for (u8 peer = 2; peer <= 4; ++peer) {
                fixture->remote(peer, tick);
            }
            fixture->progress(tick);
            REQUIRE(driver.sample({}));
            const auto resumed = driver.advance();
            REQUIRE(resumed.status == Status::Advanced);
            REQUIRE(resumed.frames.size() == 1);
            const auto& input = resumed.frames.front().input.commands[0];
            CHECK(input.pressed(CommandPress::Attack) == (tick == 7));
            CHECK(input.held(CommandHeld::Attack) == (tick == 7));
            CHECK(input.pressed(CommandPress::UsePotion) == (tick == 7));
        }
    }
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
    CHECK(physicalAudio == 0);
}

TEST_CASE("terminal agreement hashes the exact confirmed final frame on and off cadence",
          "[native-rollback-driver][native-rollback-terminal][native-rollback-digest][assets]") {
    const u64 terminalTick = GENERATE(u64{3}, u64{30});
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 4});
    auto& scene = fixture->scene;
    auto& driver = fixture->driver;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& dead = const_cast<PlayerRuntime&>(scene.participants()[1]);
    dead.life = PlayerLife::InTower;
    dead.towerPrompt = true;
    dead.actor.save().progress().health = 1;
    REQUIRE(driver.bind(scene, fixture->match, 1));
    for (u64 tick = 1; tick <= terminalTick; ++tick) {
        SessionInputs::Frame remote;
        remote[1].menu.back = tick == terminalTick;
        fixture->remote(2, tick, remote);
        fixture->remote(3, tick);
        if (tick != terminalTick) {
            fixture->remote(4, tick);
        }
        fixture->progress(tick);
        REQUIRE(driver.sample({}));
        const auto result = driver.advance();
        REQUIRE(result.status != Status::Failed);
        CHECK_FALSE(result.terminalDigest);
        CHECK(result.digests.empty());
    }
    REQUIRE(driver.tentativeTerminal());
    REQUIRE(driver.tentativeTerminal()->tick == terminalTick);
    REQUIRE(driver.confirmedTick() == terminalTick - 1);
    PlayScene::LocalSnapshot final;
    REQUIRE(scene.saveLocal(final));
    const auto savedHash = scene.nativeLocalDigest(final);
    REQUIRE(savedHash);
    const auto expected = NativeRollbackDriver::agreementDigest(
        *savedHash, {driver.arrivalVotes(), driver.tentativeTerminal()});
    REQUIRE(fixture->device.beginFrame());
    scene.render(fixture->device, glm::ortho(0.0f, 640.0f, 448.0f, 0.0f, -1.0f, 1.0f), 640, 448,
                 false, 0.5f);
    fixture->device.endFrame();
    CHECK(scene.nativeLocalDigest(final) == savedHash);
    fixture->remote(4, terminalTick);
    const auto confirmed = driver.advance(false);
    REQUIRE(confirmed.status == Status::Leave);
    REQUIRE(confirmed.terminalDigest);
    CHECK(confirmed.terminalDigest->tick == terminalTick);
    CHECK(confirmed.terminalDigest->digest == expected);
    REQUIRE(confirmed.digests.size() == (terminalTick % 30 == 0 ? 1 : 0));
    if (!confirmed.digests.empty()) {
        CHECK(confirmed.digests.front() == *confirmed.terminalDigest);
    }
    for (usize poll = 0; poll < 3; ++poll) {
        const auto held = driver.advance(false);
        REQUIRE(held.status == Status::Held);
        CHECK_FALSE(held.terminalDigest);
        CHECK(held.digests.empty());
    }
}

TEST_CASE("native rollback requires a matching audio boundary and fails closed if it is detached",
          "[native-rollback-driver][native-rollback-audio][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, true, "G1", true);
    auto& scene = fixture->scene;
    auto& driver = fixture->driver;
    using Mode = NativeRollbackDriver::AudioMode;
    CHECK_FALSE(driver.bind(scene, fixture->match, 1, Mode::Required));
    PlayScene::LocalSnapshot offline;
    REQUIRE(scene.saveLocal(offline));
    REQUIRE(scene.beginRecordedAudio(kEpoch));
    CHECK_FALSE(scene.canRestoreLocal(offline));
    CHECK_FALSE(driver.bind(scene, fixture->match, 1));
    auto wrong = fixture->match;
    wrong.epoch += 1;
    CHECK_FALSE(driver.bind(scene, wrong, 1, Mode::Required));
    REQUIRE(driver.bind(scene, fixture->match, 1, Mode::Required));
    usize physicalAudio = 0;
    const SoundPlayer::Capture watch(&fixture->sounds, [&](const auto&) { ++physicalAudio; });
    REQUIRE(driver.sample({}));
    const auto first = driver.advance();
    REQUIRE(first.status == Status::Advanced);
    REQUIRE(first.audio);
    REQUIRE(first.frames.size() == 1);
    REQUIRE(first.frames.front().audio);
    CHECK(physicalAudio == 0);
    scene.dispatchRecordedAudio(*first.audio);

    // This is intentionally a lifecycle violation, not a supported transition.
    // It invalidates the audio snapshot binding before the next native step.
    scene.endRecordedAudio();
    const auto afterDetach = physicalAudio;
    const auto failed = driver.advance();
    CHECK(failed.status == Status::Failed);
    CHECK(driver.failure() == NativeRollbackDriver::Failure::Binding);
    CHECK(failed.frames.empty());
    CHECK(failed.actions.empty());
    CHECK(failed.unretractable.empty());
    CHECK(failed.digests.empty());
    CHECK_FALSE(failed.audio);
    CHECK(physicalAudio == afterDetach);
    CHECK(driver.nextTick() == 2);
    CHECK_FALSE(driver.sample({}));
    auto newer = fixture->match;
    ++newer.epoch;
    CHECK_FALSE(driver.bind(scene, newer, 1));
    driver.clear();
    CHECK_FALSE(driver.bind(scene, fixture->match, 1));
    CHECK_FALSE(driver.bind(scene, newer, 1));
    // Reattaching the recorder is not a scene reload and cannot repair a
    // potentially partial native restore from this same loaded generation.
    REQUIRE(scene.beginRecordedAudio(newer.epoch));
    CHECK_FALSE(driver.bind(scene, newer, 1, Mode::Required));
    const auto failedGeneration = scene.loadGeneration();
    fixture->open();
    REQUIRE(scene.loadGeneration() != failedGeneration);
    // Closing the scene preserves the sound ledger's last binding epoch. The
    // repaired load must use a fresh epoch, not the attempted reattachment's.
    auto recovered = newer;
    ++recovered.epoch;
    REQUIRE(scene.beginRecordedAudio(recovered.epoch));
    REQUIRE(driver.bind(scene, recovered, 1, Mode::Required));
    REQUIRE(driver.sample({}));
    const auto resumed = driver.advance();
    REQUIRE(resumed.status == Status::Advanced);
    REQUIRE(resumed.audio);
}

TEST_CASE("a drop after a directly delivered frame settled here holds what that frame ran on",
          "[native-rollback][drop][assets]") {
    const bool early = GENERATE(false, true);
    CAPTURE(early);
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 0});
    REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 3));
    for (u64 tick = 1; tick <= 3; ++tick) {
        REQUIRE(fixture->driver.sample({}));
        fixture->remote(1, tick);
        fixture->remote(2, tick);
        REQUIRE(fixture->driver.advance().status == Status::Advanced);
    }
    REQUIRE(fixture->driver.confirmedTick() == 3);
    // Peer two's walk for tick four reaches this guest over its direct link, or never does.
    SessionInputs::Frame walking;
    walking[1].move = {{1, 0}, 1};
    REQUIRE(fixture->driver.sample({}));
    fixture->remote(1, 4);
    if (early) {
        fixture->remote(2, 4, walking);
    }
    REQUIRE(fixture->driver.advance().status == Status::Advanced);
    REQUIRE(fixture->driver.confirmedTick() == (early ? 4 : 3));
    // The host had peer two's input only through tick three, so the shared drop starts at four.
    REQUIRE(fixture->driver.suspend(2, 4));
    REQUIRE(fixture->driver.sample({}));
    fixture->remote(1, 5);
    const auto next = fixture->driver.advance();
    CHECK(next.status == Status::Advanced);
    CHECK(fixture->driver.failure() == NativeRollbackDriver::Failure::None);
    CHECK(fixture->driver.confirmedTick() == 5);
}

TEST_CASE("native rollback restores accumulated all-seat arrival skip votes with the scene",
          "[native-rollback-driver][native-rollback-votes][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 4}, false, "G5");
    auto& scene = fixture->scene;
    REQUIRE(scene.startCamera().mode() == StartCamera::Mode::Legacy);
    for (u64 tick = 1; tick <= 100 && scene.startCamera().ticksLeft() > StartCamera::kSkipBelow;
         ++tick) {
        REQUIRE(scene.recordStep(1, tick, kStep, {}).outcome == PlayOutcome::Running);
    }
    REQUIRE(scene.startCamera().phase() == StartCamera::Phase::Hold);
    REQUIRE(scene.startCamera().ticksLeft() <= StartCamera::kSkipBelow);
    auto& driver = fixture->driver;
    REQUIRE(driver.bind(scene, fixture->match, 1));
    SessionInputs::Frame local;
    local[0].menu.select = true;
    REQUIRE(driver.sample(local));
    REQUIRE(driver.advance().status == Status::Advanced);
    REQUIRE(driver.sample({}));
    REQUIRE(driver.advance().status == Status::Advanced);
    CHECK(driver.arrivalVotes() == std::array<bool, 4>{true, false, false, false});
    CHECK(scene.startCamera().phase() == StartCamera::Phase::Hold);
    SessionInputs::Frame second;
    SessionInputs::Frame third;
    SessionInputs::Frame fourth;
    second[1].menu.select = third[2].menu.select = fourth[3].menu.select = true;
    fixture->remote(2, 1, second);
    fixture->remote(3, 2, third);
    const auto partial = driver.advance(false);
    REQUIRE(partial.frames.size() == 2);
    CHECK(driver.arrivalVotes() == std::array<bool, 4>{true, true, true, false});
    CHECK(scene.startCamera().phase() == StartCamera::Phase::Hold);
    fixture->remote(4, 2, fourth);
    const auto unanimous = driver.advance(false);
    REQUIRE(unanimous.frames.size() == 1);
    CHECK(unanimous.frames.front().tick == 2);
    CHECK(driver.arrivalVotes() == std::array<bool, 4>{true, true, true, true});
    CHECK(scene.startCamera().phase() != StartCamera::Phase::Hold);
    CHECK(driver.sampleTick() == 3);
    CHECK(driver.confirmedTick() == 0);
    CHECK(fixture->physicalRumbles.pulses.empty());
    CHECK(fixture->persistentUnlocks.masks.empty());
}

TEST_CASE("a dropped peer's seat never holds up the arrival camera's skip",
          "[native-rollback-driver][native-rollback-votes][drop][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 0, 0}, false, "G5");
    auto& scene = fixture->scene;
    REQUIRE(scene.startCamera().mode() == StartCamera::Mode::Legacy);
    for (u64 tick = 1; tick <= 100 && scene.startCamera().ticksLeft() > StartCamera::kSkipBelow;
         ++tick) {
        REQUIRE(scene.recordStep(1, tick, kStep, {}).outcome == PlayOutcome::Running);
    }
    REQUIRE(scene.startCamera().phase() == StartCamera::Phase::Hold);
    auto& driver = fixture->driver;
    REQUIRE(driver.bind(scene, fixture->match, 1));
    REQUIRE(driver.suspend(2, 1));
    SessionInputs::Frame local;
    local[0].menu.select = true;
    REQUIRE(driver.sample(local));
    REQUIRE(driver.advance().status == Status::Advanced);
    CHECK(driver.arrivalVotes() == std::array<bool, 4>{true, false, false, false});
    CHECK(scene.startCamera().phase() != StartCamera::Phase::Hold);
    REQUIRE(scene.runtime(1) != nullptr);
    CHECK(scene.runtime(1)->life == PlayerLife::InTower);
}

TEST_CASE("active native rollback measures warm eight frame corrections with recorded audio",
          "[.][native-rollback-benchmark][assets]") {
    const usize seats = GENERATE(usize{2}, usize{4});
    const auto level = GENERATE(std::string_view{"G2"}, std::string_view{"F2"});
    CAPTURE(seats, level);
    MatchOwners owners{1, 2, 0, 0};
    if (seats == 4) {
        owners = {1, 2, 3, 4};
    }
    auto fixture = std::make_unique<Fixture>(owners, false, level, true);
    auto& scene = fixture->scene;
    auto& driver = fixture->driver;
    REQUIRE(scene.beginRecordedAudio(kEpoch));
    REQUIRE(driver.bind(scene, fixture->match, 1, NativeRollbackDriver::AudioMode::Required));

    const auto inputAt = [&](u64 tick) {
        SessionInputs::Frame inputs;
        constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
        for (u8 seat = 0; seat < seats; ++seat) {
            if (tick <= 360) {
                continue;
            }
            auto& input = inputs[seat];
            const u64 elapsed = tick - 361;
            const u64 phase = (elapsed + static_cast<u64>(seat * 30)) % 240;
            const auto direction = kDirections[(elapsed / 120 + seat) % 4];
            input.move = {direction, 1};
            input.aimDirection = Vec3{direction.x, 0, -direction.y};
            input.attack = phase < 90;
            input.attackPressed = phase == 0;
            input.strongAttack = phase >= 120 && phase < 150;
            input.turbo = phase >= 180 && phase < 190;
            input.defendPressed = phase == 180;
            input.usePotion = elapsed % 600 == 300;
            // Every delayed batch begins with a changed remote direction. This
            // forces the full eight-frame correction instead of timing a no-op.
            if (seat != 0 && tick >= 649) {
                input.aimDirection = ((tick - 649) / 8) % 2 == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
            }
            input = SessionInputs::playInput(
                SessionInputs::command(input, kEpoch, tick, fixture->match.grants[seat], seat));
        }
        return inputs;
    };
    usize physicalAudio = 0;
    const SoundPlayer::Capture capture(&fixture->sounds, [&](const auto&) { ++physicalAudio; });
    const auto publish = [&](const NativeRollbackDriver::Result& result, usize elapsedTicks) {
        REQUIRE(result.status != Status::Failed);
        REQUIRE(result.audio);
        for (const auto& action : result.actions) {
            REQUIRE(scene.dispatchOutput(action));
        }
        scene.dispatchRecordedAudio(*result.audio);
        scene.serviceRecordedAudio(static_cast<f32>(static_cast<f64>(elapsedTicks) * kStep));
        // Consume the mix without opening a physical audio device. Presentation
        // service and mixing are outside the measured replay call.
        std::array<f32, 1600> samples{};
        for (usize tick = 0; tick < elapsedTicks; ++tick) {
            fixture->mixer.mix(samples);
            fixture->sounds.update();
        }
    };
    for (u64 tick = 1; tick <= 648; ++tick) {
        const auto frame = inputAt(tick);
        for (u8 peer = 2; peer <= seats; ++peer) {
            fixture->remote(peer, tick, frame);
        }
        fixture->progress(tick);
        SessionInputs::Frame local;
        local[0] = frame[0];
        REQUIRE(driver.sample(local));
        const auto result = driver.advance();
        REQUIRE(result.status == Status::Advanced);
        publish(result, 1);
    }
    REQUIRE(driver.confirmedTick() == 648);
    REQUIRE_FALSE(scene.spawning());
    REQUIRE_FALSE(scene.startCamera().active());
    if (level == "G2") {
        REQUIRE(scene.enemies().targets().size() >= 10);
        REQUIRE(scene.generators().count() >= 50);
    } else {
        const auto boss = scene.bossView();
        REQUIRE(boss);
        REQUIRE(boss->awake);
        REQUIRE(boss->alive);
    }
    constexpr usize kSamples = 64;
    std::array<f64, kSamples> milliseconds{};
    std::array<std::vector<f64>, 2> digestBuckets;
    for (auto& bucket : digestBuckets) {
        bucket.reserve(kSamples);
    }
    usize mostEnemies = scene.enemies().targets().size();
    usize finiteCues = 0;
    for (usize sample = 0; sample < kSamples; ++sample) {
        const u64 first = driver.nextTick();
        PlayScene::LocalSnapshot base;
        PlayScene::LocalSnapshot expectedControl;
        REQUIRE(scene.saveLocal(base));
        std::array<PlayScene::RecordedStep, 8> expected;
        const auto physicalBefore = physicalAudio;
        for (usize frame = 0; frame < 8; ++frame) {
            expected[frame] =
                scene.recordStep(kEpoch, first + frame, kStep, inputAt(first + frame));
            REQUIRE(expected[frame].outcome == PlayOutcome::Running);
            REQUIRE(expected[frame].audio);
            finiteCues += expected[frame].audio->finite.events.size();
        }
        REQUIRE(scene.saveLocal(expectedControl));
        const auto expectedDigest = scene.nativeAgreementDigest();
        REQUIRE(expectedDigest);
        std::vector<PlayerSimulation> players;
        for (const auto& player : scene.participants()) {
            players.push_back(player.simulation());
        }
        LevelOpponents::LocalSnapshot expectedOpponents;
        LevelOpponents::LocalSnapshot actualOpponents;
        REQUIRE(scene.opponents().saveLocal(expectedOpponents));
        LevelFixtures::LocalSnapshot expectedFixtures;
        LevelFixtures::LocalSnapshot actualFixtures;
        scene.fixtures().saveLocal(expectedFixtures);
        LevelWorld::LocalSnapshot expectedWorld;
        LevelWorld::LocalSnapshot actualWorld;
        REQUIRE(scene.world() != nullptr);
        scene.world()->saveLocal(expectedWorld);
        REQUIRE(scene.restoreLocal(base));
        CHECK(physicalAudio == physicalBefore);
        for (usize frame = 0; frame < 8; ++frame) {
            fixture->progress(first + frame);
            SessionInputs::Frame local;
            local[0] = inputAt(first + frame)[0];
            REQUIRE(driver.sample(local));
            const auto predicted = driver.advance();
            REQUIRE(predicted.status == Status::Advanced);
            REQUIRE(predicted.frames.size() == 1);
            REQUIRE(predicted.frames.front().input.predictedSeats != 0);
            publish(predicted, 1);
        }
        for (u8 peer = 2; peer <= seats; ++peer) {
            for (u64 tick = first; tick < first + 8; ++tick) {
                fixture->remote(peer, tick, inputAt(tick));
            }
        }
        REQUIRE(driver.mismatchTick() == first);
        const auto callsBefore = physicalAudio;
        const auto start = std::chrono::steady_clock::now();
        const auto corrected = driver.advance(false);
        milliseconds[sample] =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        CHECK(physicalAudio == callsBefore);
        REQUIRE(corrected.status != Status::Failed);
        REQUIRE(corrected.frames.size() == 8);
        digestBuckets[corrected.digests.empty() ? 0 : 1].push_back(milliseconds[sample]);
        CHECK(driver.confirmedTick() == first + 7);
        CHECK(scene.matchesLocalControl(expectedControl));
        CHECK(scene.nativeAgreementDigest() == expectedDigest);
        for (usize frame = 0; frame < 8; ++frame) {
            CHECK(corrected.frames[frame].replay);
            CHECK(corrected.frames[frame].journal == expected[frame].journal);
            CHECK(corrected.frames[frame].audio == expected[frame].audio);
        }
        for (usize seat = 0; seat < players.size(); ++seat) {
            CHECK(scene.participants()[seat].simulation() == players[seat]);
        }
        REQUIRE(scene.opponents().saveLocal(actualOpponents));
        CHECK(scene.opponents().equalLocal(actualOpponents, expectedOpponents));
        scene.fixtures().saveLocal(actualFixtures);
        CHECK(scene.fixtures().equalLocal(actualFixtures, expectedFixtures));
        scene.world()->saveLocal(actualWorld);
        CHECK(scene.world()->equalLocal(actualWorld, expectedWorld));
        publish(corrected, 0);
        mostEnemies = std::max(mostEnemies, scene.enemies().targets().size());
    }
    std::ranges::sort(milliseconds);
    // The reference branch warms artwork and sound-resource lookups. This is a
    // steady-state correction budget, not a first-use or process-start latency.
    std::cout << "native warm full correction level=" << level << " seats=" << seats
              << " samples=" << kSamples << " replay-frames=8 audio=recorded"
              << " max-enemies=" << mostEnemies << " generators=" << scene.generators().count()
              << " finite-cues=" << finiteCues << " p50-ms=" << milliseconds[kSamples / 2]
              << " p95-ms=" << milliseconds[kSamples * 95 / 100]
              << " max-ms=" << milliseconds.back() << '\n';
    CHECK(digestBuckets[0].size() + digestBuckets[1].size() == kSamples);
    for (usize index = 0; index < digestBuckets.size(); ++index) {
        auto& bucket = digestBuckets[index];
        REQUIRE_FALSE(bucket.empty());
        std::ranges::sort(bucket);
        // The same measured advance includes publication of any newly confirmed
        // digest; bucket after timing so classification adds no measured work.
        std::cout << "native correction digest bucket level=" << level << " seats=" << seats
                  << " confirmed-digest=" << (index == 0 ? "absent" : "present")
                  << " samples=" << bucket.size() << " replay-frames=8 audio=recorded"
                  << " p50-ms=" << bucket[bucket.size() / 2]
                  << " p95-ms=" << bucket[bucket.size() * 95 / 100] << " max-ms=" << bucket.back()
                  << '\n';
    }
    CHECK(driver.statistics().corrections == kSamples);
    CHECK(driver.statistics().maxReplay == 8);
    CHECK(physicalAudio > 0);
    CHECK(finiteCues > 0);
    driver.clear();
}

TEST_CASE("a guest corrects a directly delivered frame that the host's drop holds still",
          "[native-rollback][drop][direct-link][assets]") {
    auto fixture = std::make_unique<Fixture>(MatchOwners{1, 2, 3, 0});
    REQUIRE(fixture->driver.bind(fixture->scene, fixture->match, 3));
    SessionInputs::Frame walking;
    walking[1].move = {{1, 0}, 1};
    for (u64 tick = 1; tick <= 4; ++tick) {
        // The host acknowledges peer two through tick three; tick four came directly.
        REQUIRE(fixture->driver.observeHostReceipt(2, std::min<u64>(tick, 3)));
        REQUIRE(fixture->driver.sample({}));
        fixture->remote(1, tick);
        fixture->remote(2, tick, tick == 4 ? walking : SessionInputs::Frame{});
        REQUIRE(fixture->driver.advance().status == Status::Advanced);
    }
    CHECK(fixture->driver.confirmedTick() == 3);
    // The host's drop starts where its own receipt ends.
    REQUIRE(fixture->driver.suspend(2, 4));
    REQUIRE(fixture->driver.sample({}));
    fixture->remote(1, 5);
    const auto next = fixture->driver.advance();
    CAPTURE(static_cast<unsigned>(next.status), static_cast<unsigned>(fixture->driver.failure()));
    REQUIRE(next.status == Status::Advanced);
    REQUIRE(next.frames.size() == 2); // tick four again, then tick five
    for (const auto& frame : next.frames) {
        CHECK(frame.input.commands[1].magnitude == 0.0f);
    }
    CHECK(fixture->driver.confirmedTick() == 5);
}
