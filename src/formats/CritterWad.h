#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gdl::formats {

/** When a move or a pattern may be chosen against a target. */
struct CritterTargetRecord {
    float minDistance = 0.0f;
    float maxDistance = 0.0f; ///< none when nought or under
    float yaw = 0.0f;         ///< the facing cone's offset
    float minDot = -1.0f;     ///< how squarely the target must be faced
    float minRateScale = 0.0f;
    float maxRateScale = 0.0f;
    float idleGate = 0.0f;
    float maxVertical = 0.0f; ///< none when nought
};

/** One thing a critter can do: the sequence, the frames its harm is active over, what it
 * chains to, and when it may be chosen. */
struct CritterMoveRecord {
    std::int32_t type = 0;
    std::uint32_t flags = 0;
    std::int32_t priority = 0;
    std::string name;
    std::string anim;
    std::string colnode;
    std::int32_t frameStart = 0;
    std::int32_t frameStart2 = 0;
    std::int16_t damage0 = -1; ///< the damage record active over the first window
    std::int16_t damage1 = -1; ///< and the second
    float framePeriod = 0.0f;
    std::int16_t frameEnd = 0;
    std::int16_t frameEnd2 = 0;
    std::int16_t link = -1;     ///< the move that follows
    std::int16_t interrupt = 0; ///< how loudly it refuses to be cut short
    std::int16_t sfx = -1;
    std::int16_t sfxFrame = 0;
    std::int16_t sfx2 = -1;
    std::int16_t sfx2Frame = 0;
    CritterTargetRecord target;
    float cooldown = 0.0f;
    float speed = 0.0f;    ///< units a second the body goes while it plays
    float turnRate = 0.0f; ///< radians a second
    float hold = 0.0f;
};

/** How a move harms: where about the node, how far, how much. */
struct CritterDamageRecord {
    std::int16_t type = 0; ///< 0 a blow, 4 a breath, 7 a grab, the rest effects
    std::int16_t behaviorFlags = 0;
    std::uint32_t flags = 0;
    float radius = 0.0f;
    float maxDistance = 0.0f;
    float minDistance = 0.0f;
    float yaw = 0.0f;
    float minDot = 0.0f;
    float pitch = 0.0f;
    std::array<float, 3> offset{};
    float damage = 0.0f;
    float minSpeed = 0.0f; ///< what it throws (type 9's coins) leaves at least this fast
    float maxSpeed = 0.0f;
    float gravity = 0.0f;
    std::int16_t sfxIndex = -1;
    std::int16_t sfx = -1;
};

/** A part of the body that can be struck, and what striking it does. */
struct CritterNodeRecord {
    std::string nodeName;
    std::int16_t flags = 0;
    std::int16_t sfxIndex = -1;
    float maxTargetDistance = 0.0f;
    float targetScoreScale = 0.0f;
    std::array<float, 3> position{};
    float radius = 0.0f;
    std::string attach;
    float damageScale = 1.0f;
    float healthScale = 1.0f;
};

/** An effect a move, a strike or a hit starts: the tree of the creature's archive it plays,
 * the sound with it (its `%c` the level's letter), and how it is placed. */
struct CritterSoundRecord {
    std::uint32_t flags = 0;
    std::int32_t link = -1; ///< another record started with it
    std::string name;
    std::string levelFormat;
    std::array<float, 3> offset{}; ///< from the body (or the part struck with)
    float life = 0.0f;             ///< seconds it lasts, when it does not play out
    float rate = 0.0f;
    std::int16_t custom0 = 0;
    std::int16_t custom1 = 0;
    std::uint32_t tint = 0xFFFFFFFFU; ///< none when all ones
    float scale = 1.0f;
};

/** The critter itself: its size, pace, strength and worth, and which records are its. */
struct CritterTypeRecord {
    std::string suffix; ///< appended to the prefix for its tree ("1": GOLEM1)
    std::string rootNode;
    std::int16_t descriptorIndex = 0;
    std::int16_t subtype = 0;
    std::uint32_t typeFlags = 0;
    float radius = 0.0f;
    float wallRadius = 0.0f;
    CritterTargetRecord target;
    std::array<float, 3> defaultPos{};
    float speed = 0.0f; ///< a cap on how far it is carried, not its pace
    float floorOffset = 0.0f;
    float vertDrift = 0.0f;
    float damageScale = 0.0f;
    float armor = 0.0f;
    std::array<float, 3> originOffset{};
    float turnLimit = 0.0f;
    std::uint32_t shieldFlags = 0;
    float maxHealth = 0.0f;
    float expValue = 0.0f;
    float wakeThreshold = 0.0f;
    std::array<float, 3> healthBarOffset{}; ///< where its in-world bar hangs
    std::int16_t hitSoundClose = -1;  ///< the sound record started where a blow strikes it (0xF4)
    std::int16_t hitSoundFar = -1;    ///< and where a missile does (0xF6)
    std::int16_t meterPieces = 0;     ///< the HUD meter's: how many strips of 256
    std::int16_t meterAdvance = 0;    ///< how far the next boss's meter is put along
    std::int16_t meterLeftInset = 0;  ///< the first strip's cap, where the fill starts
    std::int16_t meterRightInset = 0; ///< the last strip's, where it ends
    std::int16_t moveCount = 0;
    std::int16_t moveIndex = 0;
    std::int16_t patternCount = 0;
    std::int16_t patternIndex = 0;
    std::int16_t colCount = 0;
    std::int16_t colBase = 0;
    std::int16_t childIndex = -1;
    std::int16_t parentIndex = -1;
};

struct CritterDescriptorRecord {
    std::string name;      ///< "golem"
    std::string prefix;    ///< "GOLEM"
    std::int16_t type = 0; ///< 3 a golem, 7 a gargoyle, 8 a general, 4 a boss
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
CritterFile parseCritterWad(std::span<const std::uint8_t> bytes);

} // namespace gdl::formats
