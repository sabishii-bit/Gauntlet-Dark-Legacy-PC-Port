#include "expansions/n64/formats/LegendsN64Tower.h"

#include <bit>
#include <cmath>
#include <numbers>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/Sha256.h"

namespace gdl::formats {

LegendsN64Tower decodeLegendsN64Tower(std::span<const u8> overlay) {
    // Exact US overlay whose consumers are e0860198/e0864e7c. This is a
    // fixed-address script, not a portable record table from worlds.wad.
    Sha256 hash;
    hash.update(overlay);
    if (overlay.size() != 44112 ||
        hash.hex() != "147963c1bb70445dc17ff2a8d236e00226dba922ab61bd2b742e8b46e713b47a") {
        throw FormatError("N64 Legends Tower: unsupported native overlay");
    }
    const auto text = [&](usize at) {
        const usize begin = at;
        while (at < overlay.size() && overlay[at] != 0) {
            ++at;
        }
        if (at == overlay.size()) {
            throw FormatError("N64 Legends Tower: unterminated script name");
        }
        return std::string(std::bit_cast<const char*>(overlay.data() + begin), at - begin);
    };
    const auto vector = [&](usize at) {
        return Vec3{std::bit_cast<f32>(readU32BE(overlay, at)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 4)),
                    std::bit_cast<f32>(readU32BE(overlay, at + 8))};
    };
    LegendsN64Tower result;
    // e0860198 uses /sum artwork with /wiz/anim/seq.wad's skeleton and keys.
    // Its first ordinary idle update (e08612e0) immediately selects LKDN.
    result.sumner = {text(0xa684).substr(1), "wiz/anim",
                     text(0xa6a0),           "LKDN",
                     vector(0xa678),         std::bit_cast<f32>(0x40278d37U)};
    result.podium = {"items/tower", "items/tower",          text(0xa6ac),
                     "REDY",        result.sumner.position, result.sumner.yaw};
    // e0860198 creates shape1/category7 for Sumner: feet XYZ, height0xc0,
    // radius0x30, radius-squared0x900. e00604b4 uses radius sum and Y overlap.
    result.residentVolumes[0] = {LegendsN64TowerVolume::Shape::Cylinder,
                                 result.sumner.position + Vec3{0, 3, 0},
                                 {1.5f, 3, 1.5f},
                                 0,
                                 7};
    // Its second volume is shape3/category14. e0014364 rotates local Z=1.8
    // by negative yaw; positions/dimensions are then truncated to s16/32.
    // e0060d8c/e0060764 halve the stored dimensions using an integer shift,
    // so authored Z=1.3 becomes 41/32 wide, with a queried half-width20/32.
    const auto fixed = [](f32 value) { return std::trunc(value * 32.0f) / 32.0f; };
    const Vec3 podiumCentre{fixed(result.sumner.position.x - 1.8f * std::sin(result.sumner.yaw)),
                            fixed(result.sumner.position.y + 2.5f),
                            fixed(result.sumner.position.z + 1.8f * std::cos(result.sumner.yaw))};
    result.residentVolumes[1] = {LegendsN64TowerVolume::Shape::Box,
                                 podiumCentre,
                                 {.5f, 2.5f, 20.0f / 32.0f},
                                 -static_cast<f32>(0x3554) * std::numbers::pi_v<f32> / 16384.0f,
                                 14};
    // e0864e7c: locked doors hold the first frame of their A sequence (0x8000).
    // The unlock ceremony releases that hold at e0865c98 case7. Merely walking
    // up to the locked door does not open it. Already unlocked doors use O.
    // Required obelisks come from game e006c360's realm records, queried by
    // e004e514/e006bb1c/e006b8a4. These are NOT counts or map-completion bits.
    result.doors = {{{"TWNA", "TWNO", 0x80, 7, vector(0x96ac), 9, 0x60},
                     {"CASA", "CASO", 1, 0, vector(0x9694), 9, 0x07},
                     {"ICEA", "ICEO", 0x40, 6, vector(0x96a0), 9, 0x18},
                     {"BTLA", "BTLO", 0x1000, 12, vector(0x96b8), 9, 0x80}}};
    result.staffModel = text(0xa6c4);
    result.staffParent = text(0xa6bc);
    // e08694e4 sets staff state3 when no ceremony is pending; e0860d1c hides
    // the attached staff in that state. It is not part of casual reading.
    result.staffVisibleInIdle = false;
    // e0862fc4 hides this existing world drawable unless all four shards have
    // already been installed. A fresh visit must not show the unlocked beam.
    result.initiallyHiddenWorldObjects.push_back(text(0xa7cc));
    // e0861630 uses names from the pointer table, not the pickup tree names.
    // e08614d0 places the stones around the native floor's rune shadow; its
    // e08615d4 angle is atan2(centre.x-x, centre.z-z). The following basis
    // operations reproduce e0013878 then e0013b18 in the column-vector engine.
    constexpr std::array<usize, 13> kRuneArchives{0xa6dc, 0xa6e8, 0xa6f4, 0xa700, 0xa70c,
                                                  0xa718, 0xa724, 0xa730, 0xa73c, 0xa748,
                                                  0xa758, 0xa768, 0xa778};
    const Vec3 centre = vector(0x967c);
    for (usize i = 0; i < result.runestones.size(); ++i) {
        auto& relic = result.runestones[i];
        relic.archive = text(kRuneArchives[i]).substr(1);
        relic.model = text(readU32BE(overlay, 0x971c + i * 4) - 0xe0860000U);
        const f32 phase = static_cast<f32>(i + 1) * std::bit_cast<f32>(0x3ee90690U) +
                          std::bit_cast<f32>(0x3e2d38fcU) - kHalfPi;
        const Vec3 position = centre + Vec3{std::sin(phase) * 5.3f, .2f, std::cos(phase) * 5.3f};
        const f32 angle = std::atan2(centre.x - position.x, centre.z - position.z);
        relic.placement =
            glm::translate(Mat4{1}, position) * glm::rotate(Mat4{1}, angle, Vec3{0, 1, 0}) *
            glm::rotate(Mat4{1}, -kHalfPi, Vec3{1, 0, 0}) * glm::scale(Mat4{1}, Vec3{1.8f});
    }
    // e0862fc4's shard pointer/position tables are in native identity order:
    // Mountain, Ice, Castle, Town (not realm-table order or GDL shard order).
    constexpr std::array<usize, 4> kShardArchives{0xa7b4, 0xa79c, 0xa7c0, 0xa7a8};
    const f32 shardScale = std::bit_cast<f32>(readU32BE(overlay, 0x9790));
    for (usize i = 0; i < result.shards.size(); ++i) {
        auto& relic = result.shards[i];
        relic.archive = text(kShardArchives[i]);
        relic.model = text(readU32BE(overlay, 0x9750 + i * 4) - 0xe0860000U);
        relic.placement = glm::translate(Mat4{1}, vector(0x9760 + i * 12)) *
                          glm::scale(Mat4{1}, Vec3{shardScale});
        relic.nativeDrawFlags = 0x10001a80U;
    }
    result.shardPortal = {"items/tower", "items/tower", text(0xa7d8), "EXNR", vector(0x9794), 0};
    result.shardPortalScale = 1.3f;
    return result;
}

} // namespace gdl::formats
