#include "game/netplay/CombatSnapshot.h"

#include <bit>
#include <cmath>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLB");
constexpr u32 kVersion = 18;
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32U));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
void real(std::vector<u8>& bytes, f32 value) {
    word(bytes, std::bit_cast<u32>(value));
}
f32 real(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}
bool bounded(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool placementValid(const Mat4& placement) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            const f32 limit = column == 3 ? 1'000'000.0f : 1024.0f;
            if (!bounded(placement[column][row], -limit, limit)) {
                return false;
            }
        }
    }
    return placement[0].w == 0 && placement[1].w == 0 && placement[2].w == 0 && placement[3].w == 1;
}
void animation(std::vector<u8>& bytes, const CombatAnimation& value) {
    word(bytes, value.action);
    word(bytes, value.sequence);
    wide(bytes, value.generation);
    real(bytes, value.frame);
    real(bytes, value.transition);
}
CombatAnimation animation(ByteReader& reader) {
    CombatAnimation value;
    value.action = reader.readU32();
    value.sequence = reader.readU32();
    value.generation = wide(reader);
    value.frame = real(reader);
    value.transition = real(reader);
    return value;
}
} // namespace

bool CombatAnimation::valid() const {
    return action <= 255 && sequence <= 65535 && bounded(frame, 0, 1'000'000) &&
           bounded(transition, 0, 1) &&
           (generation != 0 || (action == 0 && sequence == 0 && frame == 0 && transition == 1));
}

