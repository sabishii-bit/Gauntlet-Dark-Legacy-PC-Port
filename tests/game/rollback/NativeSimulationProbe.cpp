// Runs the real gameplay update in an independent process. Offline mode retains
// its partial diagnostic observer; network mode exercises native rollback over
// actual Iroh (local by default, relay opt-in), reporting confirmed native hashes.
// Neither mode uses a replica scene or opens an audio/display device.
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/io/Sha256.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "game/app/NativeOnlineIdentity.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/netplay/AssetDigest.h"
#include "game/rollback/InputCommand.h"
#include "game/rollback/NativeProbeLink.h"
#include "game/rollback/NativeProbeTiming.h"
#include "game/rollback/SessionInputs.h"
#include "game/rollback/SimulationObservation.h"
#include "game/rollback/SimulationObserver.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Json = nlohmann::json;

struct Options {
    std::filesystem::path report;
    std::filesystem::path timings;
    u32 timingWarmup = 0;
    bool enemyRoundtrip = false;
    std::filesystem::path assets = GDL_TEST_ASSET_DIR;
    std::filesystem::path data = GDL_TEST_DATA_DIR;
    std::string stage = "G1";
    std::string workload = "combat";
    u32 ticks = 600;
    u32 players = 2;
    u32 renderEvery = 0;
    u32 perturbTick = 0;
    std::string networkRole;
    std::string networkPath = "local";
    u32 networkPeer = 0;
    std::filesystem::path ticket;
    u32 inputDelay = 0;
    test::NativeProbeImpairment impairment;
    std::vector<std::string> mechanics; ///< switched on, by id, from the build's own
};

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string(reason));
    }
}

u32 number(std::string_view value) {
    u32 result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Expected an unsigned integer");
    return result;
}

