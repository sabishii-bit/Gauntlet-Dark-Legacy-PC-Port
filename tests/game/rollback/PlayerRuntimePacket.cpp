#include "game/rollback/PlayerRuntimePacket.h"

#include <array>
#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

#include "engine/io/ByteReader.h"

#include "game/rollback/AnimationCheckpoint.h"
#include "game/rollback/PlayerActionCheckpoint.h"
#include "game/rollback/PlayerKnockbackCheckpoint.h"
#include "game/rollback/PlayerPoseCheckpoint.h"
#include "game/rollback/PlayerTurboCheckpoint.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("PRTS");

/** One explicit field walk serves both directions. None of the native structs'
 * object representation, padding, platform-sized indices or pointers is copied. */
class Writer {
public:
    static constexpr bool reading = false; // NOLINT(readability-identifier-naming): archive trait
    std::vector<u8> bytes;

    void raw(std::span<const u8> source) {
        if (source.size() > PlayerRuntimePacket::kMaxBytes - bytes.size()) {
            throw FormatError("participant checkpoint exceeds its byte limit");
        }
        bytes.insert(bytes.end(), source.begin(), source.end());
    }
    template <class T> void number(T value) {
        if constexpr (std::is_same_v<T, bool>) {
            const u8 byte = value ? 1 : 0;
            raw({&byte, 1});
        } else if constexpr (std::is_enum_v<T>) {
            number(static_cast<u32>(value));
        } else if constexpr (std::is_same_v<T, f32>) {
            number(std::bit_cast<u32>(value));
        } else if constexpr (std::is_same_v<T, f64>) {
            number(std::bit_cast<u64>(value));
        } else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            const U bits = static_cast<U>(value);
            std::array<u8, sizeof(T)> result{};
            for (usize index = 0; index < result.size(); ++index) {
                result[index] = static_cast<u8>(bits >> (index * 8));
            }
            raw(result);
        }
    }
    void index(usize value) {
        if (value > std::numeric_limits<u32>::max()) {
            throw FormatError("participant checkpoint index exceeds its wire range");
        }
        number(static_cast<u32>(value));
    }
    usize remaining() const { return PlayerRuntimePacket::kMaxBytes - bytes.size(); }
};

class Reader {
public:
    static constexpr bool reading = true; // NOLINT(readability-identifier-naming): archive trait
    explicit Reader(std::span<const u8> bytes) : m_reader(bytes) {}
    std::span<const u8> raw(usize count) { return m_reader.readBytes(count); }
    template <class T> void number(T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            const u8 byte = m_reader.readU8();
            if (byte > 1) {
                throw FormatError("noncanonical participant checkpoint boolean");
            }
            value = byte != 0;
        } else if constexpr (std::is_enum_v<T>) {
            using U = std::underlying_type_t<T>;
            const auto bits = m_reader.readU32();
            if (bits > static_cast<u64>(std::numeric_limits<U>::max())) {
                throw FormatError("participant checkpoint enum exceeds its native range");
            }
            value = static_cast<T>(bits);
        } else if constexpr (std::is_same_v<T, f32>) {
            value = std::bit_cast<f32>(m_reader.readU32());
        } else if constexpr (std::is_same_v<T, f64>) {
            u64 bits = 0;
            number(bits);
            value = std::bit_cast<f64>(bits);
        } else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            U bits = 0;
            const auto source = raw(sizeof(T));
            for (usize index = 0; index < source.size(); ++index) {
                bits = static_cast<U>(bits | (static_cast<U>(source[index]) << (index * 8)));
            }
            value = std::bit_cast<T>(bits);
        }
    }
    void index(usize& value) { value = m_reader.readU32(); }
    usize remaining() const { return m_reader.remaining(); }

private:
    ByteReader m_reader;
};

