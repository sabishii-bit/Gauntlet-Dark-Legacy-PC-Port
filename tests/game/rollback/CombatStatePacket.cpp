#include "game/rollback/CombatStatePacket.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/rollback/AnimationCheckpoint.h"
#include "game/rollback/CanonicalDigestSink.h"
#include "game/rollback/PlayerHealthCheckpoint.h"
#include "game/rollback/PlayerRuntimePacket.h"
#include "game/rollback/RandomCheckpoint.h"
#include "game/rollback/StageStatePacket.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("CBST");

/** Explicit little-endian scalars, never a native object representation. Small
 * scalars and booleans occupy one word; native indices occupy eight bytes. */
class Writer {
public:
    static constexpr bool reading = false; // NOLINT(readability-identifier-naming): archive trait
    std::vector<u8> bytes;
    CanonicalDigestSink* digest = nullptr;
    bool agreement = false;
    usize used = 0;
    usize records = 0;
    usize strings = 0;

    void raw(std::span<const u8> source) {
        if (source.size() > remaining()) {
            throw FormatError("combat checkpoint byte limit");
        }
        used += source.size();
        if (digest) {
            digest->append(source);
        } else {
            bytes.insert(bytes.end(), source.begin(), source.end());
        }
    }
    void word(u32 value) {
        std::array<u8, 4> data{};
        for (usize index = 0; index < data.size(); ++index) {
            data[index] = static_cast<u8>(value >> (index * 8));
        }
        raw(data);
    }
    void text(std::string_view value) {
        if (value.size() > remaining()) {
            throw FormatError("combat checkpoint byte limit");
        }
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        raw({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    template <class T> void number(T value) {
        if constexpr (std::is_enum_v<T>) {
            number(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            word(value ? 1U : 0U);
        } else if constexpr (std::is_same_v<T, f32>) {
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite combat checkpoint scalar");
            }
            word(std::bit_cast<u32>(value));
        } else if constexpr (std::is_same_v<T, f64>) {
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite combat checkpoint clock");
            }
            number(std::bit_cast<u64>(value));
        } else if constexpr (sizeof(T) == 8) {
            static_assert(std::is_integral_v<T>);
            auto bits = static_cast<u64>(
                value); // NOLINT(misc-const-correctness): the archive reads it back
            word(static_cast<u32>(bits));
            word(static_cast<u32>(bits >> 32));
        } else if constexpr (std::is_signed_v<T>) {
            static_assert(std::is_integral_v<T>);
            word(std::bit_cast<u32>(static_cast<s32>(value)));
        } else {
            static_assert(std::is_integral_v<T>);
            word(static_cast<u32>(value));
        }
    }
    usize remaining() const { return CombatStatePacket::kMaxBytes - used; }
};

class Reader {
public:
    static constexpr bool reading = true; // NOLINT(readability-identifier-naming): archive trait
    usize records = 0;
    usize strings = 0;

    explicit Reader(std::span<const u8> bytes) : m_reader(bytes) {}
    std::span<const u8> raw(usize count) { return m_reader.readBytes(count); }
    template <class T> void number(T& value) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> decoded{};
            number(decoded);
            value = static_cast<T>(decoded);
        } else if constexpr (std::is_same_v<T, bool>) {
            auto bits =
                m_reader.readU32(); // NOLINT(misc-const-correctness): the archive reads it back
            if (bits > 1) {
                throw FormatError("noncanonical combat checkpoint boolean");
            }
            value = bits != 0;
        } else if constexpr (std::is_same_v<T, f32>) {
            value = std::bit_cast<f32>(m_reader.readU32());
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite combat checkpoint scalar");
            }
        } else if constexpr (std::is_same_v<T, f64>) {
            u64 bits = 0;
            number(bits);
            value = std::bit_cast<f64>(bits);
            if (!std::isfinite(value)) {
                throw FormatError("nonfinite combat checkpoint clock");
            }
        } else if constexpr (sizeof(T) == 8) {
            static_assert(std::is_integral_v<T>);
            const u64 low = m_reader.readU32();
            const u64 high = m_reader.readU32();
            value = std::bit_cast<T>(low | (high << 32));
        } else if constexpr (std::is_signed_v<T>) {
            static_assert(std::is_integral_v<T>);
            const s32 decoded = std::bit_cast<s32>(m_reader.readU32());
            if (decoded < std::numeric_limits<T>::min() ||
                decoded > std::numeric_limits<T>::max()) {
                throw FormatError("combat checkpoint signed scalar overflow");
            }
            value = static_cast<T>(decoded);
        } else {
            static_assert(std::is_integral_v<T>);
            const u32 decoded = m_reader.readU32();
            if (decoded > std::numeric_limits<T>::max()) {
                throw FormatError("combat checkpoint unsigned scalar overflow");
            }
            value = static_cast<T>(decoded);
        }
    }
    usize remaining() const { return m_reader.remaining(); }

