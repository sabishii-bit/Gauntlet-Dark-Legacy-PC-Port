#include "expansions/n64/level/LegendsContainers.h"

#include <bit>
#include <cmath>
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

std::optional<LegendsN64Container>
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
    LegendsN64Container decoded;
    auto& result = decoded.definition;
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
        return decoded;
    }
    if (contentKind == 0x22) {
        result.selector = fixture.containedPowerup;
        return decoded;
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
    decoded.contents = std::move(contents);
    // e006cab8 substitutes the final FOURCC byte only if that tree exists.
    if (contentKind == 3 && !result.opening.empty()) {
        result.goldOpening = result.opening;
        result.goldOpening.back() = 'G';
    }
    return decoded;
}

std::optional<LegendsN64Container>
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
    LegendsN64Container decoded;
    auto& result = decoded.definition;
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
    return decoded;
}

} // namespace gdl::game
