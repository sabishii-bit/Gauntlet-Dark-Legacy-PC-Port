#include "expansions/dreamcast/level/LegendsDreamcastTraps.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

std::optional<LegendsDreamcastTrap>
LegendsDreamcastTrap::decode(const formats::ItemInfoRecord& info,
                             const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                             std::array<s32, 4> sequenceTicks, std::string* diagnostic) {
    if (diagnostic) {
        diagnostic->clear();
    }
    if (info.type != 8) {
        return {};
    }
    const auto reject = [&](std::string_view message) -> std::optional<LegendsDreamcastTrap> {
        if (diagnostic) {
            *diagnostic = message;
        }
        return {};
    };
    const bool spikes = info.subtype == 0 && info.name == "SPIKES";
    const bool flame = info.subtype == 1 && (info.name == "FLAMEH" || info.name == "FLAMEV");
    if (!spikes && !flame) {
        return reject("Dreamcast trap needs an unsupported native action/contact adapter");
    }
    if ((info.collisionType != 1 && info.collisionType != 3) || info.activeType != 5 ||
        info.value <= 0 || info.value > 10000 || info.activeOn != 0 || instance.flags != 0 ||
        !instance.name.empty() || info.properties != (spikes ? damage::kSpike : damage::kFire) ||
        !finite(instance.position) || !finite(instance.rotation) || !finite(info.collisionOffset) ||
        !std::isfinite(info.radius) || info.radius <= 0 || !std::isfinite(info.height) ||
        info.height <= 0 || !std::isfinite(info.xSize) || info.xSize < 0 ||
        !std::isfinite(info.zSize) || info.zSize < 0 ||
        (info.collisionType == 3 && (info.xSize <= 0 || info.zSize <= 0)) ||
        std::ranges::any_of(sequenceTicks, [](s32 value) { return value < 0 || value > 36000; }) ||
        sequenceTicks[2] == 0) {
        return reject("Dreamcast trap has unsupported timing or contact parameters");
    }
    LegendsDreamcastTrap result;
    result.m_nativeIndex = nativeIndex;
    result.m_position = instance.position;
    result.m_damage = static_cast<f32>(info.value);
    result.m_properties = info.properties;
    result.m_subtype = info.subtype;
    result.m_durations = sequenceTicks;
    // Empty OFF/ONA/ONB sequences are valid native states, not missing art.
    // OFF still waits its authored rest; empty transitions last one item update.
    result.m_off = info.activeOff;
    result.m_random.seed(nativeIndex + 1);
    result.m_wait = result.rest();
    auto& contact = result.m_contact;
    // Native box extents are half widths, unlike the N64 template widths.
    // SetItem's collision anchor includes its common one-unit vertical lift.
    const Vec3 offset = info.collisionOffset + Vec3{0, 1, 0};
    contact.centre =
        Vec3{legendsDreamcastItemPlacement(instance.position, instance.rotation) * Vec4{offset, 1}};
    contact.yaw = instance.rotation.y;
    contact.height = info.height;
    contact.cylinderRadius = info.collisionType == 1 ? info.radius : 0;
    contact.halfAcross = info.collisionType == 1 ? info.radius : info.xSize;
    contact.halfAlong = info.collisionType == 1 ? info.radius : info.zSize;
    contact.solid = false; // trap contact causes harm, not a movement barrier
    return result;
}

s32 LegendsDreamcastTrap::rest() {
    // SetItem8c0a8340 trap branch passes activeOff*2 to the signed random-time
    // helper. Negative values select [half, one-and-a-half) of their magnitude.
    const s32 time = m_off * 2;
    return time < 0 ? static_cast<s32>(m_random() % static_cast<u32>(-time)) - time / 2 : time;
}

void LegendsDreamcastTrap::advance(f32 seconds, bool timeStopped) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10) {
        return;
    }
    for (auto& cooldown : m_cooldowns) {
        cooldown = std::max(0.f, cooldown - seconds);
    }
    m_stopped = timeStopped;
    m_ticks += static_cast<f64>(seconds) * 60;
    while (m_ticks + .000001 >= 1) {
        m_ticks -= 1;
        if (--m_wait <= 0) {
            m_action = (m_action + 1) % 4;
            m_wait = m_action == 0 ? rest() : m_durations[static_cast<usize>(m_action)];
            ++m_generation;
        }
        // do_items 8c0ab85e advances activetime before its Stop Time pass
        // (8c0abee0), which only clears action/daction. It does not install
        // GameCube's extra 30-tick rest or freeze the remaining countdown.
        if (timeStopped && m_action != 0) {
            m_action = 0;
            ++m_generation;
        }
    }
}

std::optional<LegendsTrapHit> LegendsDreamcastTrap::touch(const LegendsPortalVisitor& visitor) {
    // ItemTouch 8c0af400 checks Levitation before fxhittime or AudioTrap.
    if (!dangerous() || !visitor.alive || visitor.levitating || visitor.player >= 4 ||
        m_cooldowns[visitor.player] > 0 || !finite(visitor.position) ||
        !std::isfinite(visitor.height) || !std::isfinite(visitor.radius) || visitor.height <= 0 ||
        visitor.radius < 0) {
        return {};
    }
    const Vec3 centre = visitor.position + Vec3{0, visitor.height * .5f, 0};
    if (std::abs(centre.y - m_contact.centre.y) > m_contact.height + visitor.height * .5f) {
        return {};
    }
    Obstacle contact = m_contact;
    const Vec3 horizontal{centre.x, contact.centre.y, centre.z};
    if (contact.cylinderRadius > 0) {
        if (!contact.touchedBy(horizontal, visitor.radius)) {
            return {};
        }
    } else {
        contact.halfAcross += visitor.radius;
        contact.halfAlong += visitor.radius;
        if (!contact.touchedBy(horizontal, 0)) {
            return {};
        }
    }
    // ItemTouch8c0af0e0 applies value and an fxhittime based on remaining
    // animation ticks. Keep player slots independent and never hurt during ONA.
    m_cooldowns[visitor.player] = static_cast<f32>(m_wait + 1) / 30;
    return LegendsTrapHit{m_nativeIndex,
                          visitor.player,
                          m_damage,
                          m_subtype == 0,
                          m_position,
                          {m_properties | damage::kStun | damage::kLow, Vec3{0}},
                          0};
}

u32 LegendsDreamcastTrap::contactSound(s32 realm) const {
    // AudioTrap 8c0df0a0 selects [subtype][realm] from 8c127698.
    // Unassigned native entries are silent, not borrowed from another realm.
    constexpr std::array<u32, 9> kSpikeSounds{0, 0x23000f, 0, 0x240033, 0x25002d, 0, 0, 0, 0};
    constexpr std::array<u32, 9> kFlameSounds{0, 0,        0x210027, 0x240034, 0x250000,
                                              0, 0x2d001f, 0x22001c, 0x26001c};
    const auto& sounds = m_subtype == 0 ? kSpikeSounds : kFlameSounds;
    return realm >= 0 && static_cast<usize>(realm) < sounds.size()
               ? sounds[static_cast<usize>(realm)]
               : 0;
}
} // namespace gdl::game
