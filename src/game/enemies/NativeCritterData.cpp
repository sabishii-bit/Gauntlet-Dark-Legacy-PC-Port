#include <array>
#include <cctype>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "formats/CritterWad.h"
#include "game/enemies/CritterData.h"

namespace gdl::game {
namespace {

Vec3 vectorOf(const std::array<f32, 3>& value) {
    return {value[0], value[1], value[2]};
}

TargetCriteria targetOf(const formats::CritterTargetRecord& source) {
    TargetCriteria target;
    target.minDistance = source.minDistance;
    target.maxDistance = source.maxDistance;
    target.yaw = source.yaw;
    target.minDot = source.minDot;
    target.maxVertical = source.maxVertical;
    target.minRateScale = source.minRateScale;
    target.maxRateScale = source.maxRateScale;
    target.maxHomeDistance = source.idleGate;
    return target;
}

template <typename T>
std::span<const T> recordsOf(const std::vector<T>& records, s16 start, s16 count) {
    if (count < 0 ||
        (count > 0 && (start < 0 || static_cast<usize>(start) > records.size() ||
                       static_cast<usize>(count) > records.size() - static_cast<usize>(start)))) {
        throw FormatError("critter wad: type record range out of bounds");
    }
    return count == 0 ? std::span<const T>{}
                      : std::span<const T>(records).subspan(static_cast<usize>(start),
                                                            static_cast<usize>(count));
}

MoveDefinition moveOf(const formats::CritterMoveRecord& source) {
    MoveDefinition move;
    move.type = source.type;
    move.flags = source.flags;
    move.priority = source.priority;
    move.name = source.name;
    move.anim = source.anim;
    move.colnode = source.colnode;
    if (!move.colnode.empty() &&
        std::isspace(static_cast<unsigned char>(move.colnode.front())) != 0) {
        move.colnode.clear();
    }
    move.frameStart = source.frameStart;
    move.frameEnd = source.frameEnd;
    move.frameStart2 = source.frameStart2;
    move.frameEnd2 = source.frameEnd2;
    move.framePeriod = source.framePeriod;
    move.damage0 = source.damage0;
    move.damage1 = source.damage1;
    move.link = source.link;
    move.interrupt = source.interrupt;
    move.sound = source.sfx;
    move.soundFrame = source.sfxFrame;
    move.sound2 = source.sfx2;
    move.sound2Frame = source.sfx2Frame;
    move.target = targetOf(source.target);
    move.cooldown = source.cooldown;
    move.speed = source.speed;
    move.turnRate = source.turnRate;
    move.hold = source.hold;
    return move;
}

AttackDefinition damageOf(const formats::CritterDamageRecord& source) {
    AttackDefinition damage;
    damage.type = source.type;
    damage.behaviorFlags = static_cast<u16>(source.behaviorFlags);
    damage.flags = source.flags;
    damage.radius = source.radius;
    damage.maxDistance = source.maxDistance;
    damage.minDistance = source.minDistance;
    damage.yaw = source.yaw;
    damage.minDot = source.minDot;
    damage.pitch = source.pitch;
    damage.offset = vectorOf(source.offset);
    damage.damage = source.damage;
    damage.speed = source.minSpeed;
    damage.maxSpeed = source.maxSpeed;
    damage.gravity = source.gravity;
    damage.morphLife = source.morphLife;
    damage.yawSpread = source.yawSpread;
    damage.sound = source.sfxIndex;
    damage.hitSound = source.sfx;
    damage.morph = source.morph;
    damage.morphEnd = source.morphEnd;
    return damage;
}

CombatEffectDefinition soundOf(const formats::CritterSoundRecord& source) {
    CombatEffectDefinition sound;
    sound.tree = source.name;
    sound.soundFormat = source.levelFormat;
    sound.flags = source.flags;
    sound.link = source.link;
    sound.offset = vectorOf(source.offset);
    sound.life = source.life;
    sound.particleRate = source.rate;
    sound.skinLoops = source.custom0;
    sound.particleSpeed = 0.01f * static_cast<f32>(source.custom1);
    sound.scale = source.scale <= 0.0f ? 1.0f : source.scale;
    return sound;
}

} // namespace

bool CritterData::loadNative(const std::filesystem::path& file, usize typeIndex) {
    const auto source = formats::parseCritterWad(readFile(file));
    if (typeIndex >= source.types.size()) {
        throw FormatError("critter wad: missing requested type");
    }
    const auto& type = source.types[typeIndex];
    for (const auto& attachment : source.attachments) {
        if (attachment.typeIndex < 0 ||
            static_cast<usize>(attachment.typeIndex) >= source.types.size()) {
            throw FormatError("critter wad: invalid attachment owner");
        }
        if (static_cast<usize>(attachment.typeIndex) == typeIndex) {
            m_attachments.push_back({attachment.tree, attachment.node, vectorOf(attachment.offset),
                                     (attachment.flags & 1U) != 0});
        }
    }
    if (type.descriptorIndex < 0 ||
        static_cast<usize>(type.descriptorIndex) >= source.descriptors.size()) {
        throw FormatError("critter wad: invalid descriptor index");
    }
    const auto& descriptor = source.descriptors[static_cast<usize>(type.descriptorIndex)];
    const auto moves = recordsOf(source.moves, type.moveIndex, type.moveCount);
    const auto patterns = recordsOf(source.patterns, type.patternIndex, type.patternCount);
    const auto nodes = recordsOf(source.nodes, type.colBase, type.colCount);
    if (moves.empty()) {
        throw FormatError("critter wad: type has no moves");
    }
    m_name = normalizeAssetName(file.stem().string());
    m_folder = toLowerAscii(descriptor.name);
    m_prefix = descriptor.prefix;
    m_kind = static_cast<CombatantKind>(descriptor.type);
    m_typeFlags = type.typeFlags;
    m_suffix = type.suffix;
    m_childIndex = type.childIndex;
    m_parentIndex = type.parentIndex;
    m_rootNode = type.rootNode;
    m_radius = type.radius;
    m_wallRadius = type.wallRadius;
    m_armor = type.armor;
    m_itemDamage = type.damageScale;
    m_shieldFlags = type.shieldFlags;
    m_maxHealth = type.maxHealth;
    m_experience = type.expValue;
    m_wake = type.wakeThreshold;
    m_vertDrift = type.vertDrift;
    m_floorOffset = type.floorOffset;
    m_originOffset = vectorOf(type.originOffset);
    m_sight = targetOf(type.target);
    m_movement.roamRadius = type.roamRadius;
    m_movement.turnLimit = type.turnLimit;
    constexpr u32 kShadowed = 1;
    constexpr u32 kSquareBounds = 0x20;
    constexpr u32 kInitialStepBasis = 0x40;
    constexpr u32 kUnrestrictedTurn = 0x400;
    m_shadowed = (type.typeFlags & kShadowed) != 0;
    m_movement.squareBounds = (type.typeFlags & kSquareBounds) != 0;
    m_movement.initialStepBasis = (type.typeFlags & kInitialStepBasis) != 0;
    m_movement.unrestrictedTurn = (type.typeFlags & kUnrestrictedTurn) != 0;
    constexpr f32 kNoHome = 999.0f;
    if (type.defaultPos[1] < kNoHome) {
        m_movement.home = vectorOf(type.defaultPos);
    }
    m_meter.pieces = type.meterPieces;
    m_meter.advance = type.meterAdvance;
    m_meter.leftInset = type.meterLeftInset;
    m_meter.rightInset = type.meterRightInset;
    m_meter.shown = (type.typeFlags & HealthMeterDefinition::kShown) != 0 && m_meter.pieces > 0;
    m_meter.backed = (type.typeFlags & HealthMeterDefinition::kBacked) != 0;
    m_meter.inWorld = (type.typeFlags & HealthMeterDefinition::kInWorld) != 0;
    m_meter.name = m_suffix;
    m_meter.barOffset = vectorOf(type.healthBarOffset);
    for (const auto& move : moves) {
        m_moves.push_back(moveOf(move));
    }
    for (const auto& record : patterns) {
        AttackPattern pattern;
        pattern.flags = record.flags;
        pattern.cooldown = record.cooldown;
        pattern.target = targetOf(record.target);
        for (const s16 move : record.moves) {
            if (move < 0) {
                break;
            }
            if (static_cast<usize>(move) >= m_moves.size()) {
                throw FormatError("critter wad: pattern move index out of bounds");
            }
            pattern.moves.push_back(move);
        }
        m_patterns.push_back(std::move(pattern));
    }
    for (const auto& damage : source.damages) {
        m_damages.push_back(damageOf(damage));
    }
    for (const auto& sound : source.sounds) {
        m_sounds.push_back(soundOf(sound));
    }
    m_hitSoundFar = type.hitSoundFar;
    m_hitSoundClose = type.hitSoundClose;
    constexpr u32 kLooksThroughParent = 0x10;
    m_looks[0] = {type.lookNode0, (type.typeFlags & kLooksThroughParent) != 0, type.lookYawRate0,
                  type.lookPitchRate0, type.lookPitchBias0};
    m_looks[1] = {type.lookNode1, false, type.lookYawRate1, type.lookPitchRate1,
                  type.lookPitchBias1};
    constexpr u32 kTargetableNodes = 2;
    for (const auto& node : nodes) {
        CritterPart part;
        part.node = node.nodeName;
        part.position = vectorOf(node.position);
        part.radius = node.radius;
        part.damageScale = node.damageScale;
        part.healthScale = node.healthScale;
        part.targetScoreScale =
            (type.typeFlags & kTargetableNodes) != 0 ? node.targetScoreScale : 0;
        part.maxTargetDistance = node.maxTargetDistance;
        part.damageEffect = node.sfxIndex;
        part.flags = static_cast<u32>(node.flags);
        m_parts.push_back(part);
    }
    return true;
}

} // namespace gdl::game
