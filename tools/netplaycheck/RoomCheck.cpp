#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "engine/net/GnsTransport.h"

#include "SessionCheck.h"
#include "game/netplay/InputCommand.h"
#include "game/netplay/MatchSession.h"
#include "game/netplay/RoomClient.h"
#include "game/netplay/RoomSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Clock = std::chrono::steady_clock;
using Connection = PacketTransport::Connection;
using Delivery = PacketTransport::Delivery;
using EventType = PacketTransport::EventType;
constexpr u64 kTicks = 120;
constexpr u8 kFinished = 254;
constexpr u8 kAcknowledged = 253;

void require(bool condition, const std::string& error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}
void report(const nlohmann::json& value) {
    std::cout << value.dump() << '\n';
    std::cout.flush();
}
const RoomMember& member(const RoomSnapshot& room, const std::string& peer) {
    const auto found =
        std::ranges::find_if(room.members, [&](const auto& m) { return m.peer == peer; });
    require(found != room.members.end(), "Peer missing from admitted roster");
    return *found;
}
u8 matchPeer(const RoomMember& member) {
    require(!member.seats.empty(), "Admitted member has no seats");
    return static_cast<u8>(*std::ranges::min_element(member.seats) + 1);
}
void control(GnsTransport& transport, Connection connection, u8 value) {
    require(transport.send(connection, std::span(&value, 1), Delivery::Reliable) ==
                PacketTransport::SendResult::Sent,
            "Control queue failed");
}