Options parse(std::span<char*> args) {
    Options result;
    for (usize index = 1; index < args.size(); ++index) {
        const std::string_view key = args[index];
        require(index + 1 < args.size(), "Missing option value");
        const std::string_view value = args[++index];
        if (key == "--report") {
            result.report = value;
        } else if (key == "--timings") {
            result.timings = value;
        } else if (key == "--timing-warmup") {
            result.timingWarmup = number(value);
        } else if (key == "--enemy-roundtrip") {
            require(value == "0" || value == "1", "Enemy roundtrip must be 0 or 1");
            result.enemyRoundtrip = value == "1";
        } else if (key == "--assets") {
            result.assets = value;
        } else if (key == "--data") {
            result.data = value;
        } else if (key == "--stage") {
            result.stage = value;
        } else if (key == "--workload") {
            require(value == "idle" || value == "combat", "Workload must be idle or combat");
            result.workload = value;
        } else if (key == "--ticks") {
            result.ticks = number(value);
        } else if (key == "--players") {
            result.players = number(value);
        } else if (key == "--render-every") {
            result.renderEvery = number(value);
        } else if (key == "--perturb-tick") {
            result.perturbTick = number(value);
        } else if (key == "--network-role") {
            require(value == "host" || value == "guest", "Network role must be host or guest");
            result.networkRole = value;
        } else if (key == "--ticket") {
            result.ticket = value;
        } else if (key == "--network-path") {
            require(value == "local" || value == "relay", "Network path must be local or relay");
            result.networkPath = value;
        } else if (key == "--network-peer") {
            result.networkPeer = number(value);
        } else if (key == "--input-delay") {
            result.inputDelay = number(value);
        } else if (key == "--input-latency-ms") {
            result.impairment.latencyMs = number(value);
        } else if (key == "--input-jitter-ms") {
            result.impairment.jitterMs = number(value);
        } else if (key == "--input-loss-ppm") {
            result.impairment.lossPpm = number(value);
        } else if (key == "--input-reorder-ppm") {
            result.impairment.reorderPpm = number(value);
        } else if (key == "--input-reorder-ms") {
            result.impairment.reorderMs = number(value);
        } else if (key == "--input-impairment-seed") {
            result.impairment.seed = number(value);
        } else if (key == "--input-outage-tick") {
            result.impairment.outageTick = number(value);
        } else if (key == "--input-outage-ms") {
            result.impairment.outageMs = number(value);
        } else if (key == "--input-outage-peer") {
            result.impairment.outagePeer = number(value);
        } else if (key == "--mechanic") {
            result.mechanics.emplace_back(value);
        } else {
            throw std::runtime_error("Unknown option: " + std::string(key));
        }
    }
    require(!result.report.empty(), "--report is required");
    require(!result.enemyRoundtrip || (!result.timings.empty() && result.networkRole.empty()),
            "Enemy roundtrip requires --timings and offline mode");
    require(!std::filesystem::exists(result.report), "Report exists; choose a new path");
    require(result.ticks > 0 && result.ticks <= 36'000, "Ticks must be 1..36000");
    require(!result.timings.empty() || result.timingWarmup == 0,
            "Timing warmup requires --timings");
    require(result.timingWarmup < result.ticks, "Timing warmup must leave measured ticks");
    if (!result.timings.empty()) {
        require(!std::filesystem::exists(result.timings), "Timings exist; choose a new path");
        require(std::filesystem::weakly_canonical(result.timings) !=
                    std::filesystem::weakly_canonical(result.report),
                "Timing and observation reports must be distinct");
    }
    require(result.players >= 1 && result.players <= 4, "Players must be 1..4");
    require(result.renderEvery <= 60, "Render interval must be 0..60 ticks");
    require(result.perturbTick <= result.ticks, "Perturbation is outside the run");
    require(result.inputDelay <= RollbackInputs::kMaxDelay, "Input delay must be 0..4");
    require(result.networkRole.empty() || result.timings.empty(),
            "Network rollback reports contain their own native timing metrics");
    require(result.networkRole.empty() == result.ticket.empty(), "Network role requires a ticket");
    require(!result.networkRole.empty() || result.networkPath == "local",
            "Relay path requires network mode");
    require(!result.networkRole.empty() || result.inputDelay == 0, "Delay requires network mode");
    require(result.networkRole.empty() || result.players >= 2,
            "Network probe requires two to four players");
    const auto& impairment = result.impairment;
    require(impairment.latencyMs <= 500 && impairment.jitterMs <= 500 &&
                impairment.lossPpm <= 250'000 && impairment.reorderPpm <= 1'000'000 &&
                impairment.reorderMs <= 500 && impairment.seed != 0 &&
                ((impairment.reorderPpm == 0) == (impairment.reorderMs == 0)),
            "Invalid bounded native input impairment");
    require(
        (impairment.outageMs == 0 && impairment.outageTick == 0 && impairment.outagePeer == 0) ||
            (impairment.outageMs <= 5000 && impairment.outageMs != 0 &&
             impairment.outageTick != 0 && impairment.outageTick < result.ticks &&
             result.ticks - impairment.outageTick > 8 && impairment.outagePeer >= 1 &&
             impairment.outagePeer <= result.players),
        "Outage requires a peer, duration1..5000ms and a tick leaving eight recovery ticks");
    require(!result.networkRole.empty() || !impairment.enabled(),
            "Input impairment requires real network mode");
    if (!result.networkRole.empty()) {
        if (result.networkPeer == 0) {
            result.networkPeer = result.networkRole == "host" ? 1 : 2;
        }
        require(
            result.networkPeer <= result.players &&
                (result.networkRole == "host" ? result.networkPeer == 1 : result.networkPeer >= 2),
            "Network peer does not match its role or player count");
    } else {
        require(result.networkPeer == 0, "Network peer requires network mode");
    }
    return result;
}

std::string hex(std::span<const u8> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        result += kDigits[byte >> 4U];
        result += kDigits[byte & 15U];
    }
    return result;
}

std::string digest(std::span<const u8> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.hex();
}

std::string eventDigest(const PlayScene::OutputLedger::Journal& journal) {
    NativeStateDigest digest;
    digest.string("native-probe/output/1");
    digest.wide(journal.events.size());
    for (const auto& event : journal.events) {
        const auto& [id, policy, payload, loop] = event;
        digest.values(id, policy, loop, payload.index());
        std::visit(
            [&](const auto& value) {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                             PlayScene::RumbleOutput>) {
                    digest.fields<3>(value);
                } else {
                    digest.fields<1>(value);
                }
            },
            payload);
    }
    return hex(digest.finish());
}

