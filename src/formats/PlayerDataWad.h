#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

/** An effect a class's moves show: a tree of the costume colour's effects and a sound. */
struct MoveEffectRecord {
    std::uint32_t flags = 0;
    std::int32_t next = -1; ///< another effect started with it
    std::string tree;
    std::string sound;
    std::array<float, 3> offset{};
    float scale = 1.0f;
};

/** One thing a move does at one of its frames: a burst about a point or something sent
 * flying, with the harm it does. */
struct MoveStrikeRecord {
    std::int16_t type = 0; ///< 2 flies, 4 bursts where it starts
    std::int16_t flags = 0;
    std::uint32_t damageType = 0;
    float hitRadius = 0.0f; ///< of what flies
    float radius = 0.0f;    ///< of a burst
    float delay = 0.0f;     ///< seconds from its start to its harm
    float minTime = 0.0f;
    float maxTime = 0.0f; ///< how long what flies lasts
    float angle = 0.0f;   ///< radians off the facing; a volley's sweep
    float arc = -1.0f;    ///< the least cosine from the facing that is hit; -1 is all round
    std::array<float, 3> offset{}; ///< from the body, in its space
    float amount = 0.0f;           ///< harm; negative, that many times the character's own
    float speedMin = 0.0f;
    float speedMax = 0.0f;
    float weight = 0.0f; ///< scales how a volley's missiles fall
    std::int16_t effect = -1;
    std::int16_t hitEffect = -1;
    std::int16_t loopEffect = -1;
    std::int16_t next = -1;      ///< the strike that goes with it
    std::int16_t startFrame = 0; ///< of the move's sequence
    std::int16_t endFrame = -1;
    std::int16_t help = -1; ///< a help message it brings up
};

/** The tuning record a class ships in its PDATA wad: stat ranges and body size. */
struct PlayerClassRecord {
    std::uint16_t effectCount = 0;
    std::uint16_t damageCount = 0;
    float fightMin = 0.0f;
    float fightMax = 0.0f;
    float speedMin = 0.0f;
    float speedMax = 0.0f;
    float armorMin = 0.0f;
    float armorMax = 0.0f;
    float magicMin = 0.0f;
    float magicMax = 0.0f;
    float height = 0.0f;
    float width = 0.0f;
    float attachY = 0.0f;    ///< where things attach to the body
    float collisionY = 0.0f; ///< the collision anchor
    float powerupTime = 0.0f;
    std::array<float, 3> weaponOffset{}; ///< where a thrown weapon leaves the body, in its space
    /** The first strike of each move, in the record's order; -1 for a move the class lacks. */
    static constexpr std::size_t kMoveCount = 12;
    static constexpr std::array<std::string_view, kMoveCount> kMoveNames{
        "turboAClose", "turboALow", "turboAStep", "turboA360", "turboAThrow", "turboB",
        "turboC1",     "turboC2",   "combo1",     "combo2",    "comboHit",    "victory",
    };
    std::array<std::int16_t, kMoveCount> moves{};
    std::vector<MoveEffectRecord> effects; ///< empty when the wad has no such section
    std::vector<MoveStrikeRecord> strikes;
};

/**
 * Parses a PDATA wad: a little-endian container of tagged sections whose PDAT section holds
 * one class record. Throws FormatError on a malformed file.
 */
PlayerClassRecord parsePlayerDataWad(std::span<const std::uint8_t> bytes);

} // namespace gdl::formats