// Deterministic transport fixture, not a second implementation of combat. The
// gameplay capture tests separately drive PlayerHealth and the real enemy pool.
CombatSnapshot combatFor(const MotionSnapshot& motion) {
    CombatSnapshot result;
    result.motion = motion;
    result.hud = HudSnapshot{};
    result.hud->screen.cinematicBars = motion.tick >= 40 && motion.tick < 60;
    result.hud->screen.gameOver = motion.tick >= 110;
    result.hud->screen.gameOverLetters =
        motion.tick >= 110 ? static_cast<u32>(motion.tick - 110) : 0;
    result.hud->screen.transitionOpacity =
        motion.tick < 40 ? 1 - static_cast<f32>(motion.tick) / 40 : 0;
    result.hud->screen.titleScale = motion.tick < 40 ? static_cast<f32>(motion.tick + 1) / 20 : 0;
    result.hud->visible = !result.hud->screen.cinematicBars && !result.hud->screen.gameOver;
    if (motion.tick < 90) {
        result.hud->hourglass =
            HudHourglass{static_cast<f32>(motion.tick) / 90,
                         motion.tick < 60 ? static_cast<s32>(motion.tick % 4) : -1};
        result.hud->runeFill = static_cast<f32>(motion.tick) / 90;
        result.hud->scroll =
            HudScroll{2, motion.tick < 40 ? 0U : 1U,
                      motion.tick >= 60 ? static_cast<s32>((motion.tick - 60) / 2) : -1,
                      static_cast<u32>(motion.tick % 256)};
        result.hud->help = HudHelp{34, 0, 20, 5, false, Vec3{1, 2, 3}};
    }
    if (motion.tick < 90) {
        result.hud->cards = {{0, HudCardKind::Magic, 384 - static_cast<s32>(motion.tick % 80)},
                             {3, HudCardKind::Legend, 384 - static_cast<s32>(motion.tick % 80)}};
        result.hud->counts[0] = HudCount{HudCountKind::Orange, static_cast<s32>(motion.tick), 250};
        result.hud->counts[3] =
            HudCount{HudCountKind::Minotaur, static_cast<s32>(motion.tick), 100};
    }
    for (s32 head = 0; head < 3; ++head) {
        result.hud->bossBars.push_back({motion.tick < 110,
                                        motion.tick < 30,
                                        {256, 256 - head * 20 - static_cast<s32>(motion.tick)}});
    }
    // Match the largest mutable-node census (806 in I1), without requiring game
    // assets in transport CI. Platforms move and visibility changes each run.
    result.geometry = SceneGeometry{1, 806, 0, {}};
    for (u32 i = 0; i < 806; ++i) {
        const u32 row = i / 31;
        const Vec3 position{static_cast<f32>(i % 31), static_cast<f32>(motion.tick) / 60,
                            static_cast<f32>(row)};
        result.geometry->objects.push_back({i, 1, glm::translate(Mat4{1}, position),
                                            motion.tick < 60 ? 1.0f : 0.5f,
                                            i != motion.tick % 806});
    }
    for (usize seat = 0; seat < motion.players.size(); ++seat) {
        if (const auto& owner = motion.players[seat]) {
            PlayerCombatState player;
            player.health = motion.tick < 30 ? 1000.0f : 750.0f;
            player.damageable = true;
            player.animation = {12, 7, 1 + motion.tick / 30, static_cast<f32>(motion.tick % 30), 1};
            for (usize slot = 0; slot < player.companions.size(); ++slot) {
                // Toggle a timed companion off and back on; another expires for good.
                if (slot == 1 && ((seat == 1 && motion.tick >= 45 && motion.tick < 75) ||
                                  (seat == 2 && motion.tick >= 90))) {
                    continue;
                }
                CompanionState companion;
                companion.form = slot == 0 ? 2 : 3;
                companion.animation = {0, 1, 1 + motion.tick / 30,
                                       static_cast<f32>(motion.tick % 30), 1};
                companion.placement[3] =
                    Vec4{owner->position + Vec3{slot == 0 ? -3.0f : 3.0f, 8, 0}, 1};
                companion.textureClock = static_cast<f32>(motion.tick) / 2;
                companion.alpha = slot == 1 && seat == 2 && motion.tick >= 60 ? 0.5f : 1;
                player.companions[slot] = companion;
            }
            if (seat == 3 && motion.tick >= 90) {
                player.health = 0;
                player.life = ReplicaPlayerLife::Dying;
                player.damageable = false;
            }
            result.players[seat] = player;
            HudPlayer hud;
            hud.name = "NET";
            hud.character = static_cast<s32>(seat);
            hud.color = static_cast<s32>(seat);
            hud.level = 60;
            hud.health = static_cast<s32>(player.health);
            hud.gold = 100 + static_cast<s32>(motion.tick);
            hud.keys = hud.potions = 9 - static_cast<s32>(motion.tick / 30);
            hud.potionKind = 4;
            hud.runes = 1U << static_cast<u32>(seat + motion.tick / 30);
            hud.bossKeys = 0xFF;
            hud.keysShown = motion.tick < 90;
            if (player.life == ReplicaPlayerLife::Standing) {
                hud.turbo = HudTurbo{static_cast<f32>(motion.tick % 60) / 60,
                                     {255, 0, 0, 255},
                                     {255, 255, 0, 255},
                                     0,
                                     -1};
                hud.usage = HudPowerup{60 - static_cast<f32>(motion.tick) / 60, 9, 0, 0x80, true};
            }
            if (seat == 1 && motion.tick >= 20 && motion.tick < 80) {
                hud.selection = HudPowerup{-1, 5, 17, 0x100000, motion.tick < 60};
                hud.labelY = 291;
            }
            result.hud->players[seat] = hud;
        }
    }
    for (u64 id = 1; id <= 26; ++id) {
        if ((id == 1 && motion.tick >= 60) || (id == 26 && motion.tick < 90)) {
            continue;
        }
        EnemyCombatState enemy;
        enemy.instance = id;
        enemy.health = 50;
        enemy.fullHealth = 50;
        enemy.position = {static_cast<f32>(id), 0, static_cast<f32>(motion.tick) / 60};
        enemy.animation = {2, 1, 1 + motion.tick / 15, static_cast<f32>(motion.tick % 15), 1};
        if (id == 1 && motion.tick >= 30) {
            enemy.health = 0;
            enemy.life = ReplicaEnemyLife::Dying;
        }
        result.enemies.push_back(enemy);
    }
    // Spawn, reflect, expire and replace all visual namespaces under real packet
    // loss. The final reliable checkpoint must contain exactly the current shots.
    const auto phase = motion.tick % 24;
    if (phase < 20) {
        for (u32 kind = 0; kind < 5; ++kind) {
            ProjectileState shot;
            shot.source = static_cast<ProjectileSource>(kind);
            shot.instance = 1 + motion.tick / 24;
            shot.resource = kind + 1;
            shot.continuity = phase < 10 ? 1U : 2U;
            shot.placement[3] = Vec4{static_cast<f32>(kind), 3,
                                     static_cast<f32>(phase < 10 ? phase : 20 - phase), 1};
            shot.direction = {0, 0, phase < 10 ? 20.0f : -20.0f};
            shot.age = static_cast<f32>(phase) / 60;
            shot.animation = {0, 1, 1 + motion.tick / 6, static_cast<f32>(motion.tick % 6), 1};
            result.projectiles.push_back(shot);
        }
    }
    // Pickup identity survives placement/resource changes. Collected items stay
    // absent; newly dropped loot does not reuse a removed instance's identity.
    for (u64 id = 1; id <= 41; ++id) {
        if ((id == 1 && motion.tick >= 60) || (id == 41 && motion.tick < 90)) {
            continue;
        }
        PickupState item;
        item.instance = id;
        item.resource = id == 2 && motion.tick >= 30 ? 2U : 1U;
        item.placement[3] = Vec4{static_cast<f32>(id), static_cast<f32>(motion.tick) / 60, 0, 1};
        item.textureFrame = static_cast<f32>(motion.tick) / 2;
        result.pickups.push_back(item);
    }
    for (u32 kind = 0; kind <= static_cast<u32>(FixtureSource::Statue); ++kind) {
        for (u64 id = 1; id <= 8; ++id) {
            const auto source = static_cast<FixtureSource>(kind);
            if (((source == FixtureSource::Switch || source == FixtureSource::Barrel) && id == 1 &&
                 motion.tick >= 60) ||
                (source == FixtureSource::Rubble && motion.tick < 60) ||
                (source == FixtureSource::Statue && id == 1 && motion.tick >= 60 &&
                 motion.tick < 90)) {
                continue;
            }
            FixtureState fixture;
            fixture.source = source;
            // A waking statue disappears without renumbering its neighbours;
            // another spawn must not reuse that disappeared instance's pose.
            fixture.instance =
                source == FixtureSource::Statue && id == 1 && motion.tick >= 90 ? 9 : id;
            fixture.resource = kind == 4 ? 5U + static_cast<u32>(motion.tick / 40) : kind + 1;
            fixture.placement[3] =
                Vec4{static_cast<f32>(id), -static_cast<f32>(motion.tick) / 60, 0, 1};
            fixture.pose = {0, 1, 1, std::min(10.0f, static_cast<f32>(motion.tick) / 2), 1};
            fixture.meshSequence = motion.tick < 20 ? 1 : 2;
            fixture.meshFrame = motion.tick < 20 ? fixture.pose.frame : 0;
            fixture.textureSequence = 1;
            fixture.textureFrame = fixture.pose.frame;
            fixture.textureClock = static_cast<f32>(motion.tick) / 2;
            result.fixtures.push_back(fixture);
        }
    }
    std::ranges::sort(result.fixtures, {}, &FixtureState::key);
    // Exercise articulated bodies, independent branches, broken-part appearance
    // and recycled population slots over the same impaired room connection.
    for (u32 actor = 0; actor <= 16; ++actor) {
        if (actor == 16 && motion.tick >= 60 && motion.tick < 90) {
            continue;
        }
        FighterMeshState body;
        body.actor = actor;
        body.incarnation = actor == 16 && motion.tick >= 90 ? 2 : 1;
        body.part = body.resource = 1;
        body.placement[3] = Vec4{static_cast<f32>(actor), 0, static_cast<f32>(motion.tick) / 60, 1};
        body.frame = static_cast<f32>(motion.tick % 30);
        body.textureClock = static_cast<f32>(motion.tick) / 2;
        body.flags = motion.tick % 10 < 3 ? FighterMeshState::kFlash : 0;
        for (u32 node = 0; node < 24; ++node) {
            FighterMeshState::Node joint;
            joint.transform =
                glm::rotate(Mat4{1}, static_cast<f32>(motion.tick + node) / 60, Vec3{0, 1, 0});
            joint.generation = 1 + motion.tick / (node < 12 ? 30 : 20);
            joint.frame = static_cast<f32>(motion.tick % (node < 12 ? 30 : 20));
            joint.alpha = node == 4 && actor == 0 && motion.tick >= 60 ? 0.0f : 1.0f;
            body.nodes.push_back(joint);
        }
        result.fighters.push_back(body);
        if (actor == 0 && motion.tick >= 60) {
            FighterMeshState broken;
            broken.incarnation = 1;
            broken.part = broken.resource = 2;
            broken.placement = body.placement * body.nodes[4].transform;
            result.fighters.push_back(broken);
        }
    }
    return result;
}
void rejectResponse(const std::string& endpoint) {
    for (const std::string invalid :
         {"http://example.org", "http://localhost:1234", "https://user:password@example.org",
          "https://example.org/path", "https://example.org?token=x", "file:///tmp/rooms"}) {
        bool rejected = false;
        try {
            const RoomClient client(invalid);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "Unsafe coordinator endpoint accepted");
    }
    RoomClient client(endpoint);
    bool rejected = false;
    try {
        client.enter("", 1, "network-test-1", std::string(64, 'a'));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && client.peer().empty(), "Malformed reply committed a room identity");
    report({{"event", "passed"}, {"role", "room-contract"}});
}