std::string audioDigest(const SceneSoundEvents::Frame& frame) {
    NativeStateDigest digest;
    digest.string("native-probe/audio/1");
    const auto payload = [&](const SceneSoundPayload& value) {
        const auto& [resource, volume, pan, after, playback, start] = value;
        digest.fields<2>(resource);
        digest.values(volume, pan, after, playback, start);
    };
    const auto& [finite, loops] = frame;
    const auto& [events] = finite;
    digest.wide(events.size());
    for (const auto& event : events) {
        const auto& [id, policy, sound, loop] = event;
        digest.values(id, policy, loop);
        payload(sound);
    }
    digest.wide(loops.size());
    for (const auto& loop : loops) {
        const auto& [key, sound] = loop;
        digest.value(key);
        payload(sound);
    }
    return hex(digest.finish());
}

Json impairmentConfig(const test::NativeProbeImpairment& value) {
    return {{"mode", value.enabled() ? "simulated-application-input" : "none"},
            {"scope", "outbound-unreliable-input-only"},
            {"reliable_impaired", false},
            {"latency_ms", value.latencyMs},
            {"jitter_ms", value.jitterMs},
            {"loss_ppm", value.lossPpm},
            {"reorder_ppm", value.reorderPpm},
            {"reorder_ms", value.reorderMs},
            {"seed", value.seed},
            {"outage_tick", value.outageTick},
            {"outage_ms", value.outageMs},
            {"outage_peer", value.outagePeer}};
}
Json networkMetrics(const test::NativeProbeLink::Metrics& value) {
    const auto& injected = value.impairment;
    Json paths = Json::array();
    for (const auto& link : value.links) {
        paths.push_back({{"peer", link.peer},
                         {"samples", link.samples},
                         {"relay_samples", link.relaySamples},
                         {"direct_samples", link.directSamples},
                         {"unavailable_samples", link.unavailableSamples},
                         {"rtt_p50_ms", link.rttP50Ms},
                         {"rtt_p95_ms", link.rttP95Ms},
                         {"rtt_max_ms", link.rttMaxMs}});
    }
    return {{"sampled_inputs", value.sampledInputs},
            {"input_sample_to_first_native_update_p50_ms", value.inputToUpdateP50Ms},
            {"input_sample_to_first_native_update_p95_ms", value.inputToUpdateP95Ms},
            {"native_ticks", value.simulation.nativeTicks},
            {"replay_ticks", value.simulation.replayTicks},
            {"corrections", value.simulation.corrections},
            {"max_replay", value.simulation.maxReplay},
            {"stalls", value.simulation.stalls},
            {"pacing_idles", value.simulation.pacingIdles},
            {"prediction_max_ticks", value.predictionMaxTicks},
            {"correction_p50_ms", value.correctionP50Ms},
            {"correction_p95_ms", value.correctionP95Ms},
            {"correction_max_ms", value.correctionMaxMs},
            {"link_rtt_ms", value.linkRttMs},
            {"transport", {{"local_peer", value.localPeer}, {"links", paths}}},
            {"impairment",
             {{"attempted", injected.attempted},
              {"enqueued", injected.enqueued},
              {"transmitted", injected.transmitted},
              {"loss_dropped", injected.lossDropped},
              {"outage_dropped", injected.outageDropped},
              {"reordered", injected.reordered},
              {"oldest_recovery_datagrams", injected.oldestRecoveryDatagrams},
              {"queue_peak", injected.queuePeak},
              {"queue_pending", injected.queuePending},
              {"queue_abandoned", injected.queueAbandoned},
              {"outage_started_tick", injected.outageStartedTick},
              {"outage_finished", injected.outageFinished},
              {"outage_elapsed_ms", injected.outageElapsedMs},
              {"outage_progress",
               {{"updates", injected.outageProgress.updates},
                {"stalls", injected.outageProgress.stalls},
                {"pacing_idles", injected.outageProgress.pacingIdles},
                {"first_next_tick", injected.outageProgress.firstNextTick},
                {"last_next_tick", injected.outageProgress.lastNextTick},
                {"first_confirmed_tick", injected.outageProgress.firstConfirmedTick},
                {"last_confirmed_tick", injected.outageProgress.lastConfirmedTick},
                {"prediction_max_ticks", injected.outageProgress.predictionMaxTicks}}},
              {"residence_p50_ms", injected.residenceP50Ms},
              {"residence_p95_ms", injected.residenceP95Ms},
              {"residence_max_ms", injected.residenceMaxMs}}}};
}