private:
    ByteReader m_reader;
};

template <class T> struct IsVector : std::false_type {};
template <class T, class Allocator> struct IsVector<std::vector<T, Allocator>> : std::true_type {};
template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};
template <class T> struct IsArray : std::false_type {};
template <class T, usize N> struct IsArray<std::array<T, N>> : std::true_type {};

template <class A, class T> void field(A& archive, T& value);
/** These optional poses are overwritten by draw before use. The agreement
 * visitor alone elides them; wire and native restore retain and validate them. */
template <class A, class T> void drawCache(A& a, T& value) {
    if constexpr (!A::reading) {
        if (a.agreement) {
            const std::remove_cv_t<T> empty{};
            field(a, empty);
            return;
        }
    }
    field(a, value);
}

template <class A, class... T> void fields(A& archive, T&... value) {
    (field(archive, value), ...);
}
template <class A, class T> void index(A& archive, T& value) {
    u64 bits = value; // NOLINT(misc-const-correctness): the archive reads it back
    archive.number(bits);
    if constexpr (A::reading) {
        if (bits > std::numeric_limits<usize>::max()) {
            throw FormatError("combat checkpoint index overflow");
        }
        value = static_cast<usize>(bits);
    }
}
template <class A, class T> void optionalIndex(A& archive, T& value) {
    bool present = value.has_value(); // NOLINT(misc-const-correctness): the archive reads it back
    archive.number(present);
    if constexpr (A::reading) {
        if (present) {
            value.emplace();
        } else {
            value.reset();
        }
    }
    if (present) {
        index(archive, *value);
    }
}
template <class T> constexpr usize recordLimit() {
    if constexpr (std::is_same_v<T, PlayerRuntimeCheckpoint::State> ||
                  std::is_same_v<T, PlayerAttacksCheckpoint::Actor> ||
                  std::is_same_v<T, LevelWatch::Mark>) {
        return 4;
    } else if constexpr (std::is_same_v<T, EffectTrees::PoolCheckpoint::Entry>) {
        return CombatStatePacket::kMaxEffects;
    } else if constexpr (std::is_same_v<T, CritterStatues::Checkpoint::Definition> ||
                         std::is_same_v<T, CritterStatues::Checkpoint::Body> ||
                         std::is_same_v<T, CritterStatues::Placement>) {
        return CritterStatues::Checkpoint::kMaxStatues;
    } else if constexpr (std::is_same_v<T, EffectTrees::PoolCheckpoint::Bank>) {
        return CombatStatePacket::kMaxBanks;
    } else if constexpr (std::is_same_v<T, TreePose::Checkpoint::Node> ||
                         std::is_same_v<T, NodePose>) {
        return TreePose::Checkpoint::kMaxNodes;
    } else if constexpr (std::is_same_v<T, CrittersCheckpoint::Bank> ||
                         std::is_same_v<T, CombatantProjectilesCheckpoint::Catalog>) {
        return CrittersCheckpoint::kMaxStocks;
    } else if constexpr (std::is_same_v<T, CombatantCheckpoint::Part> ||
                         std::is_same_v<T, CombatantCheckpoint::Attachment>) {
        return CombatantCheckpoint::kMaxParts;
    } else if constexpr (std::is_same_v<T, CombatantProjectilesCheckpoint::Flight>) {
        return CombatantProjectilesCheckpoint::kMaxFlights;
    } else if constexpr (std::is_same_v<T, Particle>) {
        return ParticleDescriptor::kMostParticles;
    } else {
        return CombatStatePacket::kMaxRecords;
    }
}

/** Only native move/pattern cooldowns use -infinity as a never-used sentinel.
 * Preserve its exact bits without permitting other nonfinite scalar fields. */
template <class A, class T> void cooldowns(A& a, T& values) {
    u32 count = 0;
    if constexpr (!A::reading) {
        if (values.size() > CombatantCheckpoint::kMaxRows) {
            throw FormatError("combatant cooldown limit");
        }
        count = static_cast<u32>(values.size());
    }
    a.number(count);
    if (count > CombatantCheckpoint::kMaxRows || count > a.remaining() / 4 ||
        count > CombatStatePacket::kMaxRecords - a.records) {
        throw FormatError("combatant cooldown budget");
    }
    a.records += count;
    if constexpr (A::reading) {
        values.resize(count);
    }
    for (auto& value : values) {
        u32 bits = 0;
        if constexpr (!A::reading) {
            bits = std::bit_cast<u32>(value);
        }
        a.number(bits);
        const f32 decoded = std::bit_cast<f32>(bits);
        if (!std::isfinite(decoded) && decoded != -std::numeric_limits<f32>::infinity()) {
            throw FormatError("invalid combatant cooldown");
        }
        if constexpr (A::reading) {
            value = decoded;
        }
    }
}

