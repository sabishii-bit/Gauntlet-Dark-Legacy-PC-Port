#include "expansions/n64/bosses/LegendsBosses.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/Sha256.h"
#include "engine/math/PortableMath.h"

#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "game/level/BossKinds.h"

namespace gdl::game {
namespace {

struct Counterpart {
    u32 realm;
    usize slot;
    s32 kind;
    std::string_view stage;
    std::string_view world;
    std::string_view sound;
    usize nativeName;
    s32 legend;
};

// game.bin e006a858 selects these native archives; e006b110 selects their
// placement-table slots. GDL WDATA LEVL.bossType/AUDS supply the donor context.
constexpr std::array kCounterparts{
    Counterpart{3, 0, boss_type::kDragon, "B6", "MOUNT", "DRAGON", 0xe00ab708, 0},
    Counterpart{6, 1, boss_type::kYeti, "I5", "ICE", "YETI", 0xe00ab71c, 1},
    Counterpart{7, 2, boss_type::kPlagueBoss, "K5", "SKY", "PBOSS", 0xe00ab724, 3},
    Counterpart{0, 3, boss_type::kChimera, "A5", "CASTLE", "CHIMERA", 0xe00ab73c, 2},
    Counterpart{13, 4, boss_type::kSkorne1, "E2", "TEMPLE", "SKORNE1", 0xe00ab764, -1},
    Counterpart{5, 5, boss_type::kSkorne2, "F2", "HELL", "SKORNE2", 0xe00ab76c, 4},
};
constexpr usize kProgramBase = 0xe0000400U;
constexpr usize kBossBase = 0xe0880000U;

void authenticate(std::span<const u8> bytes, usize size, std::string_view expected) {
    Sha256 hash;
    hash.update(bytes);
    if (bytes.size() != size || hash.hex() != expected) {
        throw FormatError("N64 Legends boss: unsupported native program");
    }
}

f32 scalar(std::span<const u8> bytes, usize at) {
    return std::bit_cast<f32>(readU32BE(bytes, at));
}

std::string name(std::span<const u8> bytes, usize at) {
    std::string value;
    for (usize i = 0; i < 16 && at + i < bytes.size(); ++i) {
        if (bytes[at + i] == 0) {
            return value;
        }
        value.push_back(static_cast<char>(bytes[at + i]));
    }
    throw FormatError("N64 Legends boss: unterminated native archive");
}

bool finite(Vec3 position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

} // namespace

LegendsBossCamera legendsN64BossCamera(std::span<const u8> moves) {
    if (moves.size() < 8) {
        throw FormatError("N64 Legends boss camera: truncated WAD");
    }
    const usize directory = readU32BE(moves, 0);
    const usize count = readU32BE(moves, 4);
    if (directory < 8 || directory > moves.size() || count > 4096 ||
        count > (moves.size() - directory) / 12) {
        throw FormatError("N64 Legends boss camera: invalid WAD directory");
    }
    usize offset = 0;
    usize types = 0;
    for (usize index = 0; index < count; ++index) {
        const usize at = directory + index * 12;
        if (readU32BE(moves, at) == 0x54595045U) { // TYPE
            if (offset != 0) {
                throw FormatError("N64 Legends boss camera: duplicate TYPE");
            }
            offset = readU32BE(moves, at + 4);
            types = readU32BE(moves, at + 8);
        }
    }
    usize end = directory;
    for (usize index = 0; index < count; ++index) {
        const usize next = readU32BE(moves, directory + index * 12 + 4);
        if (next > offset) {
            end = std::min(end, next);
        }
    }
    if (offset < 8 || offset > end || types == 0 || types > 256 || types > (end - offset) / 0xf8) {
        throw FormatError("N64 Legends boss camera: invalid TYPE records");
    }
    // Boss.bin e0881474 grounds the actor then adds TYPE+94. e0884a9c
    // publishes actor+TYPE+98 to e0038d90, the camera's focus and the rewards'
    // centre, apart from TYPE+9c's combat origin. e08842e8 supplies TYPE+8c to
    // e0038df4.
    LegendsBossCamera result{scalar(moves, offset + 0x94), scalar(moves, offset + 0x98),
                             scalar(moves, offset + 0x8c)};
    if (!std::isfinite(result.floorOffset) || !std::isfinite(result.attentionHeight) ||
        !std::isfinite(result.radius) || std::abs(result.floorOffset) > 10000 ||
        std::abs(result.attentionHeight) > 10000 || result.radius < 0 || result.radius > 1000) {
        throw FormatError("N64 Legends boss camera: invalid focus");
    }
    return result;
}

std::optional<LegendsBossEncounter>
legendsN64Boss(std::string_view map, std::span<const u8> program, std::span<const u8> bossOverlay) {
    const auto identity = LegendsProgress::mapIdentity(map);
    if (!identity) {
        return std::nullopt;
    }
    authenticate(program, 707312,
                 "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585");
    const auto stages = LegendsN64Unlocks::decode(program);
    const auto stage = std::ranges::find(stages.stages(), *identity, &LegendsStageUnlock::map);
    if (stage == stages.stages().end() || stage->stage != 8) {
        return std::nullopt;
    }
    authenticate(bossOverlay, 58608,
                 "87051fc6527f842ea144dd13bb7fab301703df441cb4ebe8b80b721ba33a93f2");
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto counterpart = std::ranges::find(kCounterparts, stage->realm, &Counterpart::realm);
    if (counterpart == kCounterparts.end()) {
        throw FormatError("N64 Legends boss: unknown encounter realm");
    }
    const usize pose = 0xe006c190U - kProgramBase + counterpart->slot * 24;
    LegendsBossEncounter result;
    result.map = *identity;
    result.nativeRealm = stage->realm;
    result.nativeArchive = name(program, counterpart->nativeName - kProgramBase);
    result.combat.kind = counterpart->kind;
    result.combat.position = {scalar(program, pose + 12), scalar(program, pose + 16),
                              scalar(program, pose + 20)};
    // e0014d04 creates the initial matrix; e0881474 recovers atan2(m[6],m[8]).
    // All authored initial rotations are yaw-only, the same +Z facing convention.
    result.combat.yaw = scalar(program, pose + 4);
    result.combat.counterpartStage = counterpart->stage;
    result.combat.counterpartWorld = counterpart->world;
    result.combat.soundBank = counterpart->sound;
    // game.bin e004f554: stage-kind8 tries Boss e088556c/e0885328 first,
    // and only falls back to the ordinary e00623a8 probe when no boss part
    // qualifies. Keep the native pillars physically breakable, not coequal
    // ranged aim candidates that pull a shot down from a visible head.
    result.combat.rangedAimFirst = true;
    result.consumedLegend = counterpart->legend;
    // e006b110 applies the realm scale and mirrors X before e0888cd4
    // creates SAFEROCK:L3 cylinders. These are script objects, absent from IPR.
    usize rocks = 0;
    f32 rockScale = 1;
    if (stage->realm == 3) {
        rocks = 0xe006c220U - kProgramBase;
        rockScale = 1.5f;
    } else if (stage->realm == 7) {
        rocks = 0xe006c290U - kProgramBase;
        rockScale = 0.8f;
    } else if (stage->realm == 6) {
        rocks = 0xe006c2d0U - kProgramBase;
    }
    for (usize i = 0; rocks != 0 && i < 16; ++i) {
        const usize at = rocks + i * 16;
        const f32 yaw = scalar(program, at + 12);
        if (yaw <= -9) {
            break;
        }
        result.combat.arenaRocks.push_back(
            {{-scalar(program, at) * rockScale, scalar(program, at + 4) * rockScale,
              scalar(program, at + 8) * rockScale},
             yaw,
             2.3f,
             5,
             stage->realm == 7 ? 80 : 40});
    }
    // Boss.bin e08898d8 uses 16-byte reward records; the list ends at realm16.
    for (usize i = 0; i < 9; ++i) {
        const usize at = 0xe0889c04U - kBossBase + i * 16;
        const u32 realm = readU32BE(bossOverlay, at);
        if (realm == 16) {
            break;
        }
        if (realm == stage->realm) {
            result.rewards.push_back({bossOverlay[at + 5], bossOverlay[at + 7],
                                      scalar(bossOverlay, at + 8), scalar(bossOverlay, at + 12)});
        }
    }
    if (stage->realm == 13) {
        result.sharedObelisk = 7; // e08898d8 calls e0068000(8), unlocking Battlefield.
    } else if (stage->realm == 5) {
        // Native final victory has an ending fade, not the ordinary twelve-second
        // exit. The adapter returns to Tower after this interval; it does not
        // claim to reproduce that ending cinematic or the opaque inventory grant.
        result.departureSeconds = 825.0f / 150.0f;
        result.clearsRunestones = true;
        result.finalNativeGrant = 0x300290U;
    }
    return result;
}

std::vector<formats::LegendsN64Fixture>
legendsN64BossRewardFixtures(const LegendsBossEncounter& encounter, std::span<const u8> powerups) {
    if (encounter.rewards.empty()) {
        return {};
    }
    if (encounter.rewards.size() > 4 || powerups.size() < 4) {
        throw FormatError("N64 Legends boss: invalid reward table");
    }
    const usize count = readU32BE(powerups, 0);
    if (count > 4096 || count > (powerups.size() - 4) / 28) {
        throw FormatError("N64 Legends boss: truncated PPow table");
    }
    std::vector<formats::LegendsN64Fixture> result;
    for (const auto& reward : encounter.rewards) {
        if ((reward.kind != 0x2a && reward.kind != 0x2b) || reward.subtype < 1 ||
            reward.subtype > 4) {
            throw FormatError("N64 Legends boss: unsupported scripted reward");
        }
        const u16 key = static_cast<u16>(static_cast<u16>(reward.kind) << 8U | reward.subtype);
        usize at = powerups.size();
        for (usize i = 0; i < count; ++i) {
            if (readU16BE(powerups, 4 + i * 28) == key) {
                if (at != powerups.size()) {
                    throw FormatError("N64 Legends boss: duplicate PPow reward");
                }
                at = 4 + i * 28;
            }
        }
        if (at == powerups.size()) {
            throw FormatError("N64 Legends boss: missing PPow reward");
        }
        formats::LegendsN64Fixture fixture;
        fixture.nativeIndex = 0x80000000U + static_cast<u32>(result.size());
        fixture.templateKey = fixture.resolvedTemplateKey = key;
        fixture.templateFlags = fixture.resolvedTemplateFlags = readU32BE(powerups, at + 4);
        fixture.templateAmount = std::bit_cast<s16>(readU16BE(powerups, at + 2));
        fixture.pickupLimit = readU32BE(powerups, at + 20);
        fixture.pickupSound = readU32BE(powerups, at + 24);
        constexpr std::array kArmorArchives{"items/s/bossm", "items/s/bossh", "items/s/bossgr",
                                            "items/s/bossgl"};
        constexpr std::array kShardArchives{"items/s/sm", "items/s/si", "items/s/sc", "items/s/st"};
        constexpr std::array<u32, 4> kArmorIdentities{0x620, 0x630, 0xa40, 0xa50};
        const usize subtype = reward.subtype - 1;
        const u32 identity = fixture.templateFlags & 0xffffU;
        if (identity != (reward.kind == 0x2a ? kArmorIdentities[subtype] : 0x8260U)) {
            throw FormatError("N64 Legends boss: incompatible PPow reward identity");
        }
        // e0064e60's script-created pickups load their own bank on demand.
        // Preload the same bank instead, before the rendering resources freeze.
        fixture.archive = reward.kind == 0x2a ? kArmorArchives[subtype] : kShardArchives[subtype];
        fixture.position = encounter.combat.position;
        fixture.group = "POWERUP";
        fixture.groundProjected = false;
        for (usize i = 0; i < 4; ++i) {
            const u8 c = powerups[at + 8 + i];
            if (c < 32 || c >= 127) {
                throw FormatError("N64 Legends boss: invalid reward sequence");
            }
            fixture.sequence.push_back(static_cast<char>(c));
        }
        result.push_back(std::move(fixture));
    }
    return result;
}

std::optional<LegendsBossRewardMotion>
legendsBossRewardMotion(const LegendsBossReward& reward, Vec3 deathPosition, Vec3 playerStart) {
    if (!finite(deathPosition) || !finite(playerStart) || !std::isfinite(reward.angle) ||
        !std::isfinite(reward.distance) || reward.distance < 0 || reward.distance > 1000) {
        return std::nullopt;
    }
    deathPosition.y = std::max(deathPosition.y, playerStart.y) + 10;
    const Vec3 delta = playerStart - deathPosition;
    const f32 length = glm::length(delta);
    if (!finite(deathPosition) || !std::isfinite(length) || length <= 0) {
        return std::nullopt;
    }
    const Vec3 direction = delta / length;
    // e0014364 uses the native clockwise yaw convention for this throw fan.
    const f32 sine = portable::sin(reward.angle);
    const f32 cosine = portable::cos(reward.angle);
    const Vec3 turned{direction.x * cosine - direction.z * sine, direction.y,
                      direction.z * cosine + direction.x * sine};
    // The first vector is an absolute position, not a velocity: e0066864
    // writes it into IPR XYZ, then e0076730 targets the second vector.
    const Vec3 origin = turned * 10.0f + Vec3{0, 5, 0};
    const Vec3 target = deathPosition + turned * reward.distance;
    const Vec3 travel = target - origin;
    const f32 duration = glm::length(travel) / 20.0f;
    if (!std::isfinite(duration) || duration <= 0) {
        return std::nullopt;
    }
    return LegendsBossRewardMotion{origin, travel / duration + Vec3{0, 32 * duration, 0}};
}

Vec3 legendsN64BossFocus(const LegendsBossCamera& type, const Vec3& floorAnchor) {
    return floorAnchor + Vec3{0.0f, type.floorOffset + type.attentionHeight, 0.0f};
}

} // namespace gdl::game
