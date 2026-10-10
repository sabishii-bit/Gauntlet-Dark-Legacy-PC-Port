#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/CombatReplica.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = CombatReplica::Admission;

CombatSnapshot state(u64 tick = 0, usize count = 25) {
    CombatSnapshot result;
    result.motion.epoch = 9;
    result.motion.tick = tick;
    result.motion.cameraContinuity = 1;
    for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
        result.motion.players[seat] = SeatMotion{1, 1, {static_cast<f32>(seat), 0, 0}, 0};
        result.players[seat] = PlayerCombatState{
            1000.5f, ReplicaPlayerLife::Standing, false, true, {12, 7, 1, 3.5f, 0.5f}};
    }
    for (usize i = 0; i < count; ++i) {
        EnemyCombatState enemy;
        enemy.instance = i + 1;
        enemy.health = 45;
        enemy.fullHealth = 50;
        enemy.position = {static_cast<f32>(i), 2, -3};
        enemy.animation = {2, 1, 2, 4.5f, 1};
        result.enemies.push_back(enemy);
    }
    return result;
}
std::vector<u8> encode(const CombatSnapshot& snapshot) {
    const auto bytes = CombatPacket::encode(snapshot);
    REQUIRE(bytes);
    return *bytes;
}
CombatReplica::Packets packets(const CombatSnapshot& snapshot) {
    const auto result = CombatReplica::packets(snapshot, SnapshotBlock::Compression::None);
    REQUIRE(result);
    return *result;
}
void writeWord(std::vector<u8>& bytes, usize offset, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
void commit(CombatReplica& replica, const CombatSnapshot& snapshot) {
    const auto chunks = packets(snapshot);
    for (usize i = 0; i < chunks.size(); ++i) {
        CHECK(replica.receive(1, chunks[i]) ==
              (i + 1 == chunks.size() ? Admission::Committed : Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(encode(*replica.latest()) == encode(snapshot));
}

TEST_CASE("combat payload round trips every seat mask and maximum enemy roster",
          "[netplay][combat-snapshot]") {
    for (u32 mask = 0; mask < 16; ++mask) {
        auto snapshot = state(0x0123456789ABCDEFULL, CombatSnapshot::kMaxEnemies);
        usize seats = 0;
        for (usize seat = 0; seat < InputCommand::kSeats; ++seat) {
            if ((mask & (1U << seat)) == 0) {
                snapshot.players[seat].reset();
                snapshot.motion.players[seat].reset();
            } else {
                ++seats;
            }
        }
        const auto bytes = encode(snapshot);
        CHECK(bytes.size() == CombatPacket::kHeaderBytes + MotionPacket::kHeaderBytes +
                                  seats * (CombatPacket::kPlayerBytes + MotionPacket::kSeatBytes) +
                                  snapshot.enemies.size() * CombatPacket::kEnemyBytes);
        CHECK(bytes.size() <= CombatPacket::kMaxBytes);
        CHECK(bytes[0] == 'G');
        CHECK(bytes[3] == 'B');
        CHECK(bytes[4] == 16);
        CHECK(bytes[6] == 128);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
        const auto chunks = packets(snapshot);
        for (const auto& chunk : chunks) {
            CHECK(chunk.size() <= PacketTransport::kMaxPacketBytes);
        }
    }
}

TEST_CASE("combat codecs reject incomplete nonfinite inconsistent and unbounded state",
          "[netplay][combat-snapshot]") {
    const auto valid = encode(state());
    for (usize size = 0; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    auto bytes = valid;
    bytes.push_back(0);
    CHECK_FALSE(CombatPacket::decode(bytes));
    constexpr usize kPlayer = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes;
    constexpr usize kEnemy = kPlayer + 4 * CombatPacket::kPlayerBytes;
    for (const usize offset : std::array<usize, 6>{0, 4, 6, 8, kPlayer + 4, kEnemy + 20}) {
        bytes = valid;
        bytes[offset] = 255;
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const usize offset :
         {kPlayer, kPlayer + 24, kPlayer + 28, kEnemy + 24, kEnemy + 28, kEnemy + 32, kEnemy + 36,
          kEnemy + 40, kEnemy + 44, kEnemy + 64, kEnemy + 68}) {
        bytes = valid;
        writeWord(bytes, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const u32 flags : {3U, 16U, 0x80000000U}) {
        bytes = valid;
        writeWord(bytes, kPlayer + 4, flags);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = state();
    invalid.players[0].reset();
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[1].instance = invalid.enemies[0].instance;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    std::swap(invalid.enemies[0], invalid.enemies[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].tier = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].animation.generation = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = state();
    invalid.enemies[0].health = -1;
    CHECK_FALSE(CombatReplica::packets(invalid));
    CHECK_FALSE(CombatPacket::encode(state(0, CombatSnapshot::kMaxEnemies + 1)));
}

TEST_CASE("companion checkpoints preserve both slots and reject malformed or ownerless meshes",
          "[netplay][combat-snapshot][replica-companions]") {
    auto source = state(5, 0);
    for (u32 form = 1; form <= 7; ++form) {
        for (auto& player : source.players) {
            CompanionState earned;
            earned.form = 2;
            earned.placement[3] = {1, 2, 3, 1};
            earned.animation = {0, 1, 0x123456789ULL, 7.5f, 1};
            earned.textureClock = 35.5f;
            earned.alpha = 0.4f;
            player->companions[0] = earned;
            earned.form = form;
            player->companions[1] = earned;
        }
        const auto bytes = encode(source);
        const auto decoded = CombatPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(encode(*decoded) == bytes);
    }
    constexpr usize kFirst = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 32;
    const auto valid = encode(source);
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 10>{{{kFirst, 3},
                                                {kFirst + CombatPacket::kCompanionBytes, 8},
                                                {kFirst + 4, 0x7FC00000},
                                                {kFirst + 40, 0x7F800000},
                                                {kFirst + 52, 1},
                                                {kFirst + 56, 65536},
                                                {kFirst + 68, 0xBF800000},
                                                {kFirst + 72, 0},
                                                {kFirst + 76, 0x7FC00000},
                                                {kFirst + 80, 0x40000000}}}) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.players[0]->life = ReplicaPlayerLife::InTower;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.players[0]->companions[0]->animation.generation = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
    auto absent = encode(state(5, 0));
    // An absent slot is canonical zero padding, not a second hidden payload.
    writeWord(absent, kFirst + 4, 1);
    CHECK_FALSE(CombatPacket::decode(absent));
}

TEST_CASE("pickup records preserve affine placements and reject malformed rosters",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    source.pickups.push_back(PickupState{.instance = 100,
                                         .resource = 4,
                                         .animation = {0, 3, 10, 2.5f, 1},
                                         .textureFrame = 43.5f,
                                         .alpha = 0.5f});
    source.pickups[0].placement = glm::scale(
        glm::rotate(glm::translate(Mat4{1}, Vec3{4, 5, 6}), 0.7f, Vec3{0, 1, 0}), Vec3{1, 2, 3});
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kPickup =
        CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 4 * CombatPacket::kPlayerBytes;
    CHECK(bytes.size() == kPickup + CombatPacket::kPickupBytes);
    for (usize length = kPickup; length < bytes.size(); ++length) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(length)));
    }
    for (const usize offset : {usize{20}, kPickup + 8, kPickup + 64}) {
        auto bad = bytes;
        writeWord(bad, offset, 0xFFFFFFFF);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (const usize offset : {kPickup, kPickup + 8, kPickup + 12}) {
        auto bad = bytes;
        writeWord(bad, offset, 0);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (usize offset = kPickup + 16; offset < kPickup + 64; offset += 4) {
        auto bad = bytes;
        writeWord(bad, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    for (const usize offset : {kPickup + 80, kPickup + 84, kPickup + 88, kPickup + 92}) {
        auto bad = bytes;
        writeWord(bad, offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bad));
    }
    auto bad = source;
    bad.pickups[0].placement[0].w = 1;
    CHECK_FALSE(CombatPacket::encode(bad));
    bad = source;
    bad.pickups.push_back(bad.pickups[0]);
    CHECK_FALSE(CombatPacket::encode(bad));
    bad.pickups.back().instance = 99;
    CHECK_FALSE(CombatPacket::encode(bad));
}

TEST_CASE("fixture codecs preserve independent cursors and reject malformed visible rosters",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    for (u32 kind = 0; kind <= static_cast<u32>(FixtureSource::SecretPortal); ++kind) {
        FixtureState fixture;
        fixture.source = static_cast<FixtureSource>(kind);
        fixture.instance = 0x123456789ABCDEF0ULL;
        fixture.resource = kind + 1;
        fixture.pose = {0, 1, 7, 9.5f, 1};
        fixture.meshSequence = 2;
        fixture.textureSequence = 3;
        fixture.textureFrame = 4;
        fixture.textureClock = 66.5f;
        fixture.cameraFacing = true;
        source.fixtures.push_back(fixture);
    }
    const auto bytes = encode(source);
    const auto decoded = CombatPacket::decode(bytes);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == bytes);
    constexpr usize kFixture =
        CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes + 4 * CombatPacket::kPlayerBytes;
    CHECK(bytes.size() == kFixture + 12 * CombatPacket::kFixtureBytes);
    for (usize length = kFixture; length < bytes.size(); ++length) {
        CHECK_FALSE(CombatPacket::decode(std::span(bytes).first(length)));
    }
    for (const auto [offset, value] : std::array<std::pair<usize, u32>, 9>{
             {{24, static_cast<u32>(CombatSnapshot::kMaxFixtures + 1)},
              {kFixture, 12},
              {kFixture + 12, 0},
              {kFixture + 16, 0},
              {kFixture + 68, 1},
              {kFixture + 92, 65536},
              {kFixture + 100, 65536},
              {kFixture + 112, 0x40000000},
              {kFixture + 116, 2}}}) {
        auto invalid = bytes;
        writeWord(invalid, offset, value);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (usize offset = 20; offset < 68; offset += 4) {
        auto invalid = bytes;
        writeWord(invalid, kFixture + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    for (const usize offset : {84U, 88U, 96U, 104U, 108U, 112U}) {
        auto invalid = bytes;
        writeWord(invalid, kFixture + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(invalid));
    }
    auto invalid = source;
    invalid.fixtures[1] = invalid.fixtures[0];
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    std::swap(invalid.fixtures[0], invalid.fixtures[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.fixtures[0].instance = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("combat chunks commit atomically despite reordering and duplicates",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    commit(replica, state(1, 0));
    auto next = state(2, CombatSnapshot::kMaxEnemies);
    next.geometry = SceneGeometry{1, static_cast<u32>(SceneGeometry::kMaxStates), 0, {}};
    for (usize i = 0; i < SceneGeometry::kMaxStates; ++i) {
        next.geometry->objects.push_back({static_cast<u32>(i)});
    }
    for (usize i = 0; i < CombatSnapshot::kMaxProjectiles; ++i) {
        ProjectileState shot;
        shot.instance = i + 1;
        shot.resource = 1;
        next.projectiles.push_back(shot);
    }
    for (usize i = 0; i < CombatSnapshot::kMaxPickups; ++i) {
        PickupState item;
        item.instance = i + 1;
        item.resource = 1;
        next.pickups.push_back(item);
    }
    for (usize i = 0; i < CombatSnapshot::kMaxFixtures; ++i) {
        FixtureState fixture;
        fixture.instance = i + 1;
        fixture.resource = 1;
        next.fixtures.push_back(fixture);
    }
    for (usize i = 0; i < FighterPacket::kMaxMeshes; ++i) {
        FighterMeshState mesh;
        mesh.incarnation = mesh.resource = 1;
        mesh.part = static_cast<u32>(i + 1);
        mesh.nodes.resize(FighterPacket::kMaxNodes / FighterPacket::kMaxMeshes);
        next.fighters.push_back(std::move(mesh));
    }
    next.hud = HudSnapshot{};
    for (auto& player : next.hud->players) {
        player = HudPlayer{};
    }
    next.hud->cards.resize(HudSnapshot::kMaxCards);
    for (auto& count : next.hud->counts) {
        count = HudCount{};
    }
    next.hud->bossBars.resize(HudSnapshot::kMaxBossBars);
    CHECK(encode(next).size() == CombatPacket::kMaxBytes);
    auto chunks = packets(next);
    REQUIRE(chunks.size() == CombatReplica::kMaxChunks);
    REQUIRE(chunks.size() > 200);
    std::ranges::reverse(chunks);
    for (usize i = 0; i + 1 < chunks.size(); ++i) {
        CHECK(replica.receive(1, chunks[i]) == Admission::Pending);
        CHECK(replica.receive(1, chunks[i]) == Admission::Duplicate);
        REQUIRE(replica.latest());
        CHECK(replica.latest()->motion.tick == 1);
        CHECK(replica.latest()->enemies.empty());
    }
    CHECK(replica.receive(1, chunks.back()) == Admission::Committed);
    REQUIRE(replica.latest());
    CHECK(encode(*replica.latest()) == encode(next));
    CHECK(replica.pending() == 0);
    CHECK(replica.receive(1, chunks.back()) == Admission::Stale);
    CombatReplica repaired;
    REQUIRE(repaired.begin(1, 9));
    const auto repair = CombatReplica::packets(next, SnapshotBlock::Compression::None,
                                               CombatReplica::Recovery::SingleLoss);
    REQUIRE(repair);
    REQUIRE(repair->size() > 255);
    for (usize i = 0; i < repair->size(); ++i) {
        if (i < chunks.size() && i % CombatReplica::kRepairGroup == 0) {
            continue;
        }
        CHECK(repaired.receive(1, (*repair)[i]) != Admission::Invalid);
    }
    REQUIRE(repaired.latest());
    CHECK(encode(*repaired.latest()) == encode(next));
}

TEST_CASE("projectile records round trip their resource namespaces and reject corrupt transforms",
          "[netplay][combat-snapshot]") {
    auto source = state(3, 0);
    for (u32 kind = 0; kind <= static_cast<u32>(ProjectileSource::Arrival); ++kind) {
        ProjectileState shot;
        shot.source = static_cast<ProjectileSource>(kind);
        shot.instance = 0x123456789ABCDEF0ULL;
        shot.resource = kind + 1;
        shot.placement = glm::rotate(glm::translate(Mat4{1}, Vec3{1, 2, 3}), 0.5f, Vec3{0, 1, 0});
        shot.direction = {3, 5, 7};
        shot.age = 0.75f;
        shot.radius = 1.5f;
        shot.forward = -0.5f;
        shot.tint = Color{0x12, 0x34, 0x56, 0x78};
        shot.animation = {0, 1, 7, 2.5f, 1};
        shot.textureFrame = 50.5f;
        shot.alpha = 0.75f;
        shot.flags = 31;
        source.projectiles.push_back(shot);
    }
    const auto valid = encode(source);
    const auto decoded = CombatPacket::decode(valid);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == valid);
    constexpr usize kShot = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                            InputCommand::kSeats * CombatPacket::kPlayerBytes;
    for (usize size = kShot; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    for (const auto [offset, value] :
         std::array<std::pair<usize, u32>, 10>{{{4, 1},
                                                {12, 129},
                                                {kShot, 6},
                                                {kShot + 12, 0},
                                                {kShot + 16, 0},
                                                {kShot + 144, 32},
                                                {kShot + 32, 0x3F800000},
                                                {kShot + 80, 0},
                                                {kShot + 16, 65536},
                                                {kShot, 256}}}) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    for (const usize offset : {20U, 68U, 84U, 88U, 92U, 96U, 100U, 104U, 128U, 132U, 136U, 140U}) {
        auto bytes = valid;
        writeWord(bytes, kShot + offset, 0x7FC00000);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.projectiles[1] = invalid.projectiles[0];
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    std::swap(invalid.projectiles[0], invalid.projectiles[1]);
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.projectiles[0].instance = 0;
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("geometry codecs reject malformed lengths indices flags and matrices",
          "[netplay][combat-snapshot][geometry-replica]") {
    auto source = state(3, 0);
    source.geometry = SceneGeometry{0x123456789ABCDEF0ULL, 5, 0.25f, {{0}, {3}}};
    source.geometry->objects[0].local =
        glm::rotate(glm::translate(Mat4{1}, Vec3{2, 3, 4}), 0.5f, Vec3{0, 1, 0});
    source.geometry->objects[1].alpha = 0.5f;
    source.geometry->objects[1].visible = false;
    const auto valid = encode(source);
    const auto decoded = CombatPacket::decode(valid);
    REQUIRE(decoded);
    CHECK(encode(*decoded) == valid);
    constexpr usize kGeometry = CombatPacket::kHeaderBytes + MotionPacket::kMaxBytes +
                                InputCommand::kSeats * CombatPacket::kPlayerBytes;
    constexpr usize kObject = kGeometry + CombatPacket::kGeometryHeaderBytes;
    for (usize size = kGeometry; size < valid.size(); ++size) {
        CHECK_FALSE(CombatPacket::decode(std::span(valid).first(size)));
    }
    const std::array<std::pair<usize, u32>, 13> corruptions{
        {{16, 1},
         {16, 0xFFFFFFFF},
         {kGeometry + 8, 0},
         {kGeometry + 8, 65536},
         {kGeometry + 12, 2049},
         {kGeometry + 12, 1},
         {kGeometry + 16, 0x7FC00000},
         {kObject, 5},
         {kObject + 4, 0},
         {kObject + 8, 0x7F800000},
         {kObject + 56, 0x40000000},
         {kObject + 60, 2},
         {kObject + CombatPacket::kGeometryObjectBytes, 0}}};
    for (const auto [offset, value] : corruptions) {
        auto bytes = valid;
        writeWord(bytes, offset, value);
        CHECK_FALSE(CombatPacket::decode(bytes));
    }
    auto invalid = source;
    invalid.geometry->objects[0].local[0].w = 1;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.geometry->objects[0].local[3].x = 1'000'001;
    CHECK_FALSE(CombatPacket::encode(invalid));
    invalid = source;
    invalid.geometry->objects.resize(SceneGeometry::kMaxStates + 1);
    CHECK_FALSE(CombatPacket::encode(invalid));
}

TEST_CASE("lost combat fragments cannot resurrect a removed or recycled enemy",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    commit(replica, state(1));
    auto dying = state(2);
    dying.enemies[0].health = 0;
    dying.enemies[0].life = ReplicaEnemyLife::Dying;
    const auto missing = packets(dying);
    REQUIRE(missing.size() > 1);
    CHECK(replica.receive(1, missing[0]) == Admission::Pending);
    auto replaced = state(3);
    replaced.enemies.erase(replaced.enemies.begin());
    auto born = replaced.enemies.back();
    ++born.instance;
    replaced.enemies.push_back(born);
    replaced.players[0]->health = 0;
    replaced.players[0]->life = ReplicaPlayerLife::Dying;
    commit(replica, replaced);
    for (const auto& chunk : missing) {
        CHECK(replica.receive(1, chunk) == Admission::Stale);
    }
    CHECK(replica.latest()->enemies.front().instance == 2);
    CHECK(replica.latest()->enemies.back().instance == 26);
    CHECK(replica.latest()->players[0]->health == 0);
}

TEST_CASE("combat fragment validation and host epochs bound partial state",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    const auto chunks = packets(state(1));
    CHECK(replica.receive(1, chunks[0]) == Admission::WrongHost);
    CHECK_FALSE(replica.begin(0, 9));
    CHECK_FALSE(replica.begin(1, 0));
    REQUIRE(replica.begin(1, 9));
    CHECK(replica.receive(2, chunks[0]) == Admission::WrongHost);
    for (usize size = 0; size < chunks[0].size(); ++size) {
        CHECK(replica.receive(1, std::span(chunks[0]).first(size)) == Admission::Invalid);
    }
    for (const usize offset : {0U, 4U, 5U, 6U, 7U}) {
        auto bad = chunks[0];
        bad[offset] = 255;
        CHECK(replica.receive(1, bad) == Admission::Invalid);
    }
    auto oversized = chunks[0];
    writeWord(oversized, 24, static_cast<u32>(SnapshotBlock::kMaxBytes + 1));
    CHECK(replica.receive(1, oversized) == Admission::Invalid);
    for (u64 tick = 1; tick <= 10; ++tick) {
        CHECK(replica.receive(1, packets(state(tick))[0]) == Admission::Pending);
        CHECK(replica.pending() <= CombatReplica::kPendingSnapshots);
    }
    CHECK(replica.receive(1, chunks[0]) == Admission::Stale);
    CHECK_FALSE(replica.latest());
    CHECK_FALSE(replica.begin(2, 10));
    CHECK_FALSE(replica.begin(1, 9));
    REQUIRE(replica.begin(1, 10));
    CHECK(replica.pending() == 0);
    CHECK(replica.receive(1, chunks[0]) == Admission::WrongEpoch);
    replica.clear();
    CHECK_FALSE(replica.latest());
    REQUIRE(replica.begin(2, 1));
}

TEST_CASE("different captures of one tick and corrupted chunks cannot be combined",
          "[netplay][combat-snapshot]") {
    CombatReplica replica;
    REQUIRE(replica.begin(1, 9));
    auto snapshot = state(1);
    const auto first = packets(snapshot);
    snapshot.players[0]->health -= 20;
    const auto other = packets(snapshot);
    CHECK(replica.receive(1, first[0]) == Admission::Pending);
    CHECK(replica.receive(1, other[1]) == Admission::Invalid);
    auto damaged = first[0];
    damaged.back() ^= 1;
    CHECK(replica.receive(1, damaged) == Admission::Invalid);
    for (usize i = 1; i < first.size(); ++i) {
        CHECK(replica.receive(1, first[i]) ==
              (i + 1 == first.size() ? Admission::Committed : Admission::Pending));
    }
    auto corrupt = packets(state(2));
    corrupt[0].back() ^= 1; // valid-sized payload, invalid whole-state digest
    for (usize i = 0; i < corrupt.size(); ++i) {
        CHECK(replica.receive(1, corrupt[i]) ==
              (i + 1 == corrupt.size() ? Admission::Invalid : Admission::Pending));
    }
    REQUIRE(replica.latest());
    CHECK(replica.latest()->motion.tick == 1);
    commit(replica, state(2)); // clean retransmission recovers
}
} // namespace