template <class Codec, class A, class T>
void leaf(A& archive, T& value, std::optional<usize> fixed = std::nullopt) {
    if constexpr (A::reading) {
        u32 length = 0;
        if (fixed) {
            length = static_cast<u32>(*fixed);
        } else {
            archive.number(length);
        }
        if (length > archive.remaining()) {
            throw FormatError("truncated combat checkpoint leaf");
        }
        auto decoded = Codec::decode(archive.raw(length));
        if (!decoded) {
            throw FormatError("invalid combat checkpoint leaf");
        }
        value = std::move(*decoded);
    } else {
        const auto encoded = Codec::encode(value);
        if (!encoded || encoded->size() > archive.remaining() ||
            (fixed && encoded->size() != *fixed)) {
            throw FormatError("unencodable combat checkpoint leaf");
        }
        if (!fixed) {
            archive.number(static_cast<u32>(encoded->size()));
        }
        archive.raw(*encoded);
    }
}

/** The same explicit field order drives both directions, including every queued
 * contact, random stream, native pool identity and fractional particle clock. */
template <class A, class T> void field(A& a, T& v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        a.number(v);
    } else if constexpr (std::is_same_v<V, Vec2>) {
        fields(a, v.x, v.y);
    } else if constexpr (std::is_same_v<V, Vec3>) {
        fields(a, v.x, v.y, v.z);
    } else if constexpr (std::is_same_v<V, Mat4>) {
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 4; ++row) {
                field(a, v[column][row]);
            }
        }
    } else if constexpr (std::is_same_v<V, Color>) {
        fields(a, v.r, v.g, v.b, v.a);
    } else if constexpr (std::is_same_v<V, std::string>) {
        u32 count = 0;
        if constexpr (!A::reading) {
            if (v.size() > CombatStatePacket::kMaxString) {
                throw FormatError("combat checkpoint string limit");
            }
            count = static_cast<u32>(v.size());
        }
        a.number(count);
        if (count > CombatStatePacket::kMaxString || count > a.remaining() ||
            count > CombatStatePacket::kMaxStringBytes - a.strings) {
            throw FormatError("combat checkpoint string budget");
        }
        a.strings += count;
        if constexpr (A::reading) {
            const auto data = a.raw(count);
            v.assign(data.begin(), data.end());
        } else {
            a.text(v);
        }
    } else if constexpr (IsVector<V>::value) {
        u32 count = 0;
        if constexpr (!A::reading) {
            if (v.size() > recordLimit<typename V::value_type>()) {
                throw FormatError("combat checkpoint record limit");
            }
            count = static_cast<u32>(v.size());
        }
        a.number(count);
        // Every record in this schema occupies at least a word. Check before
        // allocation, and share the record budget across all nested containers.
        if (count > recordLimit<typename V::value_type>() ||
            count > CombatStatePacket::kMaxRecords - a.records || count > a.remaining() / 4) {
            throw FormatError("combat checkpoint aggregate record budget");
        }
        a.records += count;
        if constexpr (A::reading) {
            v.resize(count);
        }
        for (auto& entry : v) {
            field(a, entry);
        }
    } else if constexpr (IsOptional<V>::value) {
        bool present = v.has_value(); // NOLINT(misc-const-correctness): the archive reads it back
        a.number(present);
        if constexpr (A::reading) {
            if (present) {
                v.emplace(typename V::value_type{});
            } else {
                v.reset();
            }
        }
        if (present) {
            field(a, *v);
        }
    } else if constexpr (IsArray<V>::value) {
        for (auto& entry : v) {
            field(a, entry);
        }
    } else if constexpr (std::is_same_v<V, std::pair<s32, s32>>) {
        fields(a, v.first, v.second);
    } else if constexpr (std::is_same_v<V, PlayerRuntimeCheckpoint::State>) {
        leaf<PlayerRuntimePacket>(a, v);
    } else if constexpr (std::is_same_v<V, PlayerHealth::Checkpoint>) {
        leaf<PlayerHealthCheckpoint>(a, v, PlayerHealthCheckpoint::kBytes);
    } else if constexpr (std::is_same_v<V, AnimationPlayer::Checkpoint>) {
        leaf<AnimationCheckpoint>(a, v, AnimationCheckpoint::kBytes);
    } else if constexpr (std::is_same_v<V, Mt19937::Checkpoint>) {
        leaf<RandomCheckpoint>(a, v, RandomCheckpoint::kBytes);
    } else if constexpr (std::is_same_v<V, CombatStatePacket::State>) {
        fields(a, v.combat, v.adapter);
        // the archive reads it back
        // NOLINTNEXTLINE(misc-const-correctness)
        bool present = v.stage.has_value();
        a.number(present);
        if (present) {
            u32 length = 0;
            if constexpr (!A::reading) {
                if (!StageStatePacket::envelopeValid(*v.stage)) {
                    throw FormatError("invalid stage checkpoint envelope");
                }
                length = static_cast<u32>(v.stage->size());
            }
            a.number(length);
            if (length > a.remaining() || length > StageStatePacket::kMaxBytes ||
                length < StageStatePacket::kHeaderBytes) {
                throw FormatError("stage checkpoint byte limit");
            }
            if constexpr (A::reading) {
                const auto bytes = a.raw(length);
                if (!StageStatePacket::envelopeValid(bytes)) {
                    throw FormatError("invalid stage checkpoint envelope");
                }
                v.stage.emplace(bytes.begin(), bytes.end());
            } else {
                a.raw(*v.stage);
            }
        } else if constexpr (A::reading) {
            v.stage.reset();
        }
    } else if constexpr (std::is_same_v<V, CombatRollbackCheckpoint::State>) {
        fields(a, v.players, v.health, v.levels, v.attacks, v.arsenal, v.effects);
    } else if constexpr (std::is_same_v<V, LevelWatch::Checkpoint>) {
        field(a, v.marks);
    } else if constexpr (std::is_same_v<V, LevelWatch::Mark>) {
        fields(a, v.player, v.level);
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::State>) {
        fields(a, v.hitStreak, v.lessons, v.generatorRewards, v.rockHits, v.hearers, v.cueEffects,
               v.shrink, v.bagSeed, v.statues, v.critters, v.projectiles, v.critterExperienceOwed,
               v.critterEffects, v.moveEffects, v.barrelRams);
        for (auto& item : v.carried) {
            optionalIndex(a, item);
        }
        field(a, v.bags);
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::Reward>) {
        fields(a, v.player, v.experience, v.destroyed);
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::State::FollowingEffect>) {
        fields(a, v.effect, v.critter, v.ofBoss, v.offset, v.node, v.nodeOffset, v.rootAttachment,
               v.pitchYaw, v.playerAttachment);
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::State::MoveEffect>) {
        fields(a, v.effect, v.critter, v.ofBoss);
        // NOLINTNEXTLINE(bugprone-branch-clone): one branch per state type
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::State::Ram>) {
        fields(a, v.id, v.damage);
    } else if constexpr (std::is_same_v<V, OpponentCombatCheckpoint::State::Bag>) {
        optionalIndex(a, v.item);
        fields(a, v.name, v.position, v.velocity, v.age, v.pitch, v.spin, v.effect, v.landed);
    } else if constexpr (std::is_same_v<V, CrittersCheckpoint::State>) {
        fields(a, v.scales, v.realm, v.gargoyleForm, v.lookouts, v.banks, v.actors, v.events);
    } else if constexpr (std::is_same_v<V, CrittersCheckpoint::Bank>) {
        fields(a, v.id, v.kind, v.name, v.tree, v.clock);
    } else if constexpr (std::is_same_v<V, EnemyScales>) {
        fields(a, v.health, v.speed, v.sight, v.damage, v.missileRate, v.missileAim, v.playerLevel,
               v.bossEncounter, v.players);
    } else if constexpr (std::is_same_v<V, LookoutRoute>) {
        fields(a, v.points, v.next);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::State>) {
        fields(a, v.parts, v.lookouts);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Part>) {
        fields(a, v.actor, v.incarnation, v.id, v.collision, v.hazards, v.scales, v.realm,
               v.fullHarm, v.arenaRandom, v.events);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Definition>) {
        fields(a, v.resource, v.index);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Actor>) {
        fields(a, v.state, v.hidden, v.forcedPattern, v.childrenIntact, v.smoothValid, v.definition,
               v.parent, v.branch, v.health, v.burnGap, v.maxHealth, v.position, v.floor, v.yaw,
               v.initialYaw, v.homePosition, v.initialRoot, v.push, v.target, v.targetDistance,
               v.targetYaw, v.targets, v.playerDamage, v.move, v.moveTarget, v.pattern,
               v.finishedSeconds, v.age);
        cooldowns(a, v.moveTimes);
        cooldowns(a, v.patternTimes);
        fields(a, v.patternStep, v.struckThisMove, v.areas, v.arenaAnchors, v.arenaTargets,
               v.patrolBound, v.lookout, v.sight, v.lastArenaTarget, v.hurtPending, v.hurtFlags,
               v.hurtDirection, v.roarOwed, v.sinceHurt, v.flashTicks, v.skin, v.skinAge,
               v.hitNodes, v.replacements, v.alpha, v.tint, v.scale, v.shrink, v.frozenTicks,
               v.blindTicks, v.curbSeconds, v.held, v.roarWanted, v.moveEffect, v.moveDone,
               v.soundsGiven, v.arenaCollected, v.shotFrame, v.attackTarget, v.stepTarget,
               v.patrolAim, v.grabbed, v.grabMove, v.grabNode, v.grabOffset, v.player, v.pose,
               v.smooth, v.presentation, v.attachments, v.gaze);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Floor>) {
        fields(a, v.object, v.local);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Target>) {
        fields(a, v.player, v.distance, v.score, v.inverseAnger, v.direction);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::PlayerDamage>) {
        fields(a, v.dealt, v.received, v.dealtTime, v.receivedTime);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::HitNode>) {
        fields(a, v.health, v.flashTicks, v.broken, v.heldPose);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Replacement>) {
        fields(a, v.node, v.sourceNode, v.object);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Presentation>) {
        fields(a, v.valid, v.state, v.generation, v.sequence, v.position, v.yaw, v.frame, v.pose);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Attachment>) {
        fields(a, v.player, v.pose, v.world, v.previousFrame, v.previousGeneration);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Shot>) {
        fields(a, v.definition, v.critter, v.damageIndex, v.origin, v.forward, v.target, v.rate,
               v.scale, v.damageScale, v.birthLife, v.endVisual, v.realm);
    } else if constexpr (std::is_same_v<V, CombatantCheckpoint::Events>) {
        fields(a, v.cues, v.grabs, v.spews, v.shots, v.arena, v.tramples, v.rams, v.pushes, v.blows,
               v.losses);
    } else if constexpr (std::is_same_v<V, CombatCue>) {
        fields(a, v.critter, v.tree, v.sound, v.attenuated, v.soundPosition, v.position, v.yaw,
               v.scale, v.life, v.follows, v.shakes, v.arena, v.untilNextMove, v.stopMoveEffect,
               v.rootAttachment, v.node, v.nodeOffset, v.pitchYaw, v.loop, v.placement,
               v.playerAttachment);
    } else if constexpr (std::is_same_v<V, CombatGrab>) {
        fields(a, v.player, v.critter, v.attachment, v.velocity, v.damage, v.restoreSupport);
    } else if constexpr (std::is_same_v<V, CombatSpew>) {
        fields(a, v.critter, v.origin, v.velocity, v.halfAngle);
    } else if constexpr (std::is_same_v<V, CombatArenaActivation>) {
        index(a, v.index);
        field(a, v.delay);
    } else if constexpr (std::is_same_v<V, CombatTrample>) {
        fields(a, v.enemy, v.damage, v.position);
    } else if constexpr (std::is_same_v<V, CombatantRam>) {
        fields(a, v.id, v.damage);
    } else if constexpr (std::is_same_v<V, CombatPush>) {
        fields(a, v.player, v.velocity);
    } else if constexpr (std::is_same_v<V, CombatBlow>) {
        fields(a, v.player, v.critter, v.damage, v.direction, v.breath, v.gated, v.flags, v.origin,
               v.area, v.repeatGap);
    } else if constexpr (std::is_same_v<V, CombatLoss>) {
        fields(a, v.critter, v.kind, v.form, v.player, v.experience, v.killed, v.drop, v.position);
    } else if constexpr (std::is_same_v<V, CombatArenaTarget>) {
        index(a, v.index);
        fields(a, v.placement, v.active);
    } else if constexpr (std::is_same_v<V, CritterArea>) {
        fields(a, v.local, v.node, v.worldParent, v.velocity, v.displacement, v.radius, v.minDot,
               v.damage, v.flags, v.secondsLeft, v.lifetime, v.expanding, v.linearGrowth,
               v.halfHeight, v.growFraction, v.shrinkFraction);
    } else if constexpr (std::is_same_v<V, CombatantProjectilesCheckpoint::State>) {
        fields(a, v.catalog, v.flying, v.emittedEffects, v.hits, v.worldHits, v.rockHits,
               v.generators, v.summons, v.random, v.ricochetIn, v.nextInstance);
    } else if constexpr (std::is_same_v<V, CombatantProjectilesCheckpoint::Catalog>) {
        fields(a, v.id, v.kind, v.name, v.children);
    } else if constexpr (std::is_same_v<V, CombatantProjectilesCheckpoint::Flight>) {
        fields(a, v.shot, v.archive, v.position, v.velocity, v.spin, v.rotation, v.effect,
               v.morphed, v.stuck, v.planted, v.phaseSeconds, v.settled, v.impactRadius,
               v.impactArea, v.leavesGenerator, v.summonsEnemies, v.contactSeconds, v.piercedPlayer,
               v.reflected, v.instance);
    } else if constexpr (std::is_same_v<V, CombatantProjectileHit>) {
        fields(a, v.player, v.damage, v.flags, v.direction, v.repeatGap, v.critter, v.ownerKind,
               v.target, v.ricochet, v.position, v.instance);
    } else if constexpr (std::is_same_v<V, CombatantWorldHit>) {
        fields(a, v.position, v.object, v.splash, v.instance);
    } else if constexpr (std::is_same_v<V, RockHit>) {
        index(a, v.rock);
        field(a, v.damage);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::Actor>) {
        fields(a, v.player, v.character);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State>) {
        fields(a, v.bound, v.actors, v.strikes, v.items, v.potions, v.strikeEffects,
               v.strikeSources, v.attachments, v.particles, v.shields);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::ItemArea>) {
        field(a, v.item);
        index(a, v.actor);
        fields(a, v.effect, v.elapsed, v.lifetime, v.pojoTurbo, v.hit);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::Potion>) {
        fields(a, v.impact, v.elapsed, v.duration, v.hit, v.blessed, v.struck);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::StrikeEffect>) {
        fields(a, v.strike, v.effect);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::StrikeSource>) {
        field(a, v.strike);
        index(a, v.actor);
        fields(a, v.row, v.contacts, v.multiplayer, v.damageType);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::StrikeSource::Contact>) {
        fields(a, v.target, v.remaining);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::Attachment>) {
        index(a, v.actor);
        fields(a, v.effect, v.strike, v.local, v.bodyParent);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::Particle>) {
        index(a, v.actor);
        fields(a, v.effect, v.parent, v.node, v.offset);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::Shield>) {
        index(a, v.actor);
        fields(a, v.flags, v.effect, v.radius, v.damage, v.secondsLeft, v.marks, v.blessed);
    } else if constexpr (std::is_same_v<V, PlayerAttacksCheckpoint::State::Shield::Mark>) {
        fields(a, v.kind, v.id, v.wait);
    } else if constexpr (std::is_same_v<V, MoveStrikes::Checkpoint>) {
        fields(a, v.strikes, v.next);
    } else if constexpr (std::is_same_v<V, MoveStrikes::Strike>) {
        fields(a, v.id, v.owner, v.flies, v.expanding, v.collidesWorld, v.position, v.facing,
               v.speed, v.radius, v.arc, v.damage, v.delayLeft, v.launchIn, v.secondsLeft,
               v.damageTime);
    } else if constexpr (std::is_same_v<V, PlayerArsenalCheckpoint::State>) {
        fields(a, v.bound, v.nextPotionKind, v.casts, v.missiles);
    } else if constexpr (std::is_same_v<V, PlayerArsenalCheckpoint::State::Cast>) {
        fields(a, v.owner, v.effect);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::Checkpoint>) {
        fields(a, v.missiles, v.impacts, v.history, v.visuals, v.nextInstance, v.ricochetIn,
               v.visualsBound);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::Checkpoint::Flight>) {
        fields(a, v.instance, v.continuity, v.owner, v.position, v.velocity, v.tumble, v.age,
               v.previousPosition, v.previousVelocity, v.previousTumble, v.previousAge,
               v.presentationCaptured, v.potion, v.potency, v.damage, v.scale, v.resource, v.effect,
               v.rider, v.wallSound, v.flags, v.pierced, v.release, v.streakColor, v.streakForward,
               v.potionRelease, v.multiplayer, v.playerHitGap, v.breaksPotions, v.playerContacts);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::Missile::PlayerContact>) {
        fields(a, v.player, v.remaining);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::Checkpoint::Impact>) {
        fields(a, v.position, v.owner, v.potion, v.potency, v.damage, v.target, v.effectResource,
               v.wallSound, v.flags, v.node, v.worldObject, v.liquid, v.player, v.direction, v.stun,
               v.playerHitGap);
    } else if constexpr (std::is_same_v<V, MissileRelease>) {
        fields(a, v.character, v.generation, v.strong, v.ordinal, v.emission);
    } else if constexpr (std::is_same_v<V, MissilePotionRelease>) {
        field(a, v.generation);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::VisualHistory>) {
        fields(a, v.instance, v.owner, v.character, v.release, v.effect, v.rider, v.flight,
               v.placement, v.direction, v.potion, v.potionRelease);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::ParticleFlight>) {
        fields(a, v.birth, v.stepSeconds, v.activeSteps, v.totalSteps, v.retiring);
    } else if constexpr (std::is_same_v<V, PlayerMissiles::FlightCheckpoint>) {
        fields(a, v.position, v.velocity, v.tumble, v.age, v.scale, v.continuity);
    } else if constexpr (std::is_same_v<V, EffectTrees::PoolCheckpoint>) {
        fields(a, v.effects, v.banks, v.presentationAdvanced, v.nextId, v.nextInstance);
    } else if constexpr (std::is_same_v<V, EffectTrees::PoolCheckpoint::Entry>) {
        fields(a, v.resource, v.id, v.instance, v.state, v.provenance, v.previousTransform,
               v.previousDirection, v.previousTree, v.previousGeneration, v.previousFrame,
               v.presentationCaptured, v.previousResource);
    } else if constexpr (std::is_same_v<V, EffectTrees::PoolCheckpoint::Bank>) {
        fields(a, v.resource, v.clock);
    } else if constexpr (std::is_same_v<V, EffectTrees::Provenance>) {
        fields(a, v.player, v.generation, v.occurrence, v.effectOrdinal);
    } else if constexpr (std::is_same_v<V, EffectTrees::Checkpoint>) {
        fields(a, v.name, v.tree, v.continuity, v.setting, v.position, v.timed, v.retiring, v.lived,
               v.animation, v.trails, v.particles, v.attachment, v.flightDirection,
               v.particleTextureSlot);
    } else if constexpr (std::is_same_v<V, EffectTrees::Setting>) {
        fields(a, v.scale, v.stretch, v.fadeSeconds, v.yaw, v.velocity, v.seconds, v.loop,
               v.persistent, v.emitParticles, v.missingObjectsAreEmpty, v.settled, v.additive,
               v.then, v.morphIn, v.holdForMorph, v.shrinks, v.unlit, v.depthWrite, v.tint, v.alpha,
               v.playbackRate, v.light);
    } else if constexpr (std::is_same_v<V, EffectTrees::Light>) {
        fields(a, v.color, v.radius, v.swells);
    } else if constexpr (std::is_same_v<V, TextureAnimator::Checkpoint>) {
        fields(a, v.frame, v.remainder, v.advance);
    } else if constexpr (std::is_same_v<V, ParticleEnvelope>) {
        fields(a, v.lifeStart, v.lifeEnd, v.fadeStart, v.fadeEnd);
    } else if constexpr (std::is_same_v<V, ParticleDescriptor>) {
        fields(a, v.delay, v.emitFrames, v.fadeFrames, v.forever, v.oneShot, v.dynamic, v.angle,
               v.direction, v.volume, v.rate, v.rateRandom, v.gravity, v.drag, v.speed,
               v.maxParticles, v.maxDirections, v.maxPositions, v.particleLife, v.particleFade,
               v.red, v.green, v.blue, v.alpha, v.width, v.texture, v.additive, v.multiply,
               v.sorted, v.depthTest, v.depthWrite);
    } else if constexpr (std::is_same_v<V, Particle>) {
        fields(a, v.origin, v.velocity, v.age);
    } else if constexpr (std::is_same_v<V, ParticleEmitter::Checkpoint>) {
        fields(a, v.descriptor, v.node, v.particles, v.phase, v.age, v.saved, v.emitting,
               v.spriteScale, v.random);
    } else if constexpr (std::is_same_v<V, ParticleField::Checkpoint>) {
        fields(a, v.entries, v.frameRemainder, v.lastAdvance, v.heatFrames);
    } else if constexpr (std::is_same_v<V, ParticleField::Checkpoint::Entry>) {
        fields(a, v.emitter, v.worldObject);
    } else if constexpr (std::is_same_v<V, TreeParticles::Checkpoint>) {
        fields(a, v.field, v.nodes);
    } else if constexpr (std::is_same_v<V, TreeParticles::Binding>) {
        index(a, v.node);
        field(a, v.texture);
    } else if constexpr (std::is_same_v<V, CritterStatues::Checkpoint>) {
        fields(a, v.roster, v.statues, v.risen, v.contactGridOrigin, v.contactGridInverseWidth,
               v.nextInstance);
    } else if constexpr (std::is_same_v<V, CritterStatues::Checkpoint::Definition>) {
        fields(a, v.instance, v.placement);
    } else if constexpr (std::is_same_v<V, CritterStatues::Checkpoint::Body>) {
        fields(a, v.instance, v.figure, v.woken, v.rising, v.ticksLeft);
    } else if constexpr (std::is_same_v<V, CritterStatues::Placement>) {
        fields(a, v.kind, v.enemy, v.form, v.instance, v.radius, v.height, v.viewRadius, v.sight,
               v.activeOn, v.onFloor);
        optionalIndex(a, v.carried);
        fields(a, v.support, v.supportLocal);
    } else if constexpr (std::is_same_v<V, EnemySpawn>) {
        fields(a, v.zigZagSide, v.kind, v.tier, v.algorithm, v.position, v.direction, v.clearance,
               v.generator, v.placed, v.patrolBirth, v.allBirthDirections, v.asleep,
               v.throwInterval, v.priority, v.sight);
    } else if constexpr (std::is_same_v<V, ItemInstance>) {
        fields(a, v.info, v.minPlayers, v.flags, v.name, v.position, v.rotation, v.params,
               v.collision);
    } else if constexpr (std::is_same_v<V, ItemCollisionTriangle>) {
        fields(a, v.normal, v.vertices);
    } else if constexpr (std::is_same_v<V, ItemFigure::Checkpoint>) {
        fields(a, v.animation, v.pose);
        drawCache(a, v.presentationPose);
        fields(a, v.textures, v.particles, v.poseSequence, v.poseFrame, v.poseGeneration,
               v.continuity, v.previousTransform, v.previousFrame, v.previousGeneration,
               v.presentationCaptured, v.presentationAdvanced, v.textureSequence, v.textureFrame,
               v.gateParticles, v.position, v.yaw, v.transform, v.placement, v.index, v.loop,
               v.holdPose);
    } else if constexpr (std::is_same_v<V, TreePose::Checkpoint>) {
        fields(a, v.tree, v.nodes, v.poses);
    } else if constexpr (std::is_same_v<V, TreePose::Checkpoint::Node>) {
        fields(a, v.name, v.parent, v.rest);
    } else if constexpr (std::is_same_v<V, NodePose>) {
        fields(a, v.rotation, v.position, v.scale, v.rotationOrder);
    } else {
        static_assert(std::is_same_v<V, void>, "combat checkpoint field lacks an explicit codec");
    }
}
} // namespace

