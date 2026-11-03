#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterProfile.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/netplay/RollbackExchange.h"
#include "game/online/NativeRollbackDriver.h"
#include "game/rollback/RollbackInputs.h"
#include "game/rollback/SessionInputs.h"
#include "game/rollback/SimulationObserver.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Ledger = PlayScene::OutputLedger;
using Admission = RollbackInputs::Admission;
constexpr u64 kEpoch = 83;
constexpr f64 kStep = 1.0 / 60;
// Virtual time only: one unit is 1/3 microsecond. Two peers have one 40 ms
// hop each way. Four peers have 20 ms hops, so guest-host-guest is 80 ms RTT
// while host-guest is 40 ms. These are not measured socket or human latencies.
constexpr u64 kWallTick = 50'000;
constexpr PacketTransport::Connection kHostLink = 80;
constexpr u64 kOutageTicks = 30;

struct Settings {
    usize peers = 2;
    bool secret = false;
    bool audio = false;
    bool firstInputOutage = false;
    u64 ticks = 240;
    u8 delay = 1;
    RollbackInputs::Owners owners{};
    RollbackInputs::Grants grants{};
    Settings(usize count, bool challenge, bool recordedAudio = false)
        : peers(count), secret(challenge), audio(recordedAudio) {
        if (secret) {
            // Finish before the one-second secret exit barrier. Persistence
            // must be confirmed here; loading another scene is not this test.
            ticks = 40;
            delay = 0;
        }
        for (usize seat = 0; seat < peers; ++seat) {
            owners[seat] = static_cast<u8>(seat + 1);
            grants[seat] = static_cast<u32>(100 + seat);
        }
    }
};

InputCommand actual(const Settings& settings, u8 seat, u64 tick) {
    PlayInput input;
    if (!settings.secret && tick > settings.delay) {
        constexpr std::array<Vec2, 4> kDirections{Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0}, Vec2{0, -1}};
        if ((tick >= 5 && tick < 80) || (tick >= 130 && tick < 190)) {
            input.move = {kDirections[((tick - 1) / 40) % kDirections.size()], 0.5f};
        }
        input.aimDirection = Vec3{1, 0, 0};
        input.attack = (tick >= 20 && tick < 75) || (tick >= 145 && tick < 210);
        input.attackPressed = tick == 20 || tick == 145;
        // Start a potion from idle, not during the previous throw's recovery;
        // the release must also finish before the observation at tick 240.
        input.usePotion = tick == 4 + seat * 2 || tick == 112 + seat * 2;
    }
    // The driver steadies what it samples; the expected command must match it bit for bit.
    return SessionInputs::command(input, kEpoch, tick, settings.grants[seat], seat).steadied();
}

PlayScene::Inputs controls(const Settings& settings, const RollbackInputs::Commands& commands) {
    PlayScene::Inputs result;
    for (usize seat = 0; seat < settings.peers; ++seat) {
        result[seat] = SessionInputs::playInput(commands[seat]);
    }
    return result;
}

struct NativeScene {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog levels;
    StringTable strings;
    LevelWorld world;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    usize physicalAudio = 0;
    std::unique_ptr<SoundPlayer::Capture> soundWatch;
    test::RecordingHaptics haptics;
    test::RecordingUnlocks unlocks;
    PlayScene scene;
    SimulationObserver observer{scene};

