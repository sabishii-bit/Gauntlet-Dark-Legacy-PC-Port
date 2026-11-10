#include "expansions/dreamcast/level/LegendsDreamcastTower.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>

namespace gdl::game {
namespace {
// 1ST_READ.BIN 8c065acc..8c065b2c and 8c11bc08/8c11bc6c.
constexpr std::array<std::string_view, 8> kGates{
    "T1XSNS_DRFIE#0", "T1TRAPW02", "T1XSNS_DRFIE#2", "T1TRAPW04",
    "T1XSNS_DRFIE#3", "T1TRAPW05", "T1XSNS_DRFIE#1", "T1TRAPW03"};
constexpr std::array<u64, 3> kGateObelisks{0x38, 0x7, 0x1c0};
constexpr std::array<std::string_view, 13> kRunes{
    "T1RUNEA1", "T1RUNEA2", "T1RUNEA3", "T1RUNEB1", "T1RUNEB2", "T1RUNEB3", "T1RUNEC1",
    "T1RUNEC2", "T1RUNEC3", "T1RUNED1", "T1RUNED2", "T1RUNED3", "T1RUNEE1"};
constexpr std::array<std::string_view, 4> kShards{"T1SHARD02", "T1SHARD01", "T1SHARD03",
                                                  "T1SHARD04"};
constexpr std::array<u64, 4> kShardMasks{4, 2, 8, 16};
constexpr std::array<u64, 9> kObeliskOrder{8, 16, 32, 1, 2, 4, 64, 128, 256};
constexpr std::array<u64, 13> kRuneOrder{8, 16, 32, 1, 2, 4, 64, 128, 256, 512, 1024, 2048, 4096};

/** Where a native object name stands in one of the binding tables, if it does. */
std::optional<usize> entryOf(std::span<const std::string_view> names, std::string_view name) {
    for (usize entry = 0; entry < names.size(); ++entry) {
        if (names[entry] == name) {
            return entry;
        }
    }
    return std::nullopt;
}

std::string firstMissing(std::string_view kind, u64 owned, std::span<const u64> order) {
    const auto missing =
        std::ranges::find_if(order, [owned](u64 mask) { return (owned & mask) == 0; });
    return std::format("legends.dreamcast.sumner.{}.{}", kind, missing - order.begin());
}
} // namespace

LegendsDreamcastTower::LegendsDreamcastTower(const formats::LegendsMap& map) {
    for (usize object = 0; object < map.objects.size(); ++object) {
        // A script-hidden parent must hide its children and their collision too.
        // Walk a bounded ancestry chain; malformed cycles cannot hang binding.
        s32 parent = static_cast<s32>(object);
        for (usize depth = 0; depth < map.objects.size() && parent >= 0; ++depth) {
            if (static_cast<usize>(parent) >= map.objects.size()) {
                break;
            }
            const auto& node = map.objects[static_cast<usize>(parent)];
            if (const auto gate = entryOf(kGates, node.name)) {
                m_bindings.push_back({object, Kind::Gate, *gate / 2});
                break;
            }
            if (const auto rune = entryOf(kRunes, node.name)) {
                m_bindings.push_back({object, Kind::Rune, *rune});
                break;
            }
            if (const auto shard = entryOf(kShards, node.name)) {
                m_bindings.push_back({object, Kind::Shard, *shard});
                break;
            }
            parent = node.parent;
        }
    }
}

bool LegendsDreamcastTower::gateOpen(usize gate) const {
    // TowerInit (8c065860) applies these masks during InitLevel (8c09f6a0):
    // it hides each gate pair and clears collision immediately. Dreamcast's
    // Sumner controller (8c066640) has no N64-style realm-opening ceremony.
    if (gate < kGateObelisks.size()) {
        return (m_progress.obelisks & kGateObelisks[gate]) == kGateObelisks[gate];
    }
    // The fourth pair opens after the first Skorne victory (native boss bit5).
    return gate == 3 && (m_progress.shards & 0x20) != 0;
}

bool LegendsDreamcastTower::visible(usize object) const {
    for (const auto& binding : m_bindings) {
        if (binding.object != object) {
            continue;
        }
        switch (binding.kind) {
        case Kind::Gate: return !gateOpen(binding.index);
        case Kind::Rune: return (m_progress.runestones & (u64{1} << binding.index)) != 0;
        case Kind::Shard: return (m_progress.shards & kShardMasks[binding.index]) != 0;
        }
    }
    return true;
}

void LegendsDreamcastTower::syncCollision(WorldCollision& collision) const {
    for (const auto& binding : m_bindings) {
        if (binding.kind == Kind::Gate) {
            collision.setSolid(static_cast<s32>(binding.object), !gateOpen(binding.index));
        }
    }
}

std::vector<std::string> LegendsDreamcastTower::advice(s32 topic, const LegendsProgress& progress) {
    switch (topic) {
    case 1: return {firstMissing("obelisk", progress.obelisks, kObeliskOrder)};
    case 2: return {firstMissing("rune", progress.runestones, kRuneOrder)};
    case 3: return {firstMissing("enemy", progress.shards, kShardMasks)};
    case 0: break;
    default: return {};
    }
    // 8c065e60 history selection. Later milestones supersede earlier chapters.
    usize first = 0;
    usize count = 5;
    for (usize i = 0; i < kGateObelisks.size(); ++i) {
        if ((progress.obelisks & kGateObelisks[i]) != kGateObelisks[i]) {
            break;
        }
        first = 5 + i;
        count = 1;
    }
    // Native checks are nested: a later gate or shard alone cannot skip the
    // earlier realm chapters (e.g. with collectibles acquired out of order).
    if (first == 7 && (progress.shards & 0x1e) == 0x1e) {
        first = 8;
        if ((progress.shards & 0x20) != 0) {
            first = 9;
            count = 2;
            if ((progress.shards & 0x40) != 0) {
                first = 11;
                count = 1;
            }
        }
    }
    std::vector<std::string> result;
    for (usize i = 0; i < count; ++i) {
        result.push_back(std::format("legends.dreamcast.sumner.history.{}", first + i));
    }
    return result;
}

std::optional<s32> LegendsDreamcastSumner::update(std::span<const DreamcastSumnerVisitor> visitors,
                                                  f32 seconds, bool blocked) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return {};
    }
    const DreamcastSumnerVisitor* nearest = nullptr;
    const DreamcastSumnerVisitor* asking = nullptr;
    f32 distance = std::numeric_limits<f32>::max();
    f32 askingDistance = std::numeric_limits<f32>::max();
    for (const auto& visitor : visitors) {
        if (visitor.seat < 0 || visitor.seat >= 4 || !std::isfinite(visitor.position.x) ||
            !std::isfinite(visitor.position.y) || !std::isfinite(visitor.position.z)) {
            continue;
        }
        // 8c07eb74..8c07ec60 measures XZ proximity to (0,9.75), radius5,
        // in front of the scripted wizard rather than inside his model.
        const Vec2 away{visitor.position.x, visitor.position.z - 9.75f};
        const f32 squared = glm::dot(away, away);
        if (squared >= 25) {
            continue;
        }
        if (visitor.interactPressed &&
            (squared < askingDistance ||
             (squared == askingDistance && asking && visitor.seat < asking->seat))) {
            asking = &visitor;
            askingDistance = squared;
        }
        if (squared < distance ||
            (squared == distance && nearest && visitor.seat < nearest->seat)) {
            nearest = &visitor;
            distance = squared;
        }
    }
    const f32 target =
        nearest ? std::atan2(nearest->position.x, nearest->position.z - kPosition.z) : kPi;
    // 8c066640 case4 turns by at most ten degrees per native 30Hz update.
    const f32 delta = std::remainder(target - m_yaw, 2 * kPi);
    const f32 step = (10.0f * kPi / 180.0f) * 30 * seconds;
    m_yaw = std::remainder(m_yaw + std::clamp(delta, -step, step), 2 * kPi);
    if (!blocked && asking) {
        return asking->seat;
    }
    return {};
}

} // namespace gdl::game