template <class A, class... T> void numbers(A& archive, T&... value) {
    (archive.number(value), ...);
}
template <class A, class T> void vector(A& archive, T& value) {
    numbers(archive, value.x, value.y, value.z);
}
template <class A, class T> void matrix(A& archive, T& value) {
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            archive.number(value[column][row]);
        }
    }
}
template <class A, class T, class F> void optional(A& archive, T& value, const F& field) {
    bool present = value.has_value(); // NOLINT(misc-const-correctness): the archive reads it back
    archive.number(present);
    if constexpr (A::reading) {
        if (present) {
            value.emplace(typename T::value_type{});
        } else {
            value.reset();
        }
    }
    if (present) {
        field(archive, *value);
    }
}
template <class A, class T, class F>
void sequence(A& archive, T& values, usize limit, usize minimumBytes, const F& field) {
    usize count = values.size(); // NOLINT(misc-const-correctness): the archive reads it back
    archive.index(count);
    if (count > limit || count > archive.remaining() / minimumBytes) {
        throw FormatError("participant checkpoint sequence exceeds its bounds");
    }
    if constexpr (A::reading) {
        values.resize(count);
    }
    for (auto& value : values) {
        field(archive, value);
    }
}
template <class A, class T> void text(A& archive, T& value, usize limit) {
    usize count = value.size(); // NOLINT(misc-const-correctness): the archive reads it back
    archive.index(count);
    if (count > limit || count > archive.remaining()) {
        throw FormatError("participant checkpoint string exceeds its bounds");
    }
    if constexpr (A::reading) {
        const auto bytes = archive.raw(count);
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    } else {
        // byte view of a trivially copyable buffer
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        archive.raw({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
}
template <class Codec, class A, class T> void block(A& archive, T& value, usize limit) {
    if constexpr (A::reading) {
        usize count = 0;
        archive.index(count);
        if (count > limit) {
            throw FormatError("participant checkpoint block exceeds its bounds");
        }
        auto decoded = Codec::decode(archive.raw(count));
        if (!decoded) {
            throw FormatError("invalid participant checkpoint nested state");
        }
        value = std::move(*decoded);
    } else {
        const auto encoded = Codec::encode(value);
        if (!encoded || encoded->size() > limit) {
            throw FormatError("invalid participant checkpoint nested state");
        }
        archive.index(encoded->size());
        archive.raw(*encoded);
    }
}

template <class A, class T> void progress(A& archive, T& value) {
    numbers(archive, value.experience, value.gold, value.promotedLevel, value.health,
            value.fightAdd, value.armorAdd, value.magicAdd, value.speedAdd);
    for (auto& count : value.crystals) {
        archive.number(count);
    }
    archive.number(value.unlocked);
    auto& inventory = value.inventory;
    archive.number(inventory.keys);
    sequence(archive, inventory.potions, Inventory::kMostPotions, 4,
             [](auto& a, auto& item) { a.number(item); });
    for (auto& slot : inventory.powerups) {
        numbers(archive, slot.strength, slot.kind, slot.charge, slot.flags, slot.on);
    }
    archive.number(inventory.permanentPojo);
    auto& relics = value.relics;
    numbers(archive, relics.runes, relics.legends, relics.shards, relics.pendingRunes,
            relics.pendingShards, relics.pendingCeremonies);
    for (auto& piece : relics.gargoylePieces) {
        archive.number(piece);
    }
    for (auto& realm : value.levels.beaten) {
        archive.number(realm);
    }
    for (auto& mask : value.levels.runeLevels) {
        archive.number(mask);
    }
    for (auto& mask : value.levels.legendLevels) {
        archive.number(mask);
    }
    for (auto& mask : value.levels.bossDeaths) {
        archive.number(mask);
    }
    numbers(archive, value.lifetime.enemiesKilled, value.lifetime.generatorsDestroyed,
            value.lifetime.goldFound, value.lifetime.playSeconds, value.lifetime.bestStreak);
    for (auto& campaign : value.legends) {
        numbers(archive, campaign.runestones, campaign.shards, campaign.obelisks,
                campaign.legendItems);
        sequence(archive, campaign.completedMaps, LegendsProgress::kMaxCompletedMaps, 4,
                 [](auto& a, auto& name) { text(a, name, LegendsProgress::kMaxMapName); });
    }
}
template <class A, class T> void save(A& archive, T& value) {
    text(archive, value.name, kCharacterNameLength);
    numbers(archive, value.character, value.color, value.classUnlock, value.gold, value.levelTotal,
            value.autoAttack);
    sequence(archive, value.helpSeen, PlayerRuntimePacket::kMaxRecords, 4,
             [](auto& a, auto& id) { a.number(id); });
    sequence(archive, value.moviesSeen, PlayerRuntimePacket::kMaxMovies, 4,
             [](auto& a, auto& name) { text(a, name, PlayerRuntimePacket::kMaxString); });
    for (auto& entry : value.classes) {
        progress(archive, entry);
    }
}
template <class A, class T> void actor(A& archive, T& value) {
    vector(archive, value.position);
    numbers(archive, value.yaw, value.speed, value.paceBonus, value.radius, value.height,
            value.followHeight, value.attentionHeight, value.moving);
    sequence(archive, value.contacts, PlayerActor::Checkpoint::kMaxContacts, 16,
             [](auto& a, auto& contact) {
                 a.number(contact.object);
                 vector(a, contact.point);
             });
}
template <class A, class T> void companionPose(A& archive, T& value) {
    text(archive, value.tree, TreePose::Checkpoint::kMaxName);
    sequence(archive, value.nodes, TreePose::Checkpoint::kMaxNodes, 20, [](auto& a, auto& node) {
        text(a, node.name, TreePose::Checkpoint::kMaxName);
        a.number(node.parent);
        vector(a, node.rest);
    });
    sequence(archive, value.poses, TreePose::Checkpoint::kMaxNodes, 37, [](auto& a, auto& node) {
        vector(a, node.rotation);
        vector(a, node.position);
        vector(a, node.scale);
        // number() reads and writes through one template: a const value would turn its
        // reader into one that cannot assign.
        // NOLINTNEXTLINE(misc-const-correctness)
        u8 order = static_cast<u8>(node.rotationOrder);
        a.number(order);
        if (order > static_cast<u8>(NodePose::RotationOrder::YawPitchRoll)) {
            throw FormatError("invalid participant rotation order");
        }
        if constexpr (std::remove_reference_t<decltype(a)>::reading) {
            node.rotationOrder = static_cast<NodePose::RotationOrder>(order);
        }
    });
}
template <class A, class T> void companionClock(A& archive, T& value) {
    text(archive, value.tree, TreePose::Checkpoint::kMaxName);
    block<AnimationCheckpoint>(archive, value.animation, AnimationCheckpoint::kBytes);
    numbers(archive, value.textures.frame, value.textures.remainder, value.textures.advance);
    optional(archive, value.pose, [](auto& a, auto& pose) { companionPose(a, pose); });
    numbers(archive, value.previousFrame, value.previousGeneration, value.presentationAdvanced);
}
template <class A, class T> void figure(A& archive, T& value) {
    text(archive, value.actions, 256);
    text(archive, value.costume, 256);
    const auto object = [](auto& a, auto& name) { text(a, name, 256); };
    sequence(archive, value.costumeObjects, PlayerFigure::GameplayCheckpoint::kMaxNodes, 4, object);
    sequence(archive, value.handObjects, PlayerFigure::GameplayCheckpoint::kMaxNodes, 4, object);
    sequence(archive, value.armObjects, PlayerFigure::GameplayCheckpoint::kMaxNodes, 4, object);
    numbers(archive, value.familiarTier, value.companion, value.handHeld, value.armHeld,
            value.staysInHand);
    block<PlayerActionCheckpoint>(archive, value.animation, PlayerActionCheckpoint::kBytes);
    optional(archive, value.transition, [](auto& a, auto& pose) {
        block<PlayerPoseCheckpoint>(a, pose, PlayerPoseCheckpoint::kMaxBytes);
    });
    sequence(archive, value.transforms, PlayerFigure::GameplayCheckpoint::kMaxNodes, 64,
             [](auto& a, auto& transform) { matrix(a, transform); });
    numbers(archive, value.animationRevision, value.familiarPending, value.familiarReleased,
            value.phoenixSide);
    archive.number(value.familiar.tier);
    vector(archive, value.familiar.offset);
    companionClock(archive, value.familiar);
    optional(archive, value.familiar.previousPose,
             [](auto& a, auto& pose) { companionPose(a, pose); });
    archive.number(value.powerupCompanion.kind);
    companionClock(archive, value.powerupCompanion);
    archive.number(value.companionAlpha);
}

template <class A, class T> void runtime(A& archive, T& state) {
    archive.number(state.player);
    save(archive, state.save);
    actor(archive, state.actor);
    archive.number(state.cursorAiming);
    optional(archive, state.meleeFacing, [](auto& a, auto& value) { vector(a, value); });
    optional(archive, state.attackStep, [](auto& a, auto& step) {
        vector(a, step.from);
        vector(a, step.to);
    });
    vector(archive, state.previous.position);
    numbers(archive, state.previous.yaw, state.previous.continuous,
            state.previous.animationRevision);
    auto& capture = state.capture;
    optional(archive, capture.body, [](auto& a, auto& value) { matrix(a, value); });
    vector(archive, capture.velocity);
    numbers(archive, capture.supportHeight, capture.damage, capture.flying, capture.owner,
            capture.boss);
    numbers(archive, capture.supportXZ.x, capture.supportXZ.y);
    archive.number(capture.restoreSupport);
    auto& combo = state.combo;
    numbers(archive, combo.role, combo.partner, combo.grabberClass, combo.riding, combo.rideAsked,
            combo.ticksLeft, combo.turnTicks, combo.graceSeconds);
    vector(archive, combo.saved);
    vector(archive, state.transport.destination);
    numbers(archive, state.transport.timer, state.transport.needsStepOff);
    vector(archive, state.mikey.position);
    numbers(archive, state.mikey.frames, state.mikey.state, state.mikey.sparkles,
            state.mikey.generation);
    optional(archive, state.figure, [](auto& a, auto& value) { figure(a, value); });
    optional(archive, state.slot, [](auto& a, auto& value) { a.index(value); });
    save(archive, state.entrySave);
    optional(archive, state.resultsCheckpoint,
             [](auto& a, auto& value) { numbers(a, value.gold, value.experience, value.kills); });
    numbers(archive, state.levelKills, state.life, state.towerPrompt, state.departed,
            state.healthFraction, state.painOwed, state.hitSoundGap, state.heartbeatTicks);
    optional(archive, state.fixtureSpot, [](auto& a, auto& value) { vector(a, value); });
    numbers(archive, state.surfaceGap, state.hitFlashTicks, state.itTicks, state.gesture,
            state.gagSeconds);
    sequence(archive, state.shockGaps, PlayerRuntimePacket::kMaxRecords, 8,
             [](auto& a, auto& gap) { numbers(a, gap.target, gap.seconds); });
    numbers(archive, state.deathHeld, state.deathDrainTicks, state.deathHaloHeard, state.reaction);
    block<PlayerKnockbackCheckpoint>(archive, state.knockback, PlayerKnockbackCheckpoint::kBytes);
    PlayerTurboState turbo{state.turbo, state.move};
    block<PlayerTurboCheckpoint>(archive, turbo, PlayerTurboCheckpoint::kMaxBytes);
    if constexpr (A::reading) {
        state.turbo = turbo.meter;
        state.move = std::move(turbo.move);
    }
    archive.number(state.pojoTurbo);
    sequence(archive, state.helpHeard, PlayerRuntimePacket::kMaxRecords, 4,
             [](auto& a, auto& id) { a.number(id); });
    sequence(archive, state.rammed, PlayerRuntimePacket::kMaxRecords, 4,
             [](auto& a, auto& id) { a.index(id); });
    numbers(archive, state.blockLeft, state.blocked, state.cloudGap, state.breathGap,
            state.effectGap, state.floor.object, state.floor.flags);
    optional(archive, state.floor.placement, [](auto& a, auto& value) { matrix(a, value); });
    numbers(archive, state.gem.handOfDeath, state.gem.healthVamp, state.gem.shown, state.glow,
            state.weaponGlow.element, state.weaponGlow.effect, state.streak.count,
            state.streak.quiet, state.streak.running, state.exitWait.ticks, state.nameTicks,
            state.wornSpecial);
}
} // namespace

std::optional<std::vector<u8>>
PlayerRuntimePacket::encode(const PlayerRuntimeCheckpoint::State& state) {
    if (!state.valid()) {
        return std::nullopt;
    }
    try {
        Writer writer;
        writer.number(kMagic);
        writer.number(kVersion);
        writer.number(u32{0}); // Exact complete packet length, filled after bounded encoding.
        writer.number(u32{0}); // Reserved, canonical zero.
        runtime(writer, state);
        const auto size = static_cast<u32>(writer.bytes.size());
        for (usize index = 0; index < 4; ++index) {
            writer.bytes[8 + index] = static_cast<u8>(size >> (index * 8));
        }
        return std::move(writer.bytes);
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::optional<PlayerRuntimeCheckpoint::State>
PlayerRuntimePacket::decode(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxBytes || readU32LE(bytes, 0) != kMagic ||
        readU32LE(bytes, 4) != kVersion || readU32LE(bytes, 8) != bytes.size() ||
        readU32LE(bytes, 12) != 0) {
        return std::nullopt;
    }
    try {
        Reader reader(bytes.subspan(kHeaderBytes));
        PlayerRuntimeCheckpoint::State state;
        runtime(reader, state);
        if (reader.remaining() != 0 || !state.valid()) {
            return std::nullopt;
        }
        return state;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}
} // namespace gdl::game