bool CombatStatePacket::State::valid() const {
    return combat.valid() && adapter.valid() &&
           (!stage || StageStatePacket::envelopeValid(*stage)) && references();
}

bool CombatStatePacket::State::references() const {
    const auto owns = [&](s32 player) {
        return std::ranges::any_of(combat.players,
                                   [=](const auto& member) { return member.player == player; });
    };
    for (usize index = 0; index < adapter.hitStreak.size(); ++index) {
        if (adapter.hitStreak[index] != 0 && !owns(static_cast<s32>(index))) {
            return false;
        }
    }
    for (const auto& lesson : adapter.lessons) {
        if (!owns(lesson.second)) {
            return false;
        }
    }
    for (const auto& reward : adapter.generatorRewards) {
        if (!owns(reward.player)) {
            return false;
        }
    }
    u8 playerMask = 0;
    for (const auto& player : combat.players) {
        if (player.player < 0 || player.player >= 4) {
            return false;
        }
        playerMask |= static_cast<u8>(1U << player.player);
    }
    return adapter.references(playerMask, combat.effects);
}

std::optional<std::vector<u8>> CombatStatePacket::encode(const State& state) {
    try {
        Writer writer;
        writer.word(kMagic);
        writer.word(kVersion);
        writer.word(0); // Exact whole packet length, filled after the bounded field walk.
        writer.word(0); // Reserved, canonical zero.
        field(writer, state);
        if (!state.valid()) {
            return std::nullopt;
        }
        const auto length = static_cast<u32>(writer.bytes.size());
        for (usize index = 0; index < 4; ++index) {
            writer.bytes[8 + index] = static_cast<u8>(length >> (index * 8));
        }
        return std::move(writer.bytes);
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::optional<std::array<u8, 32>>
CombatStatePacket::canonicalDigest(const CombatRollbackCheckpoint::State& combat,
                                   const OpponentCombatCheckpoint::State& adapter,
                                   bool presentationIndependent) {
    try {
        CanonicalDigestSink sink;
        Writer writer;
        writer.used = kHeaderBytes; // Same bounded body budget as ordinary encoding.
        writer.digest = &sink;
        writer.agreement = presentationIndependent;
        fields(writer, combat, adapter);
        writer.number(false); // Stage has its own uncompressed, catalog-bound digest.
        return sink.finish();
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::optional<CombatStatePacket::State> CombatStatePacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes) {
        return std::nullopt;
    }
    try {
        Reader reader(bytes);
        u32 magic = 0;
        u32 version = 0;
        u32 length = 0;
        u32 reserved = 0;
        fields(reader, magic, version, length, reserved);
        if (magic != kMagic || version != kVersion || length != bytes.size() || reserved != 0) {
            return std::nullopt;
        }
        State state;
        field(reader, state);
        if (reader.remaining() != 0 || !state.valid()) {
            return std::nullopt;
        }
        return state;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

} // namespace gdl::game
