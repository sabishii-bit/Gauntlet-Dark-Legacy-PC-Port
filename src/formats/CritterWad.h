#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/** When a move or a pattern may be chosen against a target. */
struct CritterTargetRecord {
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f; ///< none when nought or under
    f32 yaw = 0.0f;         ///< the facing cone's offset
    f32 minDot = -1.0f;     ///< how squarely the target must be faced
    f32 minRateScale = 0.0f;
    f32 maxRateScale = 0.0f;
    f32 idleGate = 0.0f;
    f32 maxVertical = 0.0f; ///< none when nought
};

/** One thing a critter can do: the sequence, the frames its harm is active over, what it
 * chains to, and when it may be chosen. */
struct CritterMoveRecord {
    s32 type = 0;
    u32 flags = 0;
    s32 priority = 0;
    std::string name;
    std::string anim;
    std::string colnode;
    s32 frameStart = 0;
    s32 frameStart2 = 0;
    s16 damage0 = -1; ///< the damage record active over the first window
    s16 damage1 = -1; ///< and the second
    f32 framePeriod = 0.0f;
    s16 frameEnd = 0;
    s16 frameEnd2 = 0;
    s16 link = -1;      ///< the move that follows
    s16 interrupt = 0;  ///< how loudly it refuses to be cut short
    s16 sfx = -1;
    s16 sfxFrame = 0;
    s16 sfx2 = -1;
    s16 sfx2Frame = 0;
    CritterTargetRecord target;
    f32 cooldown = 0.0f;
    f32 speed = 0.0f;    ///< units a second the body goes while it plays
    f32 turnRate = 0.0f; ///< radians a second
    f32 hold = 0.0f;
};

/** How a move harms: where about the node, how far, how much. */
struct CritterDamageRecord {
    s16 type = 0; ///< 0 a blow, 4 a breath, 7 a grab, the rest effects
    s16 behaviorFlags = 0;
    u32 flags = 0;
    f32 radius = 0.0f;
    f32 maxDistance = 0.0f;
    f32 minDistance = 0.0f;
    f32 yaw = 0.0f;
    f32 minDot = 0.0f;
    f32 pitch = 0.0f;
    std::array<f32, 3> offset{};
    f32 damage = 0.0f;
    s16 sfxIndex = -1;
    s16 sfx = -1;
};

/** A part of the body that can be struck, and what striking it does. */
struct CritterNodeRecord {
    std::string nodeName;
    s16 flags = 0;
    s16 sfxIndex = -1;
    f32 maxTargetDistance = 0.0f;
    f32 targetScoreScale = 0.0f;
    std::array<f32, 3> position{};
    f32 radius = 0.0f;
    std::string attach;
    f32 damageScale = 1.0f;
    f32 healthScale = 1.0f;
};

struct CritterSoundRecord {
    std::string name;
    std::string levelFormat;
};

/** The critter itself: its size, pace, strength and worth, and which records are its. */
struct CritterTypeRecord {
    std::string suffix;   ///< appended to the prefix for its tree ("1": GOLEM1)
    std::string rootNode;
    s16 descriptorIndex = 0;
    s16 subtype = 0;
    u32 typeFlags = 0;
    f32 radius = 0.0f;
    f32 wallRadius = 0.0f;
    CritterTargetRecord target;
    std::array<f32, 3> defaultPos{};
    f32 speed = 0.0f;       ///< a cap on how far it is carried, not its pace
    f32 floorOffset = 0.0f;
    f32 vertDrift = 0.0f;
    f32 damageScale = 0.0f;
    f32 armor = 0.0f;
    std::array<f32, 3> originOffset{};
    f32 turnLimit = 0.0f;
    u32 shieldFlags = 0;
    f32 maxHealth = 0.0f;
    f32 expValue = 0.0f;
    f32 wakeThreshold = 0.0f;
    std::array<f32, 3> healthBarOffset{}; ///< where its in-world bar hangs
    s16 meterPieces = 0;      ///< the HUD meter's: how many strips of 256
    s16 meterAdvance = 0;     ///< how far the next boss's meter is put along
    s16 meterLeftInset = 0;   ///< the first strip's cap, where the fill starts
    s16 meterRightInset = 0;  ///< the last strip's, where it ends
    s16 moveCount = 0;
    s16 moveIndex = 0;
    s16 patternCount = 0;
    s16 patternIndex = 0;
    s16 colCount = 0;
    s16 colBase = 0;
    s16 childIndex = -1;
    s16 parentIndex = -1;
};

struct CritterDescriptorRecord {
    std::string name;   ///< "golem"
    std::string prefix; ///< "GOLEM"
    s16 type = 0;       ///< 3 a golem, 7 a gargoyle, 8 a general, 4 a boss
};

/** A `CRITTER/<NAME>.WAD`: everything the original knows of one great creature. */
struct CritterFile {
    std::vector<CritterSoundRecord> sounds;
    std::vector<CritterDamageRecord> damages;
    std::vector<CritterDescriptorRecord> descriptors;
    std::vector<CritterNodeRecord> nodes;
    std::vector<CritterMoveRecord> moves;
    std::vector<CritterTypeRecord> types;
};

/** Parses a critter wad. Throws FormatError. */
CritterFile parseCritterWad(std::span<const u8> bytes);

} // namespace gdl::formats
