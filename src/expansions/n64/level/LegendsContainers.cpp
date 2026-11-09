#include "expansions/n64/level/LegendsContainers.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {

void require(bool value, std::string_view message) {
    if (!value) {
        throw FormatError("N64 Legends container: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize offset, usize size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset, "truncated template table");
}
usize templateAt(std::span<const u8> bytes, usize stride, u16 key) {
    range(bytes, 0, 4);
    const usize count = readU32BE(bytes, 0);
    require(count <= 4096, "template count exceeds budget");
    range(bytes, 4, count * stride);
    usize found = bytes.size();
    for (usize i = 0; i < count; ++i) {
        const usize at = 4 + i * stride;
        if (readU16BE(bytes, at) == key) {
            require(found == bytes.size(), "duplicate template key");
            found = at;
        }
    }
    require(found != bytes.size(), "unknown template key");
    return found;
}
std::string sequence(std::span<const u8> bytes, usize offset) {
    if (readU32BE(bytes, offset) == 0) {
        return {};
    }
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        const u8 c = bytes[offset + i];
        require(c >= 32 && c < 127, "invalid sequence name");
        result.push_back(static_cast<char>(c));
    }
    return result;
}
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

std::optional<LegendsContainerDefinition>
decodeLegendsN64Container(const formats::LegendsN64Fixture& fixture, std::span<const u8> containers,
                          std::span<const u8> powerups) {
    if (fixture.kind != formats::LegendsN64FixtureKind::Container) {
        return std::nullopt;
    }
    const usize at = templateAt(containers, 44, fixture.templateKey);
    const u8 kind = static_cast<u8>(fixture.templateKey >> 8U);
    const u32 flags = readU32BE(containers, at + 4);
    const bool barrel = (kind == 0x19 || kind == 0x2e) && (flags & 0x07000000U) == 0x04000000U;
    const bool chest = (kind == 0x17 || kind == 0x18) && (flags & 0x07000000U) == 0x02000000U;
    if ((!barrel && !chest) || fixture.placementFlags != 0) {
        return std::nullopt;
    }
    require(finite(fixture.position) && std::isfinite(fixture.facingRadians),
            "non-finite placement");
    LegendsContainerDefinition result;
    result.kind =
        barrel ? LegendsContainerDefinition::Kind::Barrel : LegendsContainerDefinition::Kind::Chest;
    result.nativeIndex = fixture.nativeIndex;
    result.position = fixture.position;
    result.yaw = -fixture.facingRadians;
    result.closed = sequence(containers, at + 8);
    result.opening = sequence(containers, at + 12);
    result.broken = sequence(containers, at + 16);
    result.openingSound = readU32BE(containers, at + 40);
    require(!result.closed.empty(), "container has no closed sequence");
    result.damageThreshold = static_cast<f32>(readU16BE(containers, at + 2));
    auto& obstacle = result.closedObstacle;
    obstacle.centre = fixture.position;
    obstacle.yaw = result.yaw;
    const u32 shape = readU32BE(containers, at + 24);
    const Vec3 size{std::bit_cast<f32>(readU32BE(containers, at + 28)),
                    std::bit_cast<f32>(readU32BE(containers, at + 32)),
                    std::bit_cast<f32>(readU32BE(containers, at + 36))};
    require(finite(size) && size.x > 0 && size.y > 0 && size.x <= 64 && size.y <= 64 &&
                size.z >= 0 && size.z <= 64,
            "invalid native collision dimensions");
    // e00638b8 stores full box dimensions. e0060d8c/e0060764 halve the
    // fixed-point s16 fields; cylinder radius is not halved.
    // Its collider centre is raised by half-height; Obstacle stores the feet Y.
    require(shape == 1 || (shape == 3 && size.z > 0), "unsupported native collision shape");
    obstacle.height = size.y;
    const auto half = [](f32 value) { return std::floor(std::trunc(value * 32) / 2) / 32; };
    obstacle.halfAcross = shape == 1 ? size.x : half(size.x);
    obstacle.halfAlong = shape == 1 ? size.x : half(size.z);
    obstacle.cylinderRadius = shape == 1 ? size.x : 0;

    const usize contentAt = templateAt(powerups, 28, fixture.containedPowerup);
    const u8 contentKind = static_cast<u8>(fixture.containedPowerup >> 8U);
    if (contentKind == 0x24) { // e006d570: the authored empty-content sentinel.
        return result;
    }
    if (contentKind == 0x22) {
        result.selector = fixture.containedPowerup;
        return result;
    }
    formats::LegendsN64Fixture contents;
    contents.kind = formats::LegendsN64FixtureKind::Powerup;
    contents.nativeIndex = fixture.nativeIndex;
    contents.templateKey = fixture.containedPowerup;
    contents.resolvedTemplateKey = fixture.containedPowerup;
    contents.templateFlags = readU32BE(powerups, contentAt + 4);
    contents.resolvedTemplateFlags = contents.templateFlags;
    contents.templateAmount = std::bit_cast<s16>(readU16BE(powerups, contentAt + 2));
    contents.quantity = fixture.quantity;
    contents.pickupLimit = readU32BE(powerups, contentAt + 20);
    contents.pickupSound = readU32BE(powerups, contentAt + 24);
    contents.position = fixture.position;
    contents.group = "POWERUP";
    contents.sequence = sequence(powerups, contentAt + 8);
    contents.deathSequence = sequence(powerups, contentAt + 16);
    result.contents = std::move(contents);
    // e006cab8 substitutes the final FOURCC byte only if that tree exists.
    if (contentKind == 3 && !result.opening.empty()) {
        result.goldOpening = result.opening;
        result.goldOpening.back() = 'G';
    }
    return result;
}

std::optional<LegendsContainerDefinition>
decodeLegendsN64Explosive(const formats::LegendsN64Fixture& fixture, std::span<const u8> traps) {
    // e00691bc handles kind 0x13/0x14 as one-hit explosive traps. The unused
    // 0x1302 UNKN template has no attack/debris art and is not guessed here.
    if (fixture.kind != formats::LegendsN64FixtureKind::Trap ||
        (fixture.templateKey != 0x1301 && fixture.templateKey != 0x1401)) {
        return std::nullopt;
    }
    const usize at = templateAt(traps, 96, fixture.templateKey);
    require((readU32BE(traps, at + 4) & 0x07000000U) == 0x04000000U,
            "explosive trap has incompatible activation flags");
    require(finite(fixture.position) && std::isfinite(fixture.facingRadians) &&
                std::isfinite(fixture.scale) && fixture.scale > 0 && fixture.scale <= 64,
            "invalid explosive placement");
    LegendsContainerDefinition result;
    result.nativeIndex = fixture.nativeIndex;
    result.position = fixture.position;
    result.yaw = -fixture.facingRadians;
    result.group = "TRAP";
    result.scale = fixture.scale;
    result.closed = sequence(traps, at + 12);
    result.opening = sequence(traps, at + 16);
    result.broken = sequence(traps, at + 20);
    require(!result.closed.empty() && !result.opening.empty() && !result.broken.empty(),
            "explosive trap has missing animation names");
    result.damageThreshold = static_cast<f32>(readU16BE(traps, at + 8));
    result.openingSound = readU32BE(traps, at + 28);
    result.hazard = fixture.templateKey == 0x1301 ? LegendsContainerDefinition::Hazard::Fire
                                                  : LegendsContainerDefinition::Hazard::Poison;
    result.blastRadius = fixture.templateKey == 0x1301 ? 5.0f : 4.5f;
    result.blastDamage = static_cast<f32>(traps[at + 10]);
    const Vec3 size{std::bit_cast<f32>(readU32BE(traps, at + 36)),
                    std::bit_cast<f32>(readU32BE(traps, at + 40)),
                    std::bit_cast<f32>(readU32BE(traps, at + 44))};
    require(readU32BE(traps, at + 32) == 1 && finite(size) && size.x > 0 && size.y > 0 &&
                size.x <= 64 && size.y <= 64,
            "unsupported explosive collision dimensions");
    auto& body = result.closedObstacle;
    body.centre = result.position;
    body.yaw = result.yaw;
    body.height = size.y * fixture.scale;
    body.cylinderRadius = size.x * fixture.scale;
    body.halfAcross = body.cylinderRadius;
    body.halfAlong = body.cylinderRadius;
    return result;
}

LegendsContainer::LegendsContainer(LegendsContainerDefinition definition)
    : m_definition(std::move(definition)),
      m_health(std::isfinite(m_definition.damageThreshold)
                   ? static_cast<s32>(std::clamp(m_definition.damageThreshold, 0.0f, 32767.0f))
                   : 0) {}

bool LegendsContainer::targetable() const {
    return m_definition.interactive && m_state == State::Closed &&
           m_definition.kind == LegendsContainerDefinition::Kind::Barrel;
}

bool LegendsContainer::strike(f32 power) {
    // e006cef4 compares each blow against PCon+2, with no stored hit-point sum.
    if (!targetable() || !std::isfinite(power) || power <= 0) {
        return false;
    }
    if (m_definition.damageMode == LegendsContainerDefinition::DamageMode::Accumulated) {
        // Dreamcast HitItem8c0afaa0 stores remaining HP after armor, unlike
        // N64 PCon's per-blow threshold. Use the engine's bounded rounding.
        f32 felt = power - static_cast<f32>(m_definition.armor);
        if (felt <= 0)
            felt = 1;
        m_health = std::max(m_health - static_cast<s32>(std::lround(std::min(felt, 32767.0f))), 0);
        if (m_health > 0)
            return true;
    } else if (power < m_definition.damageThreshold) {
        return false;
    }
    m_state = State::Opening;
    return true;
}

bool LegendsContainer::touch(const Vec3& feet, f32 radius, f32 height, s32& keys) {
    if (!m_definition.interactive || m_state != State::Closed ||
        m_definition.kind != LegendsContainerDefinition::Kind::Chest || keys <= 0 ||
        !finite(feet) || !std::isfinite(radius) || radius < 0 || !std::isfinite(height) ||
        height < 0 || feet.y > m_definition.position.y + m_definition.closedObstacle.height ||
        feet.y + height < m_definition.position.y ||
        !m_definition.closedObstacle.touchedBy(
            {feet.x, m_definition.closedObstacle.centre.y, feet.z}, radius, .05f)) {
        return false;
    }
    // e006d3d4 spends exactly one key on first contact, never on repeated ticks.
    --keys;
    m_state = State::Opening;
    return true;
}

bool LegendsContainer::finishOpening() {
    if (m_state != State::Opening) {
        return false;
    }
    m_state = State::Open;
    return true;
}

void LegendsContainer::collected() {
    if (m_state == State::Open) {
        m_state = State::Empty;
    }
}

void LegendsContainer::placeAt(const Vec3& feet) {
    if (!finite(feet)) {
        throw std::invalid_argument("Native container requires a finite placement");
    }
    const Vec3 delta = feet - m_definition.position;
    m_definition.position = feet;
    m_definition.closedObstacle.centre += delta;
    if (m_definition.contents) {
        m_definition.contents->position += delta;
    }
}

Obstacle LegendsContainer::obstacle() const {
    auto result = m_definition.closedObstacle;
    // e006cab8 retains and reindexes a chest's body while its opening tree
    // runs; e006d3d4 does not admit its contents before completion (0x1000).
    // Broken barrels, unlike opening chests, stop blocking immediately.
    result.solid =
        m_state == State::Closed ||
        (m_state == State::Opening && m_definition.kind == LegendsContainerDefinition::Kind::Chest);
    return result;
}

} // namespace gdl::game