    explicit NativeScene(const Settings& settings) {
        const std::string name = settings.secret ? "S4" : "G1";
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", name));
        REQUIRE(levels.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto ref = levels.byName(name);
        REQUIRE(ref);
        REQUIRE(world.load(device, root, *ref));
        config.timing.tickRate = 60;
        GameContext context;
        context.config = &config;
        context.levels = &levels;
        context.strings = &strings;
        context.unpackedRoot = root;
        context.sounds = settings.audio ? &sounds : nullptr;
        std::vector<PartyMember> party;
        for (usize seat = 0; seat < settings.peers; ++seat) {
            CharacterSave save;
            save.name = "REWIND";
            save.character = static_cast<s32>(seat);
            save.color = static_cast<s32>(seat);
            save.progress().health = 5000;
            save.progress().inventory.addPotions(static_cast<s32>(seat + 1), 5);
            if (!settings.secret) {
                save.progress().inventory.addPowerup(powerup::kWeapon, damage::kHammer, 80, -1);
                save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
            }
            REQUIRE(CharacterProfile::capture(save));
            party.push_back({static_cast<s32>(seat), save});
        }
        PlayOptions options;
        options.welcome = false;
        options.position =
            settings.secret ? world.startPoint(0)->position : Vec3{18.8f, 0.2f, 0.7f};
        REQUIRE(scene.open(device, context, world, party, options,
                           {.haptics = &haptics, .unlocks = &unlocks}));
        const auto entering = [&] {
            return scene.spawning() || std::ranges::any_of(scene.participants(), [](const auto& p) {
                       return p.figure != nullptr && p.figure->animator().entering();
                   });
        };
        for (usize tick = 0; tick < 480 && entering(); ++tick) {
            REQUIRE(scene.update(kStep, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(entering());
        if (settings.secret) {
            primeSecretContact();
        }
        haptics.pulses.clear();
        unlocks.masks.clear();
        if (settings.audio) {
            REQUIRE(scene.beginRecordedAudio(kEpoch));
            soundWatch = std::make_unique<SoundPlayer::Capture>(
                &sounds, [&](const auto&) { ++physicalAudio; });
        }
        REQUIRE(observer.bind());
    }

    void primeSecretContact() {
        // Reach the final objective using the real pickup route, then restore
        // its pre-contact scene. Tick one of the network run earns the reward.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): native contact fixture
        auto& player = *const_cast<PlayerActor*>(scene.actor(0));
        PlayScene::LocalSnapshot before;
        bool won = false;
        for (usize index = 0; index < world.placedItems().size(); ++index) {
            const auto& item = world.placedItems().item(index);
            if (item.subtype != 1 || !item.visible || item.taken) {
                continue;
            }
            player.place(item.position);
            REQUIRE(scene.saveLocal(before));
            REQUIRE(scene.update(kStep, {}) == PlayOutcome::Running);
            if (scene.challenge().state() == SecretChallenge::State::Won) {
                won = true;
                REQUIRE(scene.restoreLocal(before));
                break;
            }
        }
        REQUIRE(won);
        REQUIRE(scene.challenge().state() == SecretChallenge::State::Collecting);
    }
};

struct PlayerObservation {
    Vec3 position;
    Vec3 facing;
    PlayerLife life;
    s32 kills;
    std::string save;
    bool operator==(const PlayerObservation&) const = default;
};
struct Observation {
    SimulationObservation diagnostic;
    std::optional<SimulationObservation::Digest> native;
    std::vector<PlayerObservation> players;
    std::vector<PlayerSimulation> playerValues;
    WorldCamera camera;
    SecretChallenge::State challenge{};
    TransitionScreen::Phase transition{};
};
Observation observe(NativeScene& source) {
    Observation result{source.observer.observe(),
                       source.scene.nativeAgreementDigest(),
                       {},
                       {},
                       source.scene.viewCamera(),
                       source.scene.challenge().state(),
                       source.scene.transition().phase()};
    REQUIRE(result.native);
    // The existing observers explicitly report incomplete coverage. Agreement
    // here is evidence for their fields, not a hash of every native owner.
    REQUIRE_FALSE(result.diagnostic.complete);
    REQUIRE_FALSE(result.diagnostic.unobserved.empty());
    for (const auto& player : source.scene.participants()) {
        result.players.push_back({player.actor.position(), player.actor.facing(), player.life,
                                  player.levelKills, player.actor.save().toJson()});
        result.playerValues.push_back(static_cast<const PlayerSimulation&>(player));
    }
    return result;
}
void compare(const Observation& got, const Observation& expected) {
    CHECK(got.native == expected.native);
    REQUIRE(got.diagnostic.subsystems.size() == expected.diagnostic.subsystems.size());
    usize available = 0;
    for (usize index = 0; index < expected.diagnostic.subsystems.size(); ++index) {
        const auto& want = expected.diagnostic.subsystems[index];
        CAPTURE(want.name, want.unavailable);
        CHECK(got.diagnostic.subsystems[index] == want);
        available += want.digest.has_value() ? 1 : 0;
    }
    REQUIRE(available >= 3);
    CHECK(got.players == expected.players);
    REQUIRE(got.playerValues.size() == expected.playerValues.size());
    for (usize seat = 0; seat < expected.playerValues.size(); ++seat) {
        CAPTURE(seat);
        // Compare the actual complete participant values, including hidden
        // controls and all character classes. Child figures/scene owners are
        // separate: this does not turn the partial scene observer into proof.
        CHECK(got.playerValues[seat] == expected.playerValues[seat]);
    }
    CHECK(got.camera.position == expected.camera.position);
    CHECK(got.camera.pitch == expected.camera.pitch);
    CHECK(got.camera.yaw == expected.camera.yaw);
    CHECK(got.camera.roll == expected.camera.roll);
    CHECK(got.challenge == expected.challenge);
    CHECK(got.transition == expected.transition);
}

struct Peer {
    NativeScene native;
    NativeRollbackDriver driver;
    RollbackExchange exchange;
    std::vector<Ledger::Journal> journals;
    std::vector<std::optional<SceneSoundEvents::Frame>> audioFrames;
    std::map<FrameEventIdentity, SceneSoundPayload> heard;
    std::set<FrameEventIdentity> unretractableAudio;
    std::map<FrameEventIdentity, PlayScene::OutputPayload> presented;
    std::map<FrameEventIdentity, PlayScene::OutputPayload> committed;
    std::set<FrameEventIdentity> unretractable;
    std::map<u64, u64> sampledAt;
    std::map<u64, u64> appliedAt;
    std::map<u64, SimulationObservation::Digest> digests;
    std::vector<f64> correctionMilliseconds, forwardMilliseconds;
    usize maxCorrectionFrames = 0;
    bool pendingUnlock = false;
    u8 localSeat = 0;

    Peer(const Settings& settings, u8 id)
        : native(settings), journals(settings.ticks + 1), audioFrames(settings.ticks + 1),
          localSeat(static_cast<u8>(id - 1)) {
        MatchContext context;
        context.epoch = kEpoch;
        context.scene = 1;
        context.tickRate = 60;
        context.inputLead = settings.delay;
        context.owners = settings.owners;
        context.grants = settings.grants;
        REQUIRE(driver.bind(native.scene, context, id,
                            settings.audio ? NativeRollbackDriver::AudioMode::Required
                                           : NativeRollbackDriver::AudioMode::Disabled));
    }
    void advance(const Settings& settings, u64 wall, bool forward) {
        const auto vibrations = native.haptics.pulses.size();
        const auto unlocks = native.unlocks.masks.size();
        const auto audio = native.physicalAudio;
        const auto voices = native.sounds.voiceCount();
        const auto started = std::chrono::steady_clock::now();
        const auto result = driver.advance(forward);
        const auto elapsed =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const auto replayed = static_cast<usize>(
            std::ranges::count_if(result.frames, &NativeRollbackDriver::ExecutedFrame::replay));
        // Actual measured wall duration of the whole driver call, including
        // restore, saves, corrected ticks and journal reconciliation. Scheduling,
        // dispatch and virtual link latency are deliberately outside this clock.
        if (replayed != 0) {
            correctionMilliseconds.push_back(elapsed);
            maxCorrectionFrames = std::max(maxCorrectionFrames, replayed);
        } else if (!result.frames.empty()) {
            forwardMilliseconds.push_back(elapsed);
        }
        CAPTURE(driver.nextTick(), driver.failure());
        REQUIRE(result.status != NativeRollbackDriver::Status::Failed);
        REQUIRE_FALSE(driver.tentativeTerminal());
        CHECK(native.haptics.pulses.size() == vibrations);
        CHECK(native.unlocks.masks.size() == unlocks);
        CHECK(native.physicalAudio == audio);
        CHECK(native.sounds.voiceCount() == voices);
        REQUIRE(result.audio.has_value() == settings.audio);
        for (const auto& frame : result.frames) {
            const auto tick = frame.tick;
            CHECK((frame.input.predictedSeats & (1U << localSeat)) == 0);
            const std::array used{frame.input.commands[localSeat]};
            const std::array sampled{actual(settings, localSeat, tick)};
            const auto usedBytes = InputPacket::encode(used);
            const auto sampledBytes = InputPacket::encode(sampled);
            REQUIRE(usedBytes);
            REQUIRE(sampledBytes);
            CHECK(usedBytes == sampledBytes);
            REQUIRE(tick < journals.size());
            journals[tick] = frame.journal;
            REQUIRE(frame.audio.has_value() == settings.audio);
            audioFrames[tick] = frame.audio;
            if (!frame.replay) {
                appliedAt.emplace(tick, wall);
            }
            for (const auto& event : frame.journal.events) {
                pendingUnlock |=
                    event.policy == Ledger::Policy::Confirmed && driver.confirmedTick() < tick;
            }
        }
        for (const auto& digest : result.digests) {
            CHECK(digest.tick <= driver.confirmedTick());
            REQUIRE(digests.emplace(digest.tick, digest.digest).second);
        }
        unretractable.insert(result.unretractable.begin(), result.unretractable.end());
        for (const auto& action : result.actions) {
            if (action.kind == Ledger::ActionKind::Present) {
                REQUIRE(presented.emplace(action.event.id, action.event.payload).second);
            } else if (action.kind == Ledger::ActionKind::Commit) {
                REQUIRE(committed.emplace(action.event.id, action.event.payload).second);
            } else {
                FAIL("This native output seam currently contains no loop payloads");
            }
            REQUIRE(native.scene.dispatchOutput(action));
        }
        if (result.audio) {
            REQUIRE(*result.audio);
            unretractableAudio.insert(result.audio->unretractable.begin(),
                                      result.audio->unretractable.end());
            for (const auto& action : result.audio->actions) {
                if (action.kind == SceneSoundEvents::Ledger::ActionKind::Present) {
                    REQUIRE(heard.emplace(action.event.id, action.event.payload).second);
                }
            }
            native.scene.dispatchRecordedAudio(*result.audio);
            if (forward) {
                native.scene.serviceRecordedAudio(static_cast<f32>(kStep));
            }
        }
    }
};

struct Packet {
    u64 sequence = 0;
    usize from = 0;
    usize to = 0;
    u64 sent = 0;
    u64 arrives = 0;
    std::vector<u8> bytes;
};

struct Wire final : PacketTransport {
    std::function<void(Connection, std::span<const u8>)> deliver;
    s32 pingMs = -1;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        REQUIRE(delivery == Delivery::Unreliable);
        REQUIRE(bytes.size() <= RollbackExchange::kMaxBytes);
        deliver(connection, bytes);
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return Statistics{pingMs};
    }
    void close(Connection /*connection*/) override {}
};

void run(const Settings& settings) {
    INFO("Virtual host-star links: two-peer 80 ms RTT; four-peer 40 ms host/guest and 80 ms "
         "guest/guest RTT, bounded jitter, and 2% dropped sends. Real native scenes, no sockets. "
         "Native observations are partial; recorded audio is an explicit fixture option.");
    std::vector<Ledger::Journal> expectedJournals(settings.ticks + 1);
    std::vector<std::optional<SceneSoundEvents::Frame>> expectedAudio(settings.ticks + 1);
    std::map<u64, SimulationObservation::Digest> expectedDigests;
    Observation expected;
    {
        NativeScene reference(settings);
        for (u64 tick = 1; tick <= settings.ticks; ++tick) {
            RollbackInputs::Commands commands;
            for (u8 seat = 0; seat < settings.peers; ++seat) {
                commands[seat] = actual(settings, seat, tick);
            }
            const auto audio = reference.physicalAudio;
            const auto result =
                reference.scene.recordStep(kEpoch, tick, kStep, controls(settings, commands));
            REQUIRE(result.outcome == PlayOutcome::Running);
            expectedJournals[tick] = result.journal;
            REQUIRE(result.audio.has_value() == settings.audio);
            expectedAudio[tick] = result.audio;
            CHECK(reference.physicalAudio == audio);
            if (result.audio) {
                const std::array frames{*result.audio};
                const auto accepted = reference.scene.reconcileRecordedAudio(tick, frames);
                REQUIRE(accepted);
                reference.scene.dispatchRecordedAudio(accepted);
                const auto confirmed = reference.scene.confirmRecordedAudio(tick);
                REQUIRE(confirmed);
                reference.scene.dispatchRecordedAudio(confirmed);
                reference.scene.serviceRecordedAudio(static_cast<f32>(kStep));
            }
            if (tick % NativeRollbackDriver::kDigestPeriod == 0) {
                const auto digest = reference.scene.nativeAgreementDigest();
                REQUIRE(digest);
                // This reference starts after arrival and never leaves its
                // stage, so its real coordinator control remains empty.
                expectedDigests.emplace(tick, NativeRollbackDriver::agreementDigest(*digest, {}));
            }
        }
        REQUIRE(reference.haptics.pulses.empty());
        REQUIRE(reference.unlocks.masks.empty());
        expected = observe(reference);
        if (!settings.secret) {
            CAPTURE(reference.scene.scroll().active(), reference.scene.switchCutscene().active(),
                    reference.scene.spawning());
            REQUIRE(std::ranges::any_of(reference.scene.participants(), [](const auto& player) {
                return player.actor.save().progress().inventory.potions.size() < 5;
            }));
        }
    }
    std::vector<std::unique_ptr<Peer>> peers;
    for (usize index = 0; index < settings.peers; ++index) {
        peers.push_back(std::make_unique<Peer>(settings, static_cast<u8>(index + 1)));
    }
    std::array<std::array<u64, 4>, 4> lastSequence{};
    std::vector<Packet> pending;
    std::array<Wire, 4> wires;
    u64 sent = 0;
    u64 dropped = 0;
    u64 reordered = 0;
    u64 recoveryPackets = 0;
    u64 redundantPackets = 0;
    u64 outageDrops = 0;
    u64 heartbeats = 0;
    u64 wallNow = 0;
    const u64 hop = settings.peers == 2 ? 120'000 : 60'000;
    const s64 jitterUnit = settings.peers == 2 ? 20'000 : 10'000;
    const u64 oldestInput = settings.delay + 1;
    std::vector<RollbackExchange::Link> guests;
    for (usize index = 1; index < settings.peers; ++index) {
        guests.push_back({static_cast<u8>(index + 1), 101 + index});
        const std::array host{RollbackExchange::Link{1, kHostLink}};
        REQUIRE(peers[index]->exchange.open(
            static_cast<u8>(index + 1), kEpoch, settings.owners, settings.grants, settings.delay,
            host, [&, index] { return wallNow * kWallTick / 3 + 100'000'000 * (index + 1); }));
    }
    REQUIRE(peers[0]->exchange.open(1, kEpoch, settings.owners, settings.grants, settings.delay,
                                    guests, [&] { return wallNow * kWallTick / 3 + 100'000'000; }));
    for (usize from = 0; from < settings.peers; ++from) {
        wires[from].pingMs = static_cast<s32>(2 * hop / 3000);
        wires[from].deliver = [&, from](auto connection, std::span<const u8> bytes) {
            const usize to = from == 0 ? static_cast<usize>(connection - 101) : 0;
            REQUIRE(to < settings.peers);
            REQUIRE((from == 0 || connection == kHostLink));
            const auto message = RollbackExchange::decode(bytes);
            REQUIRE(message);
            const auto recent = peers[message->origin - 1]->driver.redundantInputs();
            if (!recent.empty() && std::ranges::any_of(message->commands, [&](const auto& input) {
                    return input.tick < recent.front().tick;
                })) {
                ++recoveryPackets;
            }
            redundantPackets += message->commands.size() > 1 ? 1 : 0;
            heartbeats += message->commands.empty() ? 1 : 0;
            const u64 sequence = ++sent;
            if (sequence % 50 == 0) {
                ++dropped;
                return;
            }
            if (settings.firstInputOutage && wallNow < kOutageTicks && from == 1 && to == 0 &&
                std::ranges::any_of(message->commands,
                                    [&](const auto& input) { return input.tick == oldestInput; })) {
                ++outageDrops;
                return;
            }
            const s64 jitter = (static_cast<s64>((sequence * 17) % 7) - 3) * jitterUnit;
            pending.push_back(
                {sequence,
                 from,
                 to,
                 wallNow * kWallTick,
                 static_cast<u64>(static_cast<s64>(wallNow * kWallTick + hop) + jitter),
                 {bytes.begin(), bytes.end()}});
        };
    }
    // Exercise empty progress/ACK heartbeats before there is a local input.
    for (usize index = 0; index < settings.peers; ++index) {
        REQUIRE(peers[index]->exchange.flush(wires[index]));
    }

    bool finished = false;
    bool outageStalled = false;
    bool oldestLeftRedundancy = false;
    for (u64 wall = 0; wall < settings.ticks + 2000 && !finished; ++wall) {
        wallNow = wall;
        std::ranges::sort(pending, [](const Packet& a, const Packet& b) {
            return a.arrives != b.arrives ? a.arrives < b.arrives : a.sequence < b.sequence;
        });
        usize delivered = 0;
        while (delivered < pending.size() && pending[delivered].arrives <= wall * kWallTick) {
            const Packet& packet = pending[delivered++];
            auto& receiver = *peers[packet.to];
            const auto connection = packet.from == 0 ? kHostLink : 101 + packet.from;
            const auto statistics = wires[packet.to].statistics(connection);
            const auto accepted = receiver.exchange.receive(
                connection, packet.bytes, statistics ? statistics->pingMs : -1,
                [&](const RollbackExchange::Message& message, std::optional<u8> transit) {
                    // The admitted origin survives the relay. Empty heartbeats
                    // still deliver pacing and receiver-owned ACK progress.
                    if (!message.commands.empty()) {
                        const auto input = receiver.driver.submit(message.origin, message.commands);
                        REQUIRE((input == Admission::Accepted || input == Admission::Duplicate ||
                                 input == Admission::Stale));
                    }
                    if (transit) {
                        // Age already includes upstream transit and relay residence;
                        // the receiver knows only its own incoming connection's RTT.
                        const auto progress = receiver.driver.observePeerTick(
                            message.origin, message.epoch, message.nextTick, *transit);
                        REQUIRE((progress == Admission::Accepted ||
                                 progress == Admission::Duplicate || progress == Admission::Stale));
                    }
                    return true;
                });
            REQUIRE(accepted == RollbackExchange::Admission::Accepted);
            reordered += packet.sequence < lastSequence[packet.to][packet.from] ? 1 : 0;
            lastSequence[packet.to][packet.from] =
                std::max(lastSequence[packet.to][packet.from], packet.sequence);
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(delivered));

        for (usize index = 0; index < settings.peers; ++index) {
            auto& peer = *peers[index];
            peer.advance(settings, wall, false);
            if (peer.driver.sampleReady() && peer.driver.sampleTick() <= settings.ticks) {
                const u64 sample = peer.driver.sampleTick();
                REQUIRE(sample - peer.driver.nextTick() <= settings.delay);
                SessionInputs::Frame local;
                local[0] =
                    SessionInputs::playInput(actual(settings, static_cast<u8>(index), sample));
                REQUIRE(peer.driver.sample(local));
                REQUIRE(peer.driver.sampleTick() == sample + 1);
                REQUIRE(peer.sampledAt.emplace(sample, wall).second);
            }
            REQUIRE(peer.exchange.publish(peer.driver.redundantInputs(), peer.driver.nextTick()));
            peer.advance(settings, wall, peer.driver.nextTick() <= settings.ticks);
            REQUIRE(peer.exchange.publish({}, peer.driver.nextTick()));
            CHECK(peer.driver.nextTick() - 1 - peer.driver.confirmedTick() <=
                  RollbackInputs::kPredictionTicks);
            CHECK(peer.driver.statistics().maxReplay <= RollbackInputs::kPredictionTicks);
        }
        if (settings.firstInputOutage && wall < kOutageTicks) {
            outageStalled |= peers[0]->driver.statistics().stalls != 0 &&
                             peers[0]->exchange.receivedThrough(2) == settings.delay;
            const auto recent = peers[1]->driver.redundantInputs();
            oldestLeftRedundancy |= !recent.empty() && recent.front().tick > oldestInput;
        }
        // Transport is serviced once per virtual 60 Hz scheduling turn, including
        // stalled or completed peers. No exchange send waits for host simulation.
        for (usize index = 0; index < settings.peers; ++index) {
            REQUIRE(peers[index]->exchange.flush(wires[index]));
        }
        finished = std::ranges::all_of(peers, [&](const auto& peer) {
            return peer->driver.nextTick() == settings.ticks + 1 &&
                   peer->driver.confirmedTick() == settings.ticks && !peer->driver.mismatchTick();
        });
    }
    REQUIRE(finished);
    REQUIRE(dropped == sent / 50);
    REQUIRE(dropped > 0);
    REQUIRE(reordered > 0);
    REQUIRE(redundantPackets > 0);
    REQUIRE(heartbeats > 0);
    if (settings.firstInputOutage) {
        REQUIRE(outageDrops > 0);
        REQUIRE(outageStalled);
        REQUIRE(oldestLeftRedundancy);
        REQUIRE(recoveryPackets > 0);
    }
    for (usize index = 0; index < settings.peers; ++index) {
        auto& peer = *peers[index];
        CAPTURE(settings.peers, settings.secret, index);
        compare(observe(peer.native), expected);
        CHECK(peer.digests == expectedDigests);
        const auto& stats = peer.driver.statistics();
        REQUIRE(peer.journals.size() == expectedJournals.size());
        std::map<FrameEventIdentity, PlayScene::OutputPayload> wantedRumbles;
        std::map<FrameEventIdentity, PlayScene::OutputPayload> wantedCommits;
        std::map<FrameEventIdentity, SceneSoundPayload> wantedAudio;
        for (usize tick = 1; tick < expectedJournals.size(); ++tick) {
            CAPTURE(tick);
            CHECK(peer.journals[tick] == expectedJournals[tick]);
            CHECK(peer.audioFrames[tick] == expectedAudio[tick]);
            if (expectedAudio[tick]) {
                for (const auto& event : expectedAudio[tick]->finite.events) {
                    wantedAudio.emplace(event.id, event.payload);
                }
            }
            for (const auto& event : expectedJournals[tick].events) {
                if (event.policy == Ledger::Policy::OneShot) {
                    wantedRumbles.emplace(event.id, event.payload);
                } else if (event.policy == Ledger::Policy::Confirmed) {
                    wantedCommits.emplace(event.id, event.payload);
                }
            }
        }
        CHECK(peer.committed == wantedCommits);
        if (settings.audio) {
            REQUIRE_FALSE(wantedAudio.empty());
            REQUIRE(peer.native.physicalAudio > 0);
            for (const auto& [id, payload] : wantedAudio) {
                REQUIRE(peer.heard.contains(id));
                if (peer.heard.at(id) != payload) {
                    CHECK(peer.unretractableAudio.contains(id));
                }
            }
            for (const auto& [id, payload] : peer.heard) {
                if (!wantedAudio.contains(id)) {
                    CHECK(peer.unretractableAudio.contains(id));
                }
            }
        }
        for (const auto& [id, payload] : wantedRumbles) {
            REQUIRE(peer.presented.contains(id));
            CHECK(peer.presented.at(id) == payload);
        }
        for (const auto& [id, payload] : peer.presented) {
            if (!wantedRumbles.contains(id)) {
                CHECK(peer.unretractable.contains(id));
            }
        }
        u64 maxLocalEdgeWait = 0;
        usize localEdges = 0;
        for (const auto& [tick, sampled] : peer.sampledAt) {
            const auto command = actual(settings, static_cast<u8>(index), tick);
            if (command.pressedButtons != 0) {
                ++localEdges;
                REQUIRE(peer.appliedAt.contains(tick));
                const auto wait = peer.appliedAt.at(tick) - sampled;
                maxLocalEdgeWait = std::max(maxLocalEdgeWait, wait);
                // A 0/1-tick local buffer is not allowed to become an RTT wait
                // in this bounded-traffic case (base RTT is 4.8 native ticks).
                if (!settings.firstInputOutage) {
                    CHECK(wait <= settings.delay);
                }
            }
        }
        if (settings.secret) {
            REQUIRE(peer.pendingUnlock);
            REQUIRE(peer.committed.size() == 1);
            REQUIRE(peer.native.unlocks.masks.size() == 1);
        } else {
            REQUIRE(localEdges >= 4);
            REQUIRE(stats.corrections > 0);
            REQUIRE(stats.replayTicks > 0);
            REQUIRE_FALSE(wantedRumbles.empty());
            REQUIRE(peer.native.haptics.pulses.size() == peer.presented.size());
        }
        std::cout << std::format(
            "native rollback {} peers={} peer={} ticks={} replay={} corrections={} max-replay={} "
            "stalls={} pacing-idles={} confirmed={} unique-rumble={} commits={} "
            "audio={} unique-finite-audio={} max-local-edge-wait-virtual-ticks={}\n",
            settings.secret ? "S4" : "G1", settings.peers, index + 1, stats.nativeTicks,
            stats.replayTicks, stats.corrections, stats.maxReplay, stats.stalls, stats.pacingIdles,
            peer.driver.confirmedTick(), peer.presented.size(), peer.committed.size(),
            settings.audio, peer.heard.size(), maxLocalEdgeWait);
        const auto timings = [&](std::string_view label, std::vector<f64> samples) {
            std::ranges::sort(samples);
            const auto percentile = [&](usize percent) {
                return samples.empty() ? 0.0 : samples[(samples.size() - 1) * percent / 100];
            };
            std::cout << std::format(
                "measured driver wall peers={} peer={} kind={} samples={} "
                "p50-ms={:.3f} p95-ms={:.3f} max-ms={:.3f} max-correction-frames={}\n",
                settings.peers, index + 1, label, samples.size(), percentile(50), percentile(95),
                samples.empty() ? 0.0 : samples.back(), peer.maxCorrectionFrames);
        };
        // Informational, not a machine-dependent pass threshold. These measured
        // durations must never be reported as the virtual 80 ms link's RTT.
        timings("correction-batch", peer.correctionMilliseconds);
        timings("forward-tick", peer.forwardMilliseconds);
    }
    std::cout << std::format(
        "virtual star hop-ms={} maximum-rtt-ms=80 sends={} dropped={} reordered={} "
        "old-input-recovery-packets={} heartbeats={} deliberate-outage-drops={}\n",
        hop / 3000, sent, dropped, reordered, recoveryPackets, heartbeats, outageDrops);
}

TEST_CASE("native scenes correct delayed lossy inputs against an all-actual reference",
          "[native-rollback-scene][assets]") {
    const usize peers = GENERATE(usize{2}, usize{4});
    Settings settings{peers, false};
    settings.delay = GENERATE(u8{0}, u8{1});
    run(settings);
}

TEST_CASE("native stalled rollback recovers input older than the latest eight through its exchange",
          "[native-rollback-scene][native-rollback-exchange-outage][assets]") {
    Settings settings{2, false};
    settings.firstInputOutage = true;
    run(settings);
}

TEST_CASE("a native secret unlock waits for every peer through the delayed input frontier",
          "[native-rollback-scene][native-rollback-secret][assets]") {
    run(Settings{2, true});
}

TEST_CASE("native delayed scene audio is published only after the entire correction suffix",
          "[native-rollback-scene][native-rollback-audio][assets]") {
    run(Settings{2, false, true});
}
} // namespace
