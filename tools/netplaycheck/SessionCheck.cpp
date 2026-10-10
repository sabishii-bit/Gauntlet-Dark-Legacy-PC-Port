#include "SessionCheck.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "game/netplay/ClientClock.h"
#include "game/netplay/OnlineSession.h"
#include "game/netplay/RoomSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void report(const nlohmann::json& message) {
    std::cout << message.dump() << '\n';
    std::cout.flush();
}
CombatSnapshot checkpoint(const MatchSession& match, const std::array<f32, 4>& travel) {
    CombatSnapshot state;
    state.motion.epoch = match.context().epoch;
    state.motion.tick = match.tick() - 1;
    state.motion.cameraContinuity = 1;
    state.motion.camera.pitch = 0.5f;
    for (usize seat = 0; seat < 4; ++seat) {
        if (match.context().owners[seat] != 0) {
            state.motion.players[seat] =
                SeatMotion{match.context().grants[seat], 1, {travel[seat], 0, 0}, 0};
            state.players[seat] =
                PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, 0, 1}};
        }
    }
    return state;
}
} // namespace

void checkOnlineSession(bool host, const std::string& endpoint, const std::string& code,
                        gdl::u8 localPlayers, gdl::usize totalPlayers,
                        gdl::GnsTransport::Simulation simulation) {
    // All room/ICE admission and match routing below uses the application driver.
    // Only gameplay and loading work are synthetic; there is no second lobby loop.
    RoomSession service(endpoint, code, localPlayers, "session-test-1", std::string(64, 'a'));
    std::string error;
    auto transport = GnsTransport::create(simulation, error);
    require(transport != nullptr, error);
    OnlineSession session(service, *transport, host, localPlayers);
    std::vector<CharacterProfile> selections(localPlayers);
    for (usize player = 0; player < selections.size(); ++player) {
        selections[player].name = host ? "HOST" : "GUEST";
        selections[player].character = static_cast<s32>(player);
    }
    require(session.select(selections), "Online character selection failed");
    auto& match = session.match();
    ClientClock presentation;
    const auto began = Clock::now();
    auto previous = began;
    auto nextTick = began;
    auto readyAt = began;
    auto stallUntil = began;
    auto progressAt = began + std::chrono::seconds(3);
    u64 loadingEpoch = 0;
    u64 firstEpoch = 0;
    u64 loadedEpoch = 0;
    u64 sampledEpoch = 0;
    u64 sampledTick = 0;
    u64 thirdEpochTick = 0;
    usize snapshots = 0;
    usize barriers = 0;
    usize players = 0;
    usize inputs = 0;
    usize interpolated = 0;
    std::array<f32, 4> travel{};
    bool announced = false;
    bool leaving = false;
    bool paused = false;
    bool stalled = false;
    s64 startupMs = 0;
    while (Clock::now() - began < std::chrono::seconds(30)) {
        const auto now = Clock::now();
        const f64 elapsed = std::chrono::duration<f64>(now - previous).count();
        session.update(elapsed);
        previous = now;
        if (now >= progressAt) {
            report({{"event", "progress"},
                    {"host", host},
                    {"session_phase", static_cast<u32>(session.phase())},
                    {"match_phase", static_cast<u32>(match.phase())},
                    {"epoch", match.context().epoch},
                    {"tick", match.tick()},
                    {"loaded_epoch", loadedEpoch},
                    {"snapshots", snapshots}});
            progressAt = now + std::chrono::seconds(3);
        }
        if (session.phase() == OnlineSession::Phase::Failed) {
            // End-of-test shutdown is initiated only after all four load barriers.
            // Every guest must prove it saw live state before acknowledging #4.
            require(!host && loadedEpoch == 4 && thirdEpochTick >= 90 &&
                        (session.failure() == OnlineSession::Failure::Match ||
                         session.failure() == OnlineSession::Failure::Transport ||
                         session.failure() == OnlineSession::Failure::Service),
                    "Online session stopped early: " +
                        std::to_string(static_cast<u32>(session.failure())));
            report({{"event", "passed"},
                    {"role", "session-client"},
                    {"epochs", loadedEpoch},
                    {"barriers", barriers},
                    {"players", players},
                    {"snapshots", snapshots},
                    {"third_epoch_tick", thirdEpochTick},
                    {"inputs", inputs},
                    {"interpolated", interpolated},
                    {"startup_ms", startupMs}});
            return;
        }
        if (session.phase() == OnlineSession::Phase::Closed) {
            require(host && leaving && loadedEpoch == 4, "Unexpected online close");
            report({{"event", "passed"},
                    {"role", "session-host"},
                    {"epochs", loadedEpoch},
                    {"barriers", barriers},
                    {"players", players},
                    {"inputs", inputs},
                    {"startup_ms", startupMs},
                    {"paused", paused},
                    {"stalled", stalled}});
            return;
        }
        if (const auto* room = session.room()) {
            if (host && !announced) {
                report({{"event", "room_created"}, {"code", room->code}});
                announced = true;
            }
            players = 0;
            for (const auto& member : room->members) {
                players += member.seats.size();
            }
            if (session.phase() == OnlineSession::Phase::Lobby) {
                session.ready();
                if (host && players == totalPlayers) {
                    session.start();
                }
            }
        }
        if (host && session.phase() == OnlineSession::Phase::Active &&
            match.phase() == MatchSession::Phase::Lobby) {
            require(session.party() != nullptr, "Online party was not agreed before loading");
            require(static_cast<usize>(std::ranges::count_if(
                        *session.party(),
                        [](const auto& profile) { return profile.has_value(); })) == totalPlayers,
                    "Online party omitted a remote character");
            require(match.prepare(1), "Online match prepare failed");
        }
        if (firstEpoch == 0 && match.context().epoch != 0) {
            firstEpoch = match.context().epoch;
        }
        const auto epoch = firstEpoch == 0 ? 0 : match.context().epoch - firstEpoch + 1;
        if (match.phase() == MatchSession::Phase::Loading) {
            if (epoch != loadingEpoch) {
                if (!host && epoch == 4) {
                    require(thirdEpochTick >= 90 && snapshots >= 20 && inputs >= 120,
                            "Online scene transitions lacked live movement");
                }
                loadingEpoch = epoch;
                if (!host) {
                    require(presentation.begin(match), "Online presentation clock failed to bind");
                }
                readyAt = now + std::chrono::milliseconds(host ? 20 : 120);
                ++barriers;
                travel = {};
            }
            require(!match.advance(), "Loading advanced the simulation");
            if (now >= readyAt && loadedEpoch != epoch &&
                (match.context().transition != MatchTransition::Travel || match.travelParty())) {
                if (match.context().transition == MatchTransition::Travel) {
                    for (usize seat = 0; seat < 4; ++seat) {
                        const auto& profile = (*match.travelParty())[seat];
                        require(profile.has_value() == (match.context().owners[seat] != 0),
                                "Travel omitted a party seat");
                        if (profile) {
                            require(profile->gold == static_cast<s32>(epoch * 100 + seat),
                                    "Travel lost updated gold");
                        }
                    }
                }
                require(match.loaded(), "Online scene-ready failed");
                loadedEpoch = epoch;
                nextTick = now;
            }
        }
        if (match.phase() == MatchSession::Phase::Paused && host) {
            paused = true;
            require(match.prepare(2, MatchTransition::Resume), "Online resume failed");
        }
        if (match.phase() == MatchSession::Phase::Running) {
            if (!host) {
                if (const auto shown = presentation.sample(match, elapsed)) {
                    require(shown->valid(), "Interpolated online scene became invalid");
                    const auto* latest = match.playback().latest();
                    require(latest != nullptr && presentation.tick() <= latest->motion.tick,
                            "Online presentation ran beyond the host");
                    if (presentation.fraction() > 0.01 && presentation.fraction() < 0.99) {
                        ++interpolated;
                    }
                }
            }
            if (startupMs == 0) {
                startupMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count();
            }
            if (const auto* state = match.playback().latest();
                state && (sampledEpoch != state->motion.epoch - firstEpoch + 1 ||
                          sampledTick != state->motion.tick)) {
                sampledEpoch = state->motion.epoch - firstEpoch + 1;
                sampledTick = state->motion.tick;
                ++snapshots;
                if (sampledEpoch == 3) {
                    thirdEpochTick = sampledTick;
                    if (sampledTick >= 90) {
                        for (usize seat = 0; seat < 4; ++seat) {
                            const auto& motion = state->motion.players[seat];
                            require(motion.has_value() == (match.context().owners[seat] != 0),
                                    "Online snapshot populated an unowned seat");
                            require(!motion || motion->position.x > 0.5f,
                                    "An online seat never moved");
                        }
                    }
                }
            }
            if (host && epoch == 4 && !leaving) {
                session.leave();
                leaving = true;
            } else if (epoch < 4 && now >= nextTick && now >= stallUntil) {
                std::vector<InputCommand> commands;
                for (const auto seat : session.seats()) {
                    InputCommand input;
                    input.epoch = match.context().epoch;
                    input.grant = match.context().grants[seat];
                    input.seat = seat;
                    input.tick = match.inputTick();
                    input.direction = {1, 0};
                    input.magnitude = 1;
                    commands.push_back(input);
                }
                require(match.sample(commands), "Online input sample failed");
                ++inputs;
                if (host) {
                    const auto frame = match.advance();
                    require(frame.has_value(), "Online host did not advance");
                    for (usize seat = 0; seat < 4; ++seat) {
                        travel[seat] += (*frame)[seat].magnitude / 60;
                    }
                    if (match.tick() % 3 == 0) {
                        require(match.publish(checkpoint(match, travel)),
                                "Online snapshot publication failed");
                    }
                    if (epoch == 1 && match.tick() == 30) {
                        auto profiles = *session.party();
                        for (usize seat = 0; seat < profiles.size(); ++seat) {
                            if (profiles[seat]) {
                                profiles[seat]->gold = static_cast<s32>(200 + seat);
                            }
                        }
                        require(match.prepare(2, MatchTransition::Travel, &profiles),
                                "Online travel failed");
                    } else if (epoch == 2 && match.tick() == 30) {
                        require(match.requestPause(), "Online pause failed");
                    } else if (epoch == 3 && match.tick() == 60 && !stalled) {
                        // Keep pumping room/transport while the host simulation
                        // stalls, as during an expensive frame or focus change.
                        // Guests must not run beyond the host's input window.
                        stalled = true;
                        stallUntil = now + std::chrono::seconds(3);
                    } else if (epoch == 3 && match.tick() == 180) {
                        auto profiles = *session.party();
                        for (usize seat = 0; seat < profiles.size(); ++seat) {
                            if (profiles[seat]) {
                                profiles[seat]->gold = static_cast<s32>(400 + seat);
                            }
                        }
                        require(match.prepare(3, MatchTransition::Travel, &profiles),
                                "Online final barrier failed");
                    }
                }
                nextTick = now + std::chrono::microseconds(16667);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Online session integration timed out");
}