bool ProjectileState::valid() const {
    if (source > ProjectileSource::Arrival || instance == 0 || continuity == 0 || resource == 0 ||
        resource > 65535 || (flags & ~31U) != 0 || !animation.valid() ||
        !bounded(age, 0, 1'000'000) || !bounded(radius, 0, 1024) ||
        !bounded(forward, -1024, 1024) || !bounded(textureFrame, 0, 1'000'000'000.0f) ||
        !bounded(alpha, 0, 1)) {
        return false;
    }
    return placementValid(placement) && bounded(direction.x, -10000, 10000) &&
           bounded(direction.y, -10000, 10000) && bounded(direction.z, -10000, 10000);
}

bool PickupState::valid() const {
    return instance != 0 && resource != 0 && resource <= 65535 && continuity != 0 &&
           placementValid(placement) && animation.valid() && animation.action == 0 &&
           animation.transition == 1 && bounded(textureFrame, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1);
}

bool FixtureState::valid() const {
    return source <= FixtureSource::SecretPortal && instance != 0 && resource != 0 &&
           resource <= 65535 && continuity != 0 && placementValid(placement) && pose.valid() &&
           pose.action == 0 && pose.transition == 1 && meshSequence <= 65535 &&
           textureSequence <= 65535 && bounded(meshFrame, 0, 1'000'000) &&
           bounded(textureFrame, 0, 1'000'000) && bounded(textureClock, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1);
}

bool CompanionState::valid(usize slot) const {
    return slot < 2 && form >= 1 && form <= (slot == 0 ? 2U : 7U) && placementValid(placement) &&
           animation.valid() && animation.generation != 0 && animation.action == 0 &&
           animation.transition == 1 && bounded(textureClock, 0, 1'000'000'000.0f) &&
           bounded(alpha, 0, 1);
}

bool PlayerShadowState::valid() const {
    const f32 length = glm::dot(normal, normal);
    return bounded(ground.x, -1'000'000, 1'000'000) && bounded(ground.y, -1'000'000, 1'000'000) &&
           bounded(ground.z, -1'000'000, 1'000'000) && bounded(normal.x, -1, 1) &&
           bounded(normal.y, 0.5f, 1) && bounded(normal.z, -1, 1) &&
           bounded(length, 0.999f, 1.001f) && bounded(alpha, 0, 1);
}

bool CombatSnapshot::valid() const {
    if (!motion.valid() || enemies.size() > kMaxEnemies || projectiles.size() > kMaxProjectiles ||
        pickups.size() > kMaxPickups || fixtures.size() > kMaxFixtures ||
        (geometry && !geometry->valid()) || !FighterPacket::valid(fighters) ||
        (hud && !hud->valid())) {
        return false;
    }
    for (usize seat = 0; seat < players.size(); ++seat) {
        const auto& player = players[seat];
        if (hud && hud->players[seat].has_value() != player.has_value()) {
            return false;
        }
        if (player.has_value() != motion.players[seat].has_value() ||
            (player && (!bounded(player->health, 0, 1'000'000) || !player->animation.valid() ||
                        static_cast<u8>(player->life) > 2))) {
            return false;
        }
        if (player) {
            if (player->shadow &&
                (!player->shadow->valid() || player->life == ReplicaPlayerLife::InTower ||
                 (player->portalPhase && *player->portalPhase >= 1))) {
                return false;
            }
            if (player->portalPhase && (player->life != ReplicaPlayerLife::Standing ||
                                        !bounded(*player->portalPhase, 0, 1))) {
                return false;
            }
            for (usize slot = 0; slot < player->companions.size(); ++slot) {
                const auto& companion = player->companions[slot];
                if (companion &&
                    (!companion->valid(slot) || player->life == ReplicaPlayerLife::InTower)) {
                    return false;
                }
            }
        }
    }
    u64 previous = 0;
    for (const auto& enemy : enemies) {
        if (enemy.instance <= previous || enemy.kind > 65535 || enemy.tier < 1 || enemy.tier > 3 ||
            enemy.variant > 255 || static_cast<u8>(enemy.life) > 2 ||
            !bounded(enemy.health, 0, 1'000'000) || !bounded(enemy.fullHealth, 0, 1'000'000) ||
            !bounded(enemy.position.x, -1'000'000, 1'000'000) ||
            !bounded(enemy.position.y, -1'000'000, 1'000'000) ||
            !bounded(enemy.position.z, -1'000'000, 1'000'000) ||
            !bounded(enemy.yaw, -kTwoPi, kTwoPi) || !enemy.animation.valid()) {
            return false;
        }
        previous = enemy.instance;
    }
    for (usize i = 0; i < projectiles.size(); ++i) {
        if (!projectiles[i].valid() ||
            (i > 0 && projectiles[i - 1].key() >= projectiles[i].key())) {
            return false;
        }
    }
    previous = 0;
    for (const auto& item : pickups) {
        if (!item.valid() || item.instance <= previous) {
            return false;
        }
        previous = item.instance;
    }
    for (usize i = 0; i < fixtures.size(); ++i) {
        if (!fixtures[i].valid() || (i > 0 && fixtures[i - 1].key() >= fixtures[i].key())) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<u8>> CombatPacket::encode(const CombatSnapshot& snapshot) {
    if (!snapshot.valid()) {
        return std::nullopt;
    }
    const auto motion = MotionPacket::encode(snapshot.motion);
    if (!motion) {
        return std::nullopt;
    }
    const auto fighters = FighterPacket::encode(snapshot.fighters);
    if (!fighters) {
        return std::nullopt;
    }
    const usize fighterBytes = snapshot.fighters.empty() ? 0 : fighters->size();
    const auto hud =
        snapshot.hud ? HudPacket::encode(*snapshot.hud) : std::optional{std::vector<u8>{}};
    if (!hud) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    const usize geometryBytes =
        snapshot.geometry
            ? kGeometryHeaderBytes + snapshot.geometry->objects.size() * kGeometryObjectBytes
            : 0;
    bytes.reserve(
        kHeaderBytes + motion->size() + InputCommand::kSeats * kPlayerBytes +
        snapshot.enemies.size() * kEnemyBytes + snapshot.projectiles.size() * kProjectileBytes +
        snapshot.pickups.size() * kPickupBytes + snapshot.fixtures.size() * kFixtureBytes +
        geometryBytes + fighterBytes + hud->size());
    word(bytes, kMagic);
    word(bytes, kVersion | (static_cast<u32>(snapshot.enemies.size()) << 16U));
    word(bytes, static_cast<u32>(motion->size()));
    word(bytes, static_cast<u32>(snapshot.projectiles.size()));
    word(bytes, static_cast<u32>(geometryBytes));
    word(bytes, static_cast<u32>(snapshot.pickups.size()));
    word(bytes, static_cast<u32>(snapshot.fixtures.size()));
    word(bytes, static_cast<u32>(fighterBytes));
    word(bytes, static_cast<u32>(hud->size()));
    bytes.insert(bytes.end(), motion->begin(), motion->end());
    for (const auto& player : snapshot.players) {
        if (player) {
            real(bytes, player->health);
            word(bytes, static_cast<u32>(player->life) | (player->hitFlash ? 4U : 0U) |
                            (player->damageable ? 8U : 0U) | (player->portalPhase ? 16U : 0U) |
                            (player->shadow ? 32U : 0U));
            animation(bytes, player->animation);
            real(bytes, player->portalPhase.value_or(0));
            for (const auto& companion : player->companions) {
                if (!companion) {
                    bytes.insert(bytes.end(), kCompanionBytes, 0);
                    continue;
                }
                word(bytes, companion->form);
                for (s32 column = 0; column < 4; ++column) {
                    for (s32 row = 0; row < 3; ++row) {
                        real(bytes, companion->placement[column][row]);
                    }
                }
                animation(bytes, companion->animation);
                real(bytes, companion->textureClock);
                real(bytes, companion->alpha);
            }
            if (player->shadow) {
                const auto& shadow = *player->shadow;
                for (const f32 value :
                     {shadow.ground.x, shadow.ground.y, shadow.ground.z, shadow.normal.x,
                      shadow.normal.y, shadow.normal.z, shadow.alpha}) {
                    real(bytes, value);
                }
            } else {
                bytes.insert(bytes.end(), kShadowBytes, 0);
            }
        }
    }
    for (const auto& enemy : snapshot.enemies) {
        wide(bytes, enemy.instance);
        word(bytes, enemy.kind);
        word(bytes, enemy.tier);
        word(bytes, enemy.variant);
        word(bytes, static_cast<u32>(enemy.life) | (enemy.hitFlash ? 4U : 0U));
        for (const f32 value : {enemy.health, enemy.fullHealth, enemy.position.x, enemy.position.y,
                                enemy.position.z, enemy.yaw}) {
            real(bytes, value);
        }
        animation(bytes, enemy.animation);
    }
    for (const auto& shot : snapshot.projectiles) {
        word(bytes, static_cast<u32>(shot.source));
        wide(bytes, shot.instance);
        word(bytes, shot.continuity);
        word(bytes, shot.resource);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                real(bytes, shot.placement[column][row]);
            }
        }
        for (const f32 value : {shot.direction.x, shot.direction.y, shot.direction.z, shot.age,
                                shot.radius, shot.forward}) {
            real(bytes, value);
        }
        word(bytes, u32{shot.tint.r} | (u32{shot.tint.g} << 8U) | (u32{shot.tint.b} << 16U) |
                        (u32{shot.tint.a} << 24U));
        animation(bytes, shot.animation);
        real(bytes, shot.textureFrame);
        real(bytes, shot.alpha);
        word(bytes, shot.flags);
    }
    for (const auto& item : snapshot.pickups) {
        wide(bytes, item.instance);
        word(bytes, item.resource);
        word(bytes, item.continuity);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                real(bytes, item.placement[column][row]);
            }
        }
        animation(bytes, item.animation);
        real(bytes, item.textureFrame);
        real(bytes, item.alpha);
    }
    for (const auto& fixture : snapshot.fixtures) {
        word(bytes, static_cast<u32>(fixture.source));
        wide(bytes, fixture.instance);
        word(bytes, fixture.resource);
        word(bytes, fixture.continuity);
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                real(bytes, fixture.placement[column][row]);
            }
        }
        animation(bytes, fixture.pose);
        word(bytes, fixture.meshSequence);
        real(bytes, fixture.meshFrame);
        word(bytes, fixture.textureSequence);
        real(bytes, fixture.textureFrame);
        real(bytes, fixture.textureClock);
        real(bytes, fixture.alpha);
        word(bytes, fixture.cameraFacing ? 1 : 0);
    }
    if (snapshot.geometry) {
        const auto& geometry = *snapshot.geometry;
        wide(bytes, geometry.layout);
        word(bytes, geometry.objectCount);
        word(bytes, static_cast<u32>(geometry.objects.size()));
        real(bytes, geometry.darken);
        for (const auto& object : geometry.objects) {
            word(bytes, object.index);
            word(bytes, object.continuity);
            // Affine bottom row is implicit: 0, 0, 0, 1. No matrix memory copies.
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 3; ++row) {
                    real(bytes, object.local[column][row]);
                }
            }
            real(bytes, object.alpha);
            word(bytes, object.visible ? 1 : 0);
        }
    }
    if (fighterBytes != 0) {
        bytes.insert(bytes.end(), fighters->begin(), fighters->end());
    }
    bytes.insert(bytes.end(), hud->begin(), hud->end());
    return bytes;
}

