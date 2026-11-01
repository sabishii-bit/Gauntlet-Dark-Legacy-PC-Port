#include "game/rollback/EnemyPoolCheckpoint.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "game/rollback/AnimationCheckpoint.h"
#include "game/rollback/CanonicalDigestSink.h"
#include "game/rollback/RandomCheckpoint.h"

namespace gdl::game {
namespace {

/** All primitives occupy explicit four-byte words except u64 (two words).
 * Shared field visitors keep encode/decode order identical without copying C++
 * object representations, padding, native enum widths or library RNG objects. */
struct Writer {
    static constexpr bool kReading = false;
    std::vector<u8> bytes;
    CanonicalDigestSink* digest = nullptr;
    usize used = 0;

    bool word(u32 value) {
        if (used > EnemyPoolCheckpoint::kMaxBytes - 4) {
            return false;
        }
        std::array<u8, 4> encoded{};
        for (u32 shift = 0; shift < 32; shift += 8) {
            encoded[shift / 8] = static_cast<u8>(value >> shift);
        }
        return raw(encoded);
    }
    bool scalar(u32 value) { return word(value); }
    bool scalar(s32 value) { return word(std::bit_cast<u32>(value)); }
    bool scalar(f32 value) { return word(std::bit_cast<u32>(value)); }
    bool scalar(bool value) { return word(value ? 1 : 0); }
    bool scalar(u64 value) {
        return word(static_cast<u32>(value)) && word(static_cast<u32>(value >> 32));
    }
    template <class T>
        requires std::is_enum_v<T>
    bool scalar(T value) {
        return word(static_cast<u32>(value));
    }
    bool scalar(const Vec3& value) { return (*this)(value.x, value.y, value.z); }
    bool scalar(const Vec2& value) { return (*this)(value.x, value.y); }
    template <class... T> bool operator()(const T&... values) { return (scalar(values) && ...); }
    bool raw(std::span<const u8> value) {
        if (value.size() > EnemyPoolCheckpoint::kMaxBytes - used) {
            return false;
        }
        used += value.size();
        if (digest) {
            digest->append(value);
        } else {
            bytes.insert(bytes.end(), value.begin(), value.end());
        }
        return true;
    }
    bool name(const std::string& value) {
        if (value.size() > TreePose::Checkpoint::kMaxName ||
            value.size() + 4 > EnemyPoolCheckpoint::kMaxBytes - used ||
            !word(static_cast<u32>(value.size()))) {
            return false;
        }
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return raw({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    bool count(usize& value, usize maximum, usize /*unused*/, usize* budget = nullptr) {
        if (value > maximum || (budget && value > *budget)) {
            return false;
        }
        if (budget) {
            *budget -= value;
        }
        return word(static_cast<u32>(value));
    }
};

struct Reader {
    static constexpr bool kReading = true;
    std::span<const u8> bytes;
    usize offset = 0;

    usize remaining() const { return bytes.size() - offset; }
    bool word(u32& value) {
        if (remaining() < 4) {
            return false;
        }
        value = 0;
        for (u32 shift = 0; shift < 32; shift += 8) {
            value |= static_cast<u32>(bytes[offset++]) << shift;
        }
        return true;
    }
    bool scalar(u32& value) { return word(value); }
    bool scalar(s32& value) {
        u32 bits = 0;
        if (!word(bits)) {
            return false;
        }
        value = std::bit_cast<s32>(bits);
        return true;
    }
    bool scalar(f32& value) {
        u32 bits = 0;
        if (!word(bits)) {
            return false;
        }
        value = std::bit_cast<f32>(bits);
        return true;
    }
    bool scalar(bool& value) {
        u32 bits = 0;
        if (!word(bits) || bits > 1) {
            return false;
        }
        value = bits != 0;
        return true;
    }
    bool scalar(u64& value) {
        u32 low = 0;
        u32 high = 0;
        if (!(*this)(low, high)) {
            return false;
        }
        value = low | (static_cast<u64>(high) << 32);
        return true;
    }
    template <class T>
        requires std::is_enum_v<T>
    bool scalar(T& value) {
        u32 bits = 0;
        if (!word(bits) || bits > std::numeric_limits<std::underlying_type_t<T>>::max()) {
            return false;
        }
        value = static_cast<T>(bits);
        return true;
    }
    bool scalar(Vec3& value) { return (*this)(value.x, value.y, value.z); }
    bool scalar(Vec2& value) { return (*this)(value.x, value.y); }
    template <class... T> bool operator()(T&... values) { return (scalar(values) && ...); }
    std::optional<std::span<const u8>> raw(usize length) {
        if (length > remaining()) {
            return std::nullopt;
        }
        const auto value = bytes.subspan(offset, length);
        offset += length;
        return value;
    }
    bool name(std::string& value) {
        usize length = 0;
        if (!count(length, TreePose::Checkpoint::kMaxName, 1)) {
            return false;
        }
        value.clear();
        value.reserve(length);
        for (usize index = 0; index < length; ++index) {
            const u8 letter = bytes[offset++];
            if (letter == 0) {
                return false;
            }
            value.push_back(static_cast<char>(letter));
        }
        return true;
    }
    bool count(usize& value, usize maximum, usize minimumBytes, usize* budget = nullptr) {
        u32 encoded = 0;
        if (!word(encoded) || encoded > maximum || encoded > remaining() / minimumBytes ||
            (budget && encoded > *budget)) {
            return false;
        }
        value = encoded;
        if (budget) {
            *budget -= value;
        }
        return true;
    }
};

template <class IO, class T, class Fn> bool optional(IO& io, T& value, Fn visit) {
    bool present = value.has_value(); // NOLINT(misc-const-correctness): the archive reads it back
    if (!io(present)) {
        return false;
    }
    if constexpr (IO::kReading) {
        if (present) {
            value.emplace(typename T::value_type{});
        } else {
            value.reset();
        }
    }
    return !present || visit(*value);
}

template <class IO, class T, class Fn>
bool vector(IO& io, T& values, usize maximum, usize minimumBytes, Fn visit,
            usize* budget = nullptr) {
    usize count = values.size();
    if (!io.count(count, maximum, minimumBytes, budget)) {
        return false;
    }
    if constexpr (IO::kReading) {
        values.resize(count);
    }
    return std::ranges::all_of(values, [&](auto& value) { return visit(value); });
}

template <class Codec, class IO, class T> bool block(IO& io, T& value) {
    if constexpr (IO::kReading) {
        const auto bytes = io.raw(Codec::kBytes);
        const auto decoded = bytes ? Codec::decode(*bytes) : std::nullopt;
        if (!decoded) {
            return false;
        }
        value = *decoded;
        return true;
    } else {
        const auto bytes = Codec::encode(value);
        return bytes && io.raw(*bytes);
    }
}

template <class IO, class T> bool pose(IO& io, T& value) {
    return io(value.rotation, value.position, value.scale, value.rotationOrder);
}

template <class IO, class T> bool animator(IO& io, T& value, usize& nodesLeft) {
    if (!io.name(value.tree) ||
        !io(value.current, value.requested, value.walksIn, value.struck, value.powerStruck,
            value.threw, value.dead, value.idleSeconds, value.throwInterval, value.idleFraction) ||
        static_cast<usize>(value.current) >= kEnemyActionCount ||
        static_cast<usize>(value.requested) >= kEnemyActionCount ||
        !block<AnimationCheckpoint>(io, value.player) ||
        !io(value.frameBefore, value.generationBefore)) {
        return false;
    }
    // the archive reads it back
    // NOLINTNEXTLINE(misc-const-correctness)
    bool hasPose = value.pose.has_value();
    if (!io(hasPose)) {
        return false;
    }
    if (!hasPose) {
        return !value.previous && !value.presentationBefore;
    }
    if constexpr (IO::kReading) {
        value.pose.emplace();
        value.previous.emplace();
        value.presentationBefore.emplace();
        value.pose->tree = value.tree;
        value.previous->tree = value.tree;
        value.presentationBefore->tree = value.tree;
    } else {
        if (!value.previous || !value.presentationBefore ||
            value.pose->nodes != value.previous->nodes ||
            value.pose->nodes != value.presentationBefore->nodes) {
            return false;
        }
    }
    usize count = value.pose->nodes.size();
    // At least a length, parent, rest and three complete local poses per node.
    if (!io.count(count, EnemyPoolCheckpoint::kMaxNodes, 140, &nodesLeft) || count == 0) {
        return false;
    }
    if constexpr (IO::kReading) {
        value.pose->nodes.resize(count);
        value.pose->poses.resize(count);
        value.previous->poses.resize(count);
        value.presentationBefore->poses.resize(count);
    }
    for (usize index = 0; index < count; ++index) {
        auto& node = value.pose->nodes[index];
        if (!io.name(node.name) || !io(node.parent, node.rest) ||
            !pose(io, value.pose->poses[index]) || !pose(io, value.previous->poses[index]) ||
            !pose(io, value.presentationBefore->poses[index])) {
            return false;
        }
    }
    if constexpr (IO::kReading) {
        value.previous->nodes = value.pose->nodes;
        value.presentationBefore->nodes = value.pose->nodes;
    }
    return value.valid();
}

template <class IO, class T> bool mind(IO& io, T& value) {
    return io(value.effectiveWay, value.heading, value.headingBefore, value.seekHeading,
              value.route, value.collided, value.stuck, value.deadEnd, value.turns, value.counter,
              value.mode, value.fuse, value.woken, value.skirting, value.keepingOff, value.lookout,
              value.primed, value.zigZag.count, value.zigZag.side, value.zigZag.spread,
              value.zigZag.hold, value.zigZag.swings);
}

template <class IO, class T> bool body(IO& io, T& value, usize& nodesLeft) {
    return io(value.instance, value.state, value.kind, value.tier, value.algorithm, value.variant,
              value.element, value.generator, value.bred, value.birth, value.health,
              value.fullHealth, value.sight, value.radius, value.height, value.reach,
              value.position, value.birthPosition, value.presentationPosition,
              value.presentationYaw, value.presentationState) &&
           optional(io, value.floor, [&](auto& floor) { return io(floor.object, floor.local); }) &&
           io(value.yaw) && mind(io, value.mind) &&
           io(value.push, value.pushMagnitude, value.pushes, value.target, value.targetBefore,
              value.targetDistance, value.weightedDistance, value.recognized, value.contact,
              value.attackIndex, value.attackCount, value.stunTicks, value.drainTicks,
              value.endurance, value.draining, value.bumpedWall, value.bumpedOther, value.blocked,
              value.otherSide, value.expired, value.hurtPending, value.hurtFlags,
              value.hurtDirection, value.hurtBy, value.killed, value.onScreen, value.hitCount,
              value.veil, value.veilClock, value.flashSeconds, value.burnGap, value.deathSeconds,
              value.deathSkinFrames) &&
           io.name(value.deathSkin) && optional(io, value.animator, [&](auto& animation) {
               return animator(io, animation, nodesLeft);
           });
}

template <class IO, class T> bool event(IO& io, T& value) {
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Value, EnemyBlow>) {
        return io(value.player, value.kind, value.tier, value.damage, value.power, value.flags,
                  value.direction, value.ward);
    } else if constexpr (std::is_same_v<Value, EnemyLoss>) {
        return io(value.enemy, value.kind, value.tier, value.player, value.experience, value.killed,
                  value.position, value.element);
    } else if constexpr (std::is_same_v<Value, EnemyBurst>) {
        return io(value.position, value.damage, value.enemy, value.kind, value.silencesYell,
                  value.instance);
    } else if constexpr (std::is_same_v<Value, EnemyBirth>) {
        return io(value.position, value.kind, value.tier);
    } else if constexpr (std::is_same_v<Value, EnemyCue>) {
        return io(value.kind, value.enemyKind, value.position, value.enemy, value.instance);
    } else if constexpr (std::is_same_v<Value, EnemyFeedback>) {
        return io(value.kind, value.tier, value.hitCount, value.killed, value.close, value.flags,
                  value.position, value.yaw, value.halfHeight, value.enemy, value.instance,
                  value.remainingHealth);
    } else if constexpr (std::is_same_v<Value, DeathEvent>) {
        return io(value.kind, value.enemy, value.player, value.form, value.amount, value.position,
                  value.instance);
    } else if constexpr (std::is_same_v<Value, EnemyDeathShot>) {
        return io(value.enemy, value.kind, value.position, value.direction);
    } else if constexpr (std::is_same_v<Value, EnemyGeneratorEvent>) {
        return io(value.generator, value.kind);
    } else {
        static_assert(std::is_same_v<Value, s32>);
        return io(value);
    }
}

/** The count, then every one of the set's views with the point its camera looks at. */
template <class IO, class V> bool viewSet(IO& io, V& views) {
    if (!io(views.count) || views.count > ViewSet::kMost) {
        return false;
    }
    for (usize i = 0; i < ViewSet::kMost; ++i) {
        auto& eye = views.eyes[i];
        if (!io(eye.position, eye.right, eye.up, eye.forward, eye.sinX, eye.cosX, eye.sinY,
                eye.cosY, views.attentions[i])) {
            return false;
        }
    }
    return true;
}

template <class IO, class T> bool pool(IO& io, T& state) {
    // the archive reads them back
    // NOLINTNEXTLINE(misc-const-correctness)
    u32 version = EnemyPoolCheckpoint::kVersion;
    // NOLINTNEXTLINE(misc-const-correctness)
    u32 slots = Enemies::kMost;
    if (!io(version, slots) || version != EnemyPoolCheckpoint::kVersion ||
        slots != Enemies::kMost || !block<RandomCheckpoint>(io, state.random) ||
        !io(state.nextInstance, state.most, state.scales.health, state.scales.speed,
            state.scales.sight, state.scales.damage, state.scales.missileRate,
            state.scales.missileAim, state.scales.playerLevel, state.scales.bossEncounter,
            state.scales.players, state.contactGridOrigin, state.contactGridInverseWidth,
            state.bomber, state.inView, state.frame, state.pendingTicks, state.pendingSeconds,
            state.shrink) ||
        !optional(io, state.view, [&](auto& views) { return viewSet(io, views); })) {
        return false;
    }
    if (!vector(io, state.stocks, kEnemyKindCount, 16, [&](auto& stock) {
            return io(stock.kind, stock.textures.frame, stock.textures.remainder,
                      stock.textures.advance);
        })) {
        return false;
    }
    usize points = state.lookouts.points.size();
    if (!io.count(points, LookoutRoute::kMost, 16)) {
        return false;
    }
    if constexpr (IO::kReading) {
        state.lookouts.points.resize(points);
        state.lookouts.next.resize(points);
    }
    for (usize index = 0; index < points; ++index) {
        if (!io(state.lookouts.points[index], state.lookouts.next[index])) {
            return false;
        }
    }
    if (!vector(io, state.combatants, EnemyPoolCheckpoint::kMaxCombatants, 24, [&](auto& contact) {
            return io(contact.id, contact.centre, contact.radius, contact.halfHeight);
        })) {
        return false;
    }
    usize eventsLeft = EnemyPoolCheckpoint::kMaxTotalEvents;
    const auto events = [&](auto& values, usize minimumBytes) {
        return vector(
            io, values, EnemyPoolCheckpoint::kMaxEvents, minimumBytes,
            [&](auto& value) { return event(io, value); }, &eventsLeft);
    };
    if (!events(state.blows, 40) || !events(state.losses, 36) || !events(state.bursts, 36) ||
        !events(state.births, 20) || !events(state.cues, 32) || !events(state.feedback, 56) ||
        !events(state.deathEvents, 40) || !events(state.deathShots, 32) ||
        !events(state.tagged, 4) || !events(state.generatorEvents, 8)) {
        return false;
    }
    usize nodesLeft = EnemyPoolCheckpoint::kMaxTotalNodes;
    for (auto& enemy : state.enemies) {
        if (!body(io, enemy, nodesLeft)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::vector<u8>> EnemyPoolCheckpoint::encode(const Enemies::Checkpoint& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    Writer writer;
    if (!pool(writer, state)) {
        return std::nullopt;
    }
    return std::move(writer.bytes);
}

std::optional<std::array<u8, 32>>
EnemyPoolCheckpoint::canonicalDigest(const Enemies::Checkpoint& state) {
    CanonicalDigestSink sink;
    Writer writer;
    writer.digest = &sink;
    return pool(writer, state) ? std::optional{sink.finish()} : std::nullopt;
}

std::optional<Enemies::Checkpoint> EnemyPoolCheckpoint::decode(std::span<const u8> bytes) {
    if (bytes.size() > kMaxBytes || bytes.size() < 8 + RandomCheckpoint::kBytes) {
        return std::nullopt;
    }
    Reader reader{bytes};
    Enemies::Checkpoint state;
    if (!pool(reader, state) || reader.remaining() != 0 || !state.valid()) {
        return std::nullopt;
    }
    return state;
}

} // namespace gdl::game