void workerTest(const std::string& endpoint) {
    RoomSession session(endpoint, "", 1, "network-test-1", std::string(64, 'a'));
    const GnsTransport::Signal signal{std::string(32, 'a'), {1}};
    require(!session.send({signal.peer, {}}), "Empty signal queued");
    require(!session.send({signal.peer, std::vector<u8>(GnsTransport::kMaxSignalBytes + 1)}),
            "Oversized signal queued");
    // The fixture delays admission for one second. These calls cannot wait for
    // that response and the queues must reject overflow instead of growing.
    for (usize count = 0; count < 64; ++count) {
        require(session.send(signal), "Signaling queue filled early");
    }
    require(!session.send(signal), "Unbounded signaling queue");
    for (usize count = 0; count < 8; ++count) {
        require(session.ready(1), "Control queue filled early");
    }
    require(!session.start(1), "Unbounded control queue");
    usize polls = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const auto update = session.poll();
        if (update.closed) {
            require(!update.error.empty() && !update.room,
                    "Malformed admission silently succeeded");
            require(polls >= 25, "Room service blocked the polling thread");
            require(!session.send(signal) && !session.ready(1), "Closed worker accepts messages");
            session.leave();
            report({{"event", "passed"}, {"role", "room-worker"}, {"polls_during_request", polls}});
            return;
        }
        ++polls;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Room worker did not report failed admission");
}