std::optional<CombatSnapshot> CombatPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU16() != kVersion) {
        return std::nullopt;
    }
    const usize enemies = reader.readU16();
    const usize motionBytes = reader.readU32();
    const usize projectiles = reader.readU32();
    const usize geometryBytes = reader.readU32();
    const usize pickups = reader.readU32();
    const usize fixtures = reader.readU32();
    const usize fighterBytes = reader.readU32();
    const usize hudBytes = reader.readU32();
    if (enemies > CombatSnapshot::kMaxEnemies || projectiles > CombatSnapshot::kMaxProjectiles ||
        pickups > CombatSnapshot::kMaxPickups || fixtures > CombatSnapshot::kMaxFixtures ||
        motionBytes > reader.remaining() || fighterBytes > FighterPacket::kMaxBytes ||
        hudBytes > HudPacket::kMaxBytes ||
        (geometryBytes != 0 && (geometryBytes < kGeometryHeaderBytes ||
                                geometryBytes > kGeometryHeaderBytes + SceneGeometry::kMaxStates *
                                                                           kGeometryObjectBytes))) {
        return std::nullopt;
    }
    const auto motion = MotionPacket::decode(bytes.subspan(kHeaderBytes, motionBytes));
    if (!motion) {
        return std::nullopt;
    }
    CombatSnapshot result;
    result.motion = *motion;
    usize players = 0;
    for (const auto& player : motion->players) {
        players += player ? 1 : 0;
    }
    if (bytes.size() != kHeaderBytes + motionBytes + players * kPlayerBytes +
                            enemies * kEnemyBytes + projectiles * kProjectileBytes +
                            pickups * kPickupBytes + fixtures * kFixtureBytes + geometryBytes +
                            fighterBytes + hudBytes) {
        return std::nullopt;
    }
    ByteReader data(bytes.subspan(kHeaderBytes + motionBytes));
    for (usize seat = 0; seat < result.players.size(); ++seat) {
        if (motion->players[seat]) {
            PlayerCombatState player;
            player.health = real(data);
            const u32 flags = data.readU32();
            if ((flags & ~63U) != 0) {
                return std::nullopt;
            }
            player.life = static_cast<ReplicaPlayerLife>(flags & 3U);
            player.hitFlash = (flags & 4U) != 0;
            player.damageable = (flags & 8U) != 0;
            player.animation = animation(data);
            const f32 portalPhase = real(data);
            if ((flags & 16U) != 0) {
                player.portalPhase = portalPhase;
            } else if (portalPhase != 0) {
                return std::nullopt;
            }
            for (auto& companion : player.companions) {
                CompanionState value;
                value.form = data.readU32();
                if (value.form == 0) {
                    for (usize wordIndex = 1; wordIndex < kCompanionBytes / 4; ++wordIndex) {
                        if (data.readU32() != 0) {
                            return std::nullopt;
                        }
                    }
                    continue;
                }
                for (s32 column = 0; column < 4; ++column) {
                    for (s32 row = 0; row < 3; ++row) {
                        value.placement[column][row] = real(data);
                    }
                }
                value.animation = animation(data);
                value.textureClock = real(data);
                value.alpha = real(data);
                companion = value;
            }
            if ((flags & 32U) != 0) {
                PlayerShadowState shadow;
                shadow.ground = {real(data), real(data), real(data)};
                shadow.normal = {real(data), real(data), real(data)};
                shadow.alpha = real(data);
                player.shadow = shadow;
            } else {
                for (usize i = 0; i < kShadowBytes / 4; ++i) {
                    if (data.readU32() != 0) {
                        return std::nullopt;
                    }
                }
            }
            result.players[seat] = player;
        }
    }
    result.enemies.reserve(enemies);
    for (usize i = 0; i < enemies; ++i) {
        EnemyCombatState enemy;
        enemy.instance = wide(data);
        enemy.kind = data.readU32();
        enemy.tier = data.readU32();
        enemy.variant = data.readU32();
        const u32 flags = data.readU32();
        if ((flags & ~7U) != 0) {
            return std::nullopt;
        }
        enemy.life = static_cast<ReplicaEnemyLife>(flags & 3U);
        enemy.hitFlash = (flags & 4U) != 0;
        enemy.health = real(data);
        enemy.fullHealth = real(data);
        enemy.position.x = real(data);
        enemy.position.y = real(data);
        enemy.position.z = real(data);
        enemy.yaw = real(data);
        enemy.animation = animation(data);
        result.enemies.push_back(enemy);
    }
    result.projectiles.reserve(projectiles);
    for (usize i = 0; i < projectiles; ++i) {
        ProjectileState shot;
        const u32 source = data.readU32();
        if (source > static_cast<u32>(ProjectileSource::Arrival)) {
            return std::nullopt;
        }
        shot.source = static_cast<ProjectileSource>(source);
        shot.instance = wide(data);
        shot.continuity = data.readU32();
        shot.resource = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                shot.placement[column][row] = real(data);
            }
        }
        shot.direction.x = real(data);
        shot.direction.y = real(data);
        shot.direction.z = real(data);
        shot.age = real(data);
        shot.radius = real(data);
        shot.forward = real(data);
        const u32 tint = data.readU32();
        shot.tint = Color::rgba(static_cast<u8>(tint), static_cast<u8>(tint >> 8U),
                                static_cast<u8>(tint >> 16U), static_cast<u8>(tint >> 24U));
        shot.animation = animation(data);
        shot.textureFrame = real(data);
        shot.alpha = real(data);
        shot.flags = data.readU32();
        result.projectiles.push_back(shot);
    }
    result.pickups.reserve(pickups);
    for (usize i = 0; i < pickups; ++i) {
        PickupState item;
        item.instance = wide(data);
        item.resource = data.readU32();
        item.continuity = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                item.placement[column][row] = real(data);
            }
        }
        item.animation = animation(data);
        item.textureFrame = real(data);
        item.alpha = real(data);
        result.pickups.push_back(item);
    }
    result.fixtures.reserve(fixtures);
    for (usize i = 0; i < fixtures; ++i) {
        FixtureState fixture;
        const auto source = data.readU32();
        if (source > static_cast<u32>(FixtureSource::SecretPortal)) {
            return std::nullopt;
        }
        fixture.source = static_cast<FixtureSource>(source);
        fixture.instance = wide(data);
        fixture.resource = data.readU32();
        fixture.continuity = data.readU32();
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                fixture.placement[column][row] = real(data);
            }
        }
        fixture.pose = animation(data);
        fixture.meshSequence = data.readU32();
        fixture.meshFrame = real(data);
        fixture.textureSequence = data.readU32();
        fixture.textureFrame = real(data);
        fixture.textureClock = real(data);
        fixture.alpha = real(data);
        const auto flags = data.readU32();
        if (flags > 1) {
            return std::nullopt;
        }
        fixture.cameraFacing = flags != 0;
        result.fixtures.push_back(fixture);
    }
    if (geometryBytes != 0) {
        SceneGeometry geometry;
        geometry.layout = wide(data);
        geometry.objectCount = data.readU32();
        const usize count = data.readU32();
        geometry.darken = real(data);
        if (count > SceneGeometry::kMaxStates ||
            geometryBytes != kGeometryHeaderBytes + count * kGeometryObjectBytes) {
            return std::nullopt;
        }
        geometry.objects.reserve(count);
        for (usize i = 0; i < count; ++i) {
            SceneGeometry::Object object;
            object.index = data.readU32();
            object.continuity = data.readU32();
            for (s32 column = 0; column < 4; ++column) {
                for (s32 row = 0; row < 3; ++row) {
                    object.local[column][row] = real(data);
                }
            }
            object.alpha = real(data);
            const u32 visible = data.readU32();
            if (visible > 1) {
                return std::nullopt;
            }
            object.visible = visible != 0;
            geometry.objects.push_back(object);
        }
        result.geometry = std::move(geometry);
    }
    if (fighterBytes != 0) {
        auto fighters = FighterPacket::decode(data.readBytes(fighterBytes));
        if (!fighters || fighters->empty()) {
            return std::nullopt;
        }
        result.fighters = std::move(*fighters);
    }
    if (hudBytes != 0) {
        result.hud = HudPacket::decode(data.readBytes(hudBytes));
        if (!result.hud) {
            return std::nullopt;
        }
    }
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

} // namespace gdl::game