// Journal v1 is tick-indexed, including startup. Aim is already resolved into
// world space, just as it must be at the device/network input boundary. No mouse
// projection, current camera, wall clock or render cadence selects these inputs.
std::vector<InputCommand> journal(const Options& options, u32 tick) {
    constexpr std::array<Vec2, 4> kDirections{Vec2{0, 1}, Vec2{1, 0}, Vec2{0, -1}, Vec2{-1, 0}};
    std::vector<InputCommand> commands;
    for (u32 seat = 0; seat < options.players; ++seat) {
        PlayInput input;
        if (options.workload == "combat" && tick > 360) {
            const u32 elapsed = tick - 361;
            const u32 phase = (elapsed + seat * 30) % 240;
            const Vec2 direction = kDirections[(elapsed / 120 + seat) % kDirections.size()];
            input.move = {direction, 1};
            input.aimDirection = Vec3{direction.x, 0, -direction.y};
            input.attack = phase < 90;
            input.attackPressed = phase == 0;
            input.strongAttack = phase >= 120 && phase < 150;
            input.turbo = phase >= 180 && phase < 190;
            input.defendPressed = phase == 180;
            input.usePotion = elapsed % 600 == 300;
            input.menu.select = elapsed % 120 == 0;
            input.menu.buttonPressed = input.menu.select;
        }
        // Negative control changes the resolved journal, not scene internals.
        if (seat == 0 && tick == options.perturbTick) {
            input.move = {{-1, 0}, 1};
            input.aimDirection = Vec3{-1, 0, 0};
            input.usePotion = true;
        }
        commands.push_back(SessionInputs::command(input, 1, tick, 1, static_cast<u8>(seat)));
    }
    return commands;
}