void run(bool host, const std::string& endpoint, const std::string& code, u8 localPlayers,
         usize clients, GnsTransport::Simulation simulation) {
    const auto began = Clock::now();
    // Deliberately synthetic compatibility identity: this is not a real game's
    // asset manifest or version handshake. Production must compute those inputs.
    RoomSession coordinator(endpoint, code, localPlayers, "network-test-1", std::string(64, 'a'));
    RoomSnapshot room;
    std::string localPeer;
    const auto admissionDeadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < admissionDeadline) {
        auto update = coordinator.poll();
        require(update.error.empty(), update.error);
        if (update.room) {
            room = std::move(*update.room);
            localPeer = std::move(update.peer);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!localPeer.empty(), "Room admission timed out");
    std::string error;
    auto transport = GnsTransport::create(simulation, error);
    require(transport != nullptr, error);
    require(transport->configurePeer(localPeer, host), "P2P configuration failed");
    require(!transport->configurePeer(localPeer, host), "Identity changed mid-session");
    require(!transport->listenLoopback(), "P2P adapter exposed raw IP listener");
    require(!transport->connectPeer(std::string(32, 'f')), "Unadmitted peer connected");
    require(!transport->receiveSignal({std::string(32, 'f'), {1}}), "Unadmitted signal accepted");
    if (host) {
        report({{"event", "room_created"}, {"code", room.code}});
    }
    struct Peer {
        std::string identity;
        std::vector<u8> seats;
        std::array<std::bitset<kTicks + InputTimeline::kMaxAhead + 1>, 4> accepted;
        bool finished = false;
        bool closed = false;
        bool acknowledged = false;
        std::optional<CombatReplica::Packets> finalPackets;
        usize finalNext = 0;
    };
    MatchSession match;
    std::map<Connection, Peer> peers;
    std::optional<Connection> server;
    const auto& combat = match.playback();
    // A synthetic dense authoritative fixture. Real collision and the shared
    // gameplay camera are covered by NetplayMotionTests; this process harness
    // checks bidirectional state transport without loading graphics/assets.
    MotionSnapshot motion;
    motion.cameraContinuity = 1;
    motion.camera.pitch = 0.5f;
    for (auto& player : motion.players) {
        player = SeatMotion{1, 1, {0, 0, 0}, 0};
    }
    u64 snapshotsReceived = 0;
    u64 projectileSnapshots = 0;
    u64 pickupSnapshots = 0;
    u64 fixtureSnapshots = 0;
    u64 fighterSnapshots = 0;
    u64 companionSnapshots = 0;
    usize companionsShown = 0;
    u64 hudSnapshots = 0;
    u64 hudOverlaySnapshots = 0;
    u32 screenStates = 0;
    usize hudPlayers = 0;
    std::optional<Clock::time_point> nextSimulation;
    bool connected = false;
    bool finished = false;
    bool acknowledged = false;
    bool leaving = false;
    bool simulationStarted = false;
    s64 startupMs = 0;
    bool loaded = false;
    usize loadBarrierChecks = 0;
    std::optional<u64> readyRevision;
    std::optional<u64> startRevision;
    std::optional<Clock::time_point> loadAt;
    u64 tick = 0;
    auto now = Clock::now();
    auto nextTick = now;
    auto nextProgress = now + std::chrono::seconds(3);
    auto closeAt = now;
    auto lastUpdate = now;
    const auto deadline = now + std::chrono::seconds(30);
    while ((now = Clock::now()) < deadline) {
        match.update(std::chrono::duration<f64>(now - lastUpdate).count());
        lastUpdate = now;
        require(match.phase() != MatchSession::Phase::Stopped, "Match lifecycle stopped early");
        if (now >= nextProgress) {
            report({{"event", "progress"},
                    {"host", host},
                    {"started", room.started},
                    {"connected", host ? peers.size() : static_cast<usize>(connected)},
                    {"tick", host ? match.tick() : tick}});
            nextProgress = now + std::chrono::seconds(3);
        }
        auto update = coordinator.poll();
        require(update.error.empty(), update.error);
        if (update.closed) {
            require(leaving, "Room worker stopped early");
            report({{"event", "passed"},
                    {"role", host ? "room-host" : "room-client"},
                    {"ticks", tick},
                    {"simulation_ticks", match.tick()},
                    {"startup_ms", startupMs},
                    {"match_epoch", match.context().epoch},
                    {"load_barrier_checks", loadBarrierChecks},
                    {"snapshots_received", snapshotsReceived},
                    {"combat_tick", combat.latest() ? combat.latest()->motion.tick : 0},
                    {"combat_enemies", combat.latest() ? combat.latest()->enemies.size() : 0},
                    {"geometry_objects", combat.latest() && combat.latest()->geometry
                                             ? combat.latest()->geometry->objects.size()
                                             : 0},
                    {"projectile_snapshots", projectileSnapshots},
                    {"pickup_snapshots", pickupSnapshots},
                    {"combat_pickups", combat.latest() ? combat.latest()->pickups.size() : 0},
                    {"fixture_snapshots", fixtureSnapshots},
                    {"combat_fixtures", combat.latest() ? combat.latest()->fixtures.size() : 0},
                    {"fighter_snapshots", fighterSnapshots},
                    {"companion_snapshots", companionSnapshots},
                    {"combat_companions", companionsShown},
                    {"hud_snapshots", hudSnapshots},
                    {"hud_overlay_snapshots", hudOverlaySnapshots},
                    {"screen_states", screenStates},
                    {"hud_players", hudPlayers},
                    {"combat_fighters", combat.latest() ? combat.latest()->fighters.size() : 0},
                    {"combat_projectiles",
                     combat.latest() ? combat.latest()->projectiles.size() : 0}});
            return;
        }
        if (update.room) {
            const bool justStarted = !room.started && update.room->started;
            room = std::move(*update.room);
            if (host && justStarted) {
                report({{"event", "started"}, {"players", 4}, {"computers", clients + 1}});
            }
            std::vector<std::string> allowed;
            for (const auto& m : room.members) {
                if (m.peer != localPeer && (host || m.peer == room.host)) {
                    allowed.push_back(m.peer);
                }
            }
            require(transport->authorizePeers(allowed), "Invalid room authorization roster");
            if (!host && !server) {
                server = transport->connectPeer(room.host);
                require(server.has_value(), "Could not initiate ICE connection");
                require(!transport->connectPeer(room.host), "Duplicate peer connection admitted");
            }
            for (const auto& signal : room.signals) {
                auto mislabeled = signal;
                for (const auto& other : allowed) {
                    if (other != signal.peer) {
                        mislabeled.peer = other;
                        require(!transport->receiveSignal(mislabeled),
                                "Signaling sender spoof accepted");
                    }
                }
                auto duplicateIdentity = signal;
                duplicateIdentity.bytes.insert(duplicateIdentity.bytes.end(), {0x42, 0});
                require(!transport->receiveSignal(duplicateIdentity),
                        "Duplicate identity accepted");
                // Stale cleanup signals may legitimately refer to a closed link.
                transport->receiveSignal(signal);
            }
        }
        for (const auto& signal : transport->takeSignals()) {
            // A departed peer's cleanup signaling is no longer routable.
            if (std::ranges::any_of(room.members,
                                    [&](const auto& m) { return m.peer == signal.peer; })) {
                coordinator.send(signal); // Bounded/best effort. GNS retries if congested.
            }
        }
        if (host && room.started && match.phase() == MatchSession::Phase::Lobby) {
            require(match.prepare(1), "Match preparation failed");
        }
        for (const auto& event : transport->poll()) {
            if (event.type == EventType::Connected) {
                const auto identity = transport->peer(event.connection);
                require(identity.has_value(), "Connection lacks room identity");
                if (host) {
                    const auto& admitted = member(room, *identity);
                    Peer peer;
                    peer.identity = *identity;
                    peer.seats = admitted.seats;
                    peers.emplace(event.connection, std::move(peer));
                } else {
                    require(server == event.connection && *identity == room.host,
                            "Connection is not the admitted host");
                    connected = true;
                }
                continue;
            }
            if (host) {
                const auto found = peers.find(event.connection);
                require(found != peers.end(), "Peer failed before connecting: " + event.reason);
                auto& peer = found->second;
                if (event.type == EventType::Disconnected) {
                    require(peer.acknowledged,
                            "Peer disconnected before final state: " + event.reason);
                    // Test-only successful shutdown after final convergence. A
                    // real disconnect is delivered to MatchSession::disconnected;
                    // its fail-closed behavior is covered by lifecycle tests.
                    peer.closed = true;
                } else if (event.bytes.size() == 1 && event.bytes[0] == kFinished) {
                    require(!peer.finished, "Duplicate finish");
                    peer.finished = true;
                    for (const auto seat : peer.seats) {
                        const auto count = peer.accepted[seat].count();
                        require(count >= kTicks / 2, "Too few admitted input ticks");
                        report({{"event", "seat_done"}, {"seat", seat}, {"accepted", count}});
                    }
                } else {
                    const auto admission = match.receive(event.connection, event.bytes);
                    require(admission == MatchSession::Admission::Accepted ||
                                admission == MatchSession::Admission::Stale,
                            "Host match packet rejected");
                    const auto commands = InputPacket::decode(event.bytes);
                    if (commands) {
                        require(simulationStarted, "Input sent before simulation start");
                        for (const auto& command : *commands) {
                            require(std::ranges::find(peer.seats, command.seat) !=
                                            peer.seats.end() &&
                                        command.held(CommandHeld::Attack) &&
                                        command.tick < peer.accepted[command.seat].size(),
                                    "Seat spoof or corrupted input");
                            peer.accepted.at(command.seat).set(static_cast<usize>(command.tick));
                        }
                        if (!nextSimulation) {
                            // Fixed fixture playout lead, not a production clock-sync
                            // policy. The host then advances independently of delivery.
                            nextSimulation = now + std::chrono::milliseconds(300);
                        }
                    }
                }
            } else {
                require(server == event.connection, "Wrong connection");
                require(event.type != EventType::Disconnected || acknowledged,
                        "Host disconnected early: " + event.reason);
                if (event.type == EventType::Message) {
                    const bool final = !event.bytes.empty() && event.bytes[0] == kAcknowledged;
                    const auto payload = std::span(event.bytes).subspan(final ? 1 : 0);
                    if (!final) {
                        const auto previous = combat.latest()
                                                  ? std::optional{combat.latest()->motion.tick}
                                                  : std::nullopt;
                        const auto admission = match.receive(event.connection, payload);
                        require(admission == MatchSession::Admission::Accepted ||
                                    admission == MatchSession::Admission::Stale,
                                "Client match packet rejected: admission=" +
                                    std::to_string(static_cast<u8>(admission)) +
                                    " phase=" + std::to_string(static_cast<u8>(match.phase())) +
                                    " size=" + std::to_string(payload.size()));
                        if (combat.latest() &&
                            (!previous || combat.latest()->motion.tick > *previous)) {
                            const auto* received = combat.latest();
                            require(received != nullptr && received->hud.has_value(),
                                    "Missing committed HUD state");
                            hudPlayers = static_cast<usize>(
                                std::ranges::count_if(received->hud->players, [](const auto& slot) {
                                    return slot.has_value();
                                }));
                            ++hudSnapshots;
                            const auto& screen = received->hud->screen;
                            screenStates |= (screen.transitionOpacity > 0 ? 1U : 0U) |
                                            (screen.titleScale > 0 ? 2U : 0U) |
                                            (screen.cinematicBars ? 4U : 0U) |
                                            (screen.gameOver ? 8U : 0U);
                            if (!received->hud->cards.empty() && received->hud->counts[0] &&
                                received->hud->counts[3] && received->hud->bossBars.size() == 3 &&
                                received->hud->hourglass && received->hud->runeFill &&
                                received->hud->scroll && received->hud->help) {
                                ++hudOverlaySnapshots;
                            }
                            companionsShown = 0;
                            for (const auto& player : received->players) {
                                if (player) {
                                    for (const auto& companion : player->companions) {
                                        companionsShown += companion ? 1 : 0;
                                    }
                                }
                            }
                            companionSnapshots += companionsShown > 0 ? 1 : 0;
                            require(received != nullptr, "Missing committed combat state");
                            require(CombatPacket::encode(*received) ==
                                        CombatPacket::encode(combatFor(received->motion)),
                                    "Live combat presentation state diverged");
                            if (!received->projectiles.empty()) {
                                ++projectileSnapshots;
                            }
                            if (!received->pickups.empty()) {
                                ++pickupSnapshots;
                            }
                            if (!received->fixtures.empty()) {
                                ++fixtureSnapshots;
                            }
                            if (!received->fighters.empty()) {
                                ++fighterSnapshots;
                            }
                            const auto displayed = combat.sample(received->motion.tick);
                            require(displayed.has_value() && CombatPacket::encode(*displayed) ==
                                                                 CombatPacket::encode(*received),
                                    "Combat display is not aligned with committed host state");
                            ++snapshotsReceived;
                        }
                    }
                    if (final) {
                        report({{"event", "live_state_summary"},
                                {"snapshots", snapshotsReceived},
                                {"projectiles", projectileSnapshots},
                                {"fighters", fighterSnapshots},
                                {"companions", companionSnapshots},
                                {"hud", hudSnapshots},
                                {"overlays", hudOverlaySnapshots},
                                {"players", hudPlayers},
                                {"screen_states", screenStates}});
                        require(finished && snapshotsReceived >= 10 && projectileSnapshots >= 5 &&
                                    fighterSnapshots >= 5 && companionSnapshots >= 5 &&
                                    hudSnapshots >= 5 && hudOverlaySnapshots >= 5 &&
                                    hudPlayers == 4 && screenStates == 15,
                                "Final arrived without live state replication");
                        const auto shown = combat.sample(kTicks + 100);
                        require(shown && shown->motion.tick == kTicks - 1,
                                "Incomplete final state");
                        const auto encoded = MotionPacket::encode(shown->motion);
                        require(encoded && std::ranges::equal(*encoded, payload),
                                "Client did not converge to final host state");
                        require(combat.latest() != nullptr &&
                                    CombatPacket::encode(*combat.latest()) ==
                                        CombatPacket::encode(combatFor(shown->motion)),
                                "Client health, animation or enemy lifecycle diverged");
                        for (const auto& player : shown->motion.players) {
                            require(player && player->position.x >= 0.5f,
                                    "A seat never moved in authoritative simulation");
                        }
                        acknowledged = true;
                        transport->close(*server);
                        closeAt = now + std::chrono::milliseconds(500);
                    }
                }
            }
        }
        // Freeze the complete roster before marking room-ready. That guarantees
        // every guest has its lifecycle router before host Prepare can arrive.
        usize occupied = 0;
        MatchOwners owners{};
        for (const auto& m : room.members) {
            occupied += m.seats.size();
            for (const auto seat : m.seats) {
                owners[seat] = matchPeer(m);
            }
        }
        if (match.phase() == MatchSession::Phase::Offline && occupied == 4 &&
            (host ? peers.size() == clients : connected)) {
            std::vector<MatchLink> links;
            if (host) {
                for (const auto& [connection, peer] : peers) {
                    links.push_back({matchPeer(member(room, peer.identity)), connection});
                }
            } else {
                links.push_back({1, *server});
            }
            require(match.open(matchPeer(member(room, localPeer)), owners, links),
                    "Frozen match roster rejected");
        }
        if (!room.started && match.phase() == MatchSession::Phase::Lobby &&
            !member(room, localPeer).ready && readyRevision != room.revision &&
            coordinator.ready(room.revision)) {
            readyRevision = room.revision;
        }
        if (host && !room.started && match.phase() == MatchSession::Phase::Lobby &&
            startRevision != room.revision &&
            std::ranges::all_of(room.members, [](const auto& m) { return m.ready; }) &&
            coordinator.start(room.revision)) {
            startRevision = room.revision;
        }
        if (match.phase() == MatchSession::Phase::Loading) {
            if (!loadAt) {
                loadAt = now + std::chrono::milliseconds(host ? 0 : 100);
            }
            require(!match.advance(), "Simulation advanced while a machine was loading");
            require(match.tick() == 0, "Loading consumed simulation ticks");
            ++loadBarrierChecks;
            if (!loaded && now >= *loadAt) {
                require(match.loaded(), "Local scene-ready rejected");
                loaded = true;
            }
        }
        if (match.phase() == MatchSession::Phase::Running && !simulationStarted) {
            require(loaded && loadBarrierChecks > 0, "Started without loading barrier");
            simulationStarted = true;
            startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count();
            motion.epoch = match.context().epoch;
            nextTick = now;
        }
        if (!host && connected && simulationStarted && tick < kTicks && now >= nextTick) {
            std::vector<InputCommand> inputs;
            for (const auto seat : member(room, localPeer).seats) {
                InputCommand command;
                command.epoch = match.context().epoch;
                command.grant = match.context().grants[seat];
                command.tick = match.inputTick();
                command.seat = seat;
                command.direction = {1, 0};
                command.magnitude = 1;
                command.heldButtons = static_cast<u32>(CommandHeld::Attack);
                inputs.push_back(command);
            }
            require(match.sample(inputs), "Could not sample local inputs");
            ++tick;
            nextTick = now + std::chrono::microseconds(16667);
        }
        if (!host && tick == kTicks && !finished) {
            control(*transport, *server, kFinished);
            finished = true;
        }
        if (host && nextSimulation && now >= *nextSimulation && match.tick() < kTicks) {
            std::vector<InputCommand> local;
            for (const auto seat : member(room, localPeer).seats) {
                InputCommand input;
                input.epoch = match.context().epoch;
                input.tick = match.inputTick();
                input.grant = match.context().grants[seat];
                input.seat = seat;
                input.direction = {1, 0};
                input.magnitude = 1;
                local.push_back(input);
            }
            require(match.sample(local), "Host local sampling failed");
            const auto frame = match.advance();
            require(frame.has_value(), "Running host did not advance");
            const auto& inputs = *frame;
            Vec3 middle{0};
            for (usize seat = 0; seat < inputs.size(); ++seat) {
                auto& player = *motion.players[seat];
                const auto& input = inputs[seat];
                const Vec2 velocity = input.direction * input.magnitude;
                player.position += Vec3{velocity.x, 0, velocity.y} / 60.0f;
                middle += player.position * 0.25f;
            }
            motion.tick = match.tick() - 1;
            motion.camera.position = middle + Vec3{0, 12, -20};
            if (motion.tick % 3 == 0 || match.tick() == kTicks) {
                if (motion.tick == 0 || match.tick() == kTicks) {
                    const auto packets = CombatReplica::packets(
                        combatFor(motion), SnapshotBlock::Compression::Automatic,
                        CombatReplica::Recovery::SingleLoss);
                    require(packets.has_value(), "Checkpoint metrics failed");
                    usize bytes = 0;
                    for (const auto& packet : *packets) {
                        bytes += packet.size();
                    }
                    // 20 Hz dense scenes must fit below GNS's default 256 KiB/s
                    // pacing rate, including our fragment and recovery overhead.
                    require(bytes <= usize{12} * 1024,
                            "Dense checkpoint exceeded live bandwidth budget");
                    report({{"event", "checkpoint_size"},
                            {"tick", motion.tick},
                            {"packets", packets->size()},
                            {"bytes", bytes}});
                }
                require(match.publish(combatFor(motion)), "Host state publication failed");
            }
            nextSimulation = now + std::chrono::microseconds(16667);
        }
        match.flush(*transport);
        if (host && match.tick() == kTicks) {
            for (auto& [connection, peer] : peers) {
                if (peer.finished && !peer.acknowledged) {
                    // Reliable final checkpoint is test control, not a reliable
                    // backlog of old motion. Ordinary states use unreliable delivery.
                    if (!peer.finalPackets) {
                        peer.finalPackets = CombatReplica::packets(combatFor(motion));
                        require(peer.finalPackets.has_value(), "Could not encode final combat");
                        auto final = MotionPacket::encode(motion);
                        require(final.has_value(), "Missing final host state");
                        final->insert(final->begin(), kAcknowledged);
                        peer.finalPackets->push_back(std::move(*final));
                    }
                    // Reliable delivery can back up behind the last live state.
                    // Keep one bounded batch/cursor and keep pumping the network;
                    // restarting this batch would duplicate already accepted chunks.
                    while (peer.finalNext < peer.finalPackets->size()) {
                        const auto sent = transport->send(
                            connection, (*peer.finalPackets)[peer.finalNext], Delivery::Reliable);
                        if (sent == PacketTransport::SendResult::Congested) {
                            break;
                        }
                        require(sent == PacketTransport::SendResult::Sent,
                                "Final state queue failed");
                        ++peer.finalNext;
                    }
                    peer.acknowledged = peer.finalNext == peer.finalPackets->size();
                }
            }
        }
        if (host && peers.size() == clients &&
            std::ranges::all_of(peers, [](const auto& row) { return row.second.closed; })) {
            // Guests still need time to flush cleanup signaling and leave the room.
            if (room.members.size() == 1 && !leaving) {
                coordinator.leave();
                leaving = true;
            }
        }
        if (!host && acknowledged && now >= closeAt && !leaving) {
            coordinator.leave();
            leaving = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Room/ICE test timed out");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::span args(argv, static_cast<usize>(argc));
        if (argc == 3 && std::string(args[1]) == "reject-response") {
            rejectResponse(args[2]);
            return 0;
        }
        if (argc == 3 && std::string(args[1]) == "worker-test") {
            workerTest(args[2]);
            return 0;
        }
        require(argc == 7 || (argc == 8 && std::string(args[7]) == "--trace"),
                "Usage: roomcheck host ENDPOINT LOCAL_PLAYERS CLIENTS LAG LOSS [--trace] | "
                "client ENDPOINT LOCAL_PLAYERS CODE LAG LOSS [--trace] | "
                "session-host ENDPOINT LOCAL_PLAYERS TOTAL_PLAYERS LAG LOSS [--trace] | "
                "session-client ENDPOINT LOCAL_PLAYERS CODE LAG LOSS [--trace]");
        const std::string role = args[1];
        if (role == "session-host" || role == "session-client") {
            const bool host = role == "session-host";
            const auto players = std::stoi(args[3]);
            const auto total = host ? std::stoi(args[4]) : 0;
            require(players >= 1 && players <= 3 && (!host || (total > players && total <= 4)),
                    "Invalid online session roster");
            checkOnlineSession(host, args[2], host ? "" : args[4], static_cast<u8>(players),
                               static_cast<usize>(total),
                               {std::stoi(args[5]), std::stof(args[6]), argc == 8});
            return 0;
        }
        require(role == "host" || role == "client", "Invalid role");
        const auto players = std::stoi(args[3]);
        const bool host = role == "host";
        const auto clients = host ? std::stoi(args[4]) : 0;
        require(players >= 1 && players <= 3 && (!host || (clients >= 1 && clients <= 3)),
                "Invalid roster size");
        run(host, args[2], host ? "" : args[4], static_cast<u8>(players),
            static_cast<usize>(clients), {std::stoi(args[5]), std::stof(args[6]), argc == 8});
        return 0;
    } catch (const std::exception& error) {
        std::fputs("Room test failed: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