void run(const Options& options) {
    GameConfig config;
    config.timing.tickRate = 60;
    MechanicCatalog mechanics;
    registerCompiledMechanics(mechanics);
    for (const std::string& id : options.mechanics) {
        require(mechanics.find(id) != nullptr, "This build carries no mechanic " + id);
        config.game.mechanics[id] = true;
    }
    LevelCatalog levels;
    StringTable strings;
    require(levels.load(options.assets), "Native level catalog missing");
    require(strings.load(options.data / "text", "en"), "Shipped text missing");
    const auto level =
        options.stage == "tower" ? std::optional{LevelRef::tower()} : levels.byName(options.stage);
    require(level.has_value(), "Requested stage missing from native catalog");
    const std::string assetsDigest = assetDigest(options.assets);
    const std::string dataDigest = assetDigest(options.data);
    const std::string configuration = config.toJson();
    const auto configBytes =
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        std::span{reinterpret_cast<const u8*>(configuration.data()), configuration.size()};
    test::FakeRenderDevice device;
    device.recordDraws = false;
    LevelWorld world;
    require(world.load(device, options.assets, *level), "Could not load native stage");
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.mechanics = &mechanics;
    context.strings = &strings;
    context.unpackedRoot = options.assets;
    f32 stopTimeTotal = 0;
    // No save slots, persisted settings, audio devices or other external side
    // effects. Their exclusion is part of the observation's coverage contract.
    std::vector<PartyMember> party;
    for (u32 seat = 0; seat < options.players; ++seat) {
        CharacterSave character;
        character.name = "PROBE";
        character.character = static_cast<s32>(seat);
        character.color = static_cast<s32>(seat);
        character.progress().health = 2000;
        character.progress().inventory.addKeys(9);
        character.progress().inventory.addPotions(static_cast<s32>(seat + 1), 9);
        party.push_back({static_cast<s32>(seat), character});
    }
    PlayScene scene;
    PlayOptions openOptions;
    openOptions.welcome = false;
    require(
        scene.open(device, context, world, party, openOptions, {.stopTimeTotal = &stopTimeTotal}),
        "Could not open native scene");
    std::ofstream report(options.report, std::ios::binary);
    require(report.is_open(), "Could not open report");
    const auto write = [&](const Json& row) {
        report << row.dump() << '\n';
        require(report.good(), "Could not write report");
    };
    const std::string configDigest = digest(configBytes);
    if (!options.networkRole.empty()) {
        std::string identityError;
        const auto identityInputs = NativeOnlineIdentity::snapshot(strings, &identityError);
        require(identityInputs.has_value(), identityError);
        const auto identity = NativeOnlineIdentity::capture(*identityInputs, &identityError);
        require(identity.has_value() && NativeOnlineIdentity::numericMatches(*identityInputs),
                identityError.empty() ? "Native runtime identity changed" : identityError);
        const std::string contract = Json{
            {"assets", assetsDigest},
            {"data", dataDigest},
            {"runtime", identity->runtime},
            {"language", identity->language},
            {"config", configDigest},
            {"stage", options.stage},
            {"ticks", options.ticks},
            {"workload", options.workload},
            {"delay", options.inputDelay},
            {"network_path", options.networkPath},
            {"players", options.players},
            {"impairment", impairmentConfig(options.impairment)},
            {"protocol",
             "native-rollback-probe-v4"}}.dump();
        const auto contractBytes =
            // byte view of a trivially copyable buffer
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            std::span{reinterpret_cast<const u8*>(contract.data()), contract.size()};
        auto network = std::make_unique<test::NativeProbeLink>(
            options.networkRole == "host", options.ticket, digest(contractBytes),
            static_cast<u8>(options.inputDelay), scene, static_cast<u8>(options.players),
            static_cast<u8>(options.networkPeer), options.impairment,
            options.networkPath == "relay");
        write({{"type", "metadata"},
               {"schema", 2},
               {"simulation", "NativeRollbackDriver"},
               {"stage", options.stage},
               {"ticks", options.ticks},
               {"players", options.players},
               {"tick_rate", 60},
               {"render_every", options.renderEvery},
               {"workload", options.workload},
               {"input_version", 1},
               {"input_delivery", "iroh-rollback"},
               {"input_delay", options.inputDelay},
               {"network_path", options.networkPath},
               {"initialization", "native-defaults-v1"},
               {"assets_digest", assetsDigest},
               {"data_digest", dataDigest},
               {"config_digest", configDigest},
               {"runtime_digest", identity->runtime},
               {"complete_state", false},
               {"subsystems", {"audio", "events", "inputs", "native_scene_and_control"}},
               {"unobserved", {"Rendered pixels", "Physical audio and controller output"}},
               {"agreement_schema", "native-scene-and-control-v1"},
               {"impairment", impairmentConfig(options.impairment)}});
        const Mat4 projection = glm::ortho(0.0f, 640.0f, 448.0f, 0.0f, -1.0f, 1.0f);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{60 + options.ticks / 30};
        while (network->completed() < options.ticks) {
            require(std::chrono::steady_clock::now() < deadline, "Native rollback probe timed out");
            const auto result = network->frame(options.ticks, [&](u64 tick, u8 seat) {
                return journal(options, static_cast<u32>(tick))[seat];
            });
            for (const auto& confirmed : result.confirmed) {
                const auto& frame = confirmed.frame;
                const auto commands = std::span{frame.input.commands}.first(options.players);
                const auto bytes = InputPacket::encode(commands);
                require(bytes.has_value() && frame.audio.has_value(),
                        "Confirmed native report lacks input or audio");
                write({{"type", "tick"},
                       {"tick", frame.tick},
                       {"input_digest", digest(*bytes)},
                       {"event_digest", eventDigest(frame.journal)},
                       {"audio_digest", audioDigest(*frame.audio)}});
                if (confirmed.digest) {
                    write({{"type", "confirmed"},
                           {"tick", frame.tick},
                           {"digest", hex(*confirmed.digest)}});
                }
            }
            if (result.advanced && options.renderEvery != 0 &&
                result.simulatedThrough % options.renderEvery == 0) {
                require(device.beginFrame(), "Headless frame refused");
                scene.render(device, projection, 640, 448, false, 0.5f);
                device.endFrame();
            }
        }
        const auto final = network->finalDigest();
        network->finish();
        write({{"type", "complete"},
               {"ticks", options.ticks},
               {"last_tick", options.ticks},
               {"native_digest", hex(final)},
               {"metrics", networkMetrics(network->metrics())}});
        report.close();
        require(!report.fail(), "Could not finish native rollback report");
        return;
    }
    // The older offline observer is diagnostic only. The network path above
    // hashes actual captured native values and does not bind these catalogs.
    SimulationObserver observer{scene};
    require(observer.bind(), "Could not bind native observation resources");
    const Mat4 projection = glm::ortho(0.0f, 640.0f, 448.0f, 0.0f, -1.0f, 1.0f);
    u32 readyTicks = 0;
    u32 movedTicks = 0;
    usize mostEnemies = 0;
    usize mostProjectiles = 0;
    std::vector<f64> updateTimes;
    std::vector<f64> observationTimes;
    std::vector<f64> renderTimes;
    std::vector<f64> enemySaveTimes;
    std::vector<f64> enemyRestoreTimes;
    auto enemySaved = options.enemyRoundtrip ? std::make_unique<Enemies::LocalSnapshot>() : nullptr;
    if (!options.timings.empty()) {
        updateTimes.reserve(options.ticks);
        observationTimes.reserve(options.ticks);
        renderTimes.reserve(options.ticks);
        if (enemySaved) {
            enemySaveTimes.reserve(options.ticks);
            enemyRestoreTimes.reserve(options.ticks);
        }
    }
    for (u32 tick = 1; tick <= options.ticks; ++tick) {
        const auto measure = [&](std::vector<f64>& samples, const auto& operation) {
            if (options.timings.empty() || tick <= options.timingWarmup) {
                operation();
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            operation();
            const f64 elapsed =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                    .count();
            samples.push_back(elapsed);
        };
        const auto commands = journal(options, tick);
        const auto bytes = InputPacket::encode(commands);
        require(bytes.has_value(), "Invalid journal input");
        PlayScene::Inputs inputs;
        for (const auto& command : commands) {
            inputs[command.seat] = SessionInputs::playInput(command);
        }
        const Vec3 before = scene.actor(0)->position();
        PlayOutcome outcome = PlayOutcome::Running;
        measure(updateTimes, [&] { outcome = scene.update(1.0 / 60, inputs); });
        require(outcome == PlayOutcome::Running,
                "Scene requested travel or ended; campaign transitions are not exercised yet");
        // An identity roundtrip inside the real scene tests integration without pretending
        // that rewinding only enemies could replay a whole scene. The observer and a separate
        // uninterrupted run compare outcomes; neither capture nor restore emits world events.
        if (enemySaved) {
            bool saved = false;
            bool restored = false;
            measure(enemySaveTimes, [&] { saved = scene.enemies().saveLocal(*enemySaved); });
            require(saved, "Local enemy save refused");
            measure(enemyRestoreTimes,
                    [&] { restored = scene.enemies().restoreLocal(*enemySaved); });
            require(restored, "Local enemy restore refused");
        }
        readyTicks += scene.spawning() ? 0U : 1U;
        movedTicks += before == scene.actor(0)->position() ? 0U : 1U;
        mostEnemies = std::max(mostEnemies, scene.enemies().count());
        mostProjectiles = std::max(mostProjectiles, scene.missiles().count());
        if (options.renderEvery != 0 && tick % options.renderEvery == 0) {
            measure(renderTimes, [&] {
                require(device.beginFrame(), "Headless frame refused");
                scene.render(device, projection, 640, 448, false, 0.5f);
                device.endFrame();
            });
        }
        SimulationObservation observation;
        measure(observationTimes, [&] { observation = observer.observe(); });
        std::ranges::sort(observation.subsystems, {}, &SimulationObservation::Subsystem::name);
        if (tick == 1) {
            std::vector<std::string> names;
            names.reserve(observation.subsystems.size());
            for (const auto& subsystem : observation.subsystems) {
                names.push_back(subsystem.name);
            }
            write({{"type", "metadata"},
                   {"schema", 1},
                   {"simulation", "PlayScene::update"},
                   {"stage", options.stage},
                   {"ticks", options.ticks},
                   {"players", options.players},
                   {"tick_rate", 60},
                   {"render_every", options.renderEvery},
                   {"workload", options.workload},
                   {"input_version", 1},
                   {"input_delivery", "local"},
                   {"input_delay", options.inputDelay},
                   {"initialization", "native-defaults-v1"},
                   {"assets_digest", assetsDigest},
                   {"data_digest", dataDigest},
                   {"config_digest", configDigest},
                   {"complete_state", observation.complete},
                   {"unobserved", observation.unobserved},
                   {"subsystems", names}});
        }
        Json hashes = Json::object();
        Json unavailable = Json::object();
        for (const auto& subsystem : observation.subsystems) {
            if (subsystem.digest) {
                hashes[subsystem.name] = hex(*subsystem.digest);
            } else {
                unavailable[subsystem.name] = subsystem.unavailable;
            }
        }
        write({{"type", "tick"},
               {"tick", tick},
               {"input_digest", digest(*bytes)},
               {"subsystems", hashes},
               {"unavailable", unavailable}});
    }
    write({{"type", "complete"}, {"ticks", options.ticks}, {"last_tick", options.ticks}});
    report.close();
    require(!report.fail(), "Could not finish report");
    if (!options.timings.empty()) {
        const auto summary = [](std::span<const f64> samples) -> Json {
            const auto value = test::summarizeNativeTimings(samples);
            if (!value) {
                require(samples.empty(), "Invalid measured timing");
                return nullptr;
            }
            return {{"samples", value->samples}, {"mean", value->mean},
                    {"p50", value->p50},         {"p95", value->p95},
                    {"p99", value->p99},         {"maximum", value->maximum}};
        };
        const Json timings{{"schema", 1},
                           {"units", "milliseconds"},
                           {"simulation", "PlayScene::update"},
                           {"tick_rate", 60},
                           {"stage", options.stage},
                           {"players", options.players},
                           {"workload", options.workload},
                           {"input_delivery", "local"},
                           {"assets_digest", assetsDigest},
                           {"data_digest", dataDigest},
                           {"config_digest", configDigest},
                           {"ticks", options.ticks},
                           {"warmup_ticks", options.timingWarmup},
                           {"render_every", options.renderEvery},
                           {"headless", true},
                           {"ready_ticks", readyTicks},
                           {"moved_ticks", movedTicks},
                           {"most_enemies", mostEnemies},
                           {"most_projectiles", mostProjectiles},
                           {"update", summary(updateTimes)},
                           {"diagnostic_observation", summary(observationTimes)},
                           {"headless_render", summary(renderTimes)},
                           {"enemy_pool_roundtrip",
                            {{"enabled", options.enemyRoundtrip},
                             {"save", summary(enemySaveTimes)},
                             {"restore", summary(enemyRestoreTimes)}}},
                           {"save", nullptr},
                           {"restore", nullptr},
                           {"replay", nullptr},
                           {"complete_rewind", false}};
        std::ofstream timingReport(options.timings, std::ios::binary);
        require(timingReport.is_open(), "Could not open timing report");
        timingReport << timings.dump(2) << '\n';
        timingReport.close();
        require(!timingReport.fail(), "Could not finish timing report");
    }
    scene.close();
    std::cout << "Native simulation report: " << options.report << '\n';
    std::cout << "Native workload: ready_ticks=" << readyTicks << " moved_ticks=" << movedTicks
              << " most_enemies=" << mostEnemies << " most_player_projectiles=" << mostProjectiles
              << '\n';
}
} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): the handler only reports
int main(int argc, char** argv) {
    try {
        std::string numericError;
        if (!NativeOnlineIdentity::prepareProcessNumeric(&numericError)) {
            throw std::runtime_error(numericError);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout
                << "nativeprobe --report path.jsonl [--stage G1|tower|...] [--ticks 600] "
                   "[--players 1..4] [--render-every 0..60] [--workload idle|combat] "
                   "[--assets path] [--data path] [--perturb-tick tick] [--mechanic id]... "
                   "[--timings timings.json --timing-warmup ticks] "
                   "[--enemy-roundtrip 0|1 (offline, with timings)] "
                   "[--network-role host|guest --network-peer 1..4 --ticket path "
                   "--input-delay 0..2 --network-path local|relay]\n"
                   "[--input-latency-ms 0..500 --input-jitter-ms 0..500 "
                   "--input-loss-ppm 0..250000 --input-reorder-ppm 0..1000000 "
                   "--input-reorder-ms 0..500 --input-impairment-seed positive]\n"
                   "[--input-outage-tick tick --input-outage-ms 1..5000 --input-outage-peer 1..4]\n"
                   "Relay is opt-in public Iroh with direct paths disabled. "
                   "Impairment is simulated outbound application input, "
                   "not Internet latency. Reliable controls and provider RTT are unmodified.\n"
                   "No desktop control, audio device, or persisted saves. "
                   "Partial state observation; not a full determinism certificate.\n";
            return 0;
        }
        run(parse({argv, static_cast<usize>(argc)}));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native simulation probe failed: " << error.what() << '\n';
        return 1;
    }
}
