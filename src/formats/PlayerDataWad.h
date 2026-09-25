#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/** An effect a class's moves show: a tree of the costume colour's effects and a sound. */
struct MoveEffectRecord {
    u32 flags = 0;
    s32 next = -1; ///< another effect started with it
    std::string tree;
    std::string sound;
    std::array<f32, 3> offset{};
    f32 scale = 1.0f;
};

/** One thing a move does at one of its frames: a burst about a point or something sent
 * flying, with the harm it does. */
struct MoveStrikeRecord {
    s16 type = 0; ///< 2 flies, 4 bursts where it starts
    s16 flags = 0;
    u32 damageType = 0;
    f32 hitRadius = 0.0f; ///< of what flies
    f32 radius = 0.0f;    ///< of a burst
    f32 delay = 0.0f;     ///< seconds from its start to its harm
    f32 minTime = 0.0f;
    f32 maxTime = 0.0f;          ///< how long what flies lasts
    f32 angle = 0.0f;            ///< radians off the facing; a volley's sweep
    f32 arc = -1.0f;             ///< the least cosine from the facing that is hit; -1 is all round
    std::array<f32, 3> offset{}; ///< from the body, in its space
    f32 amount = 0.0f;           ///< harm; negative, that many times the character's own
    f32 speedMin = 0.0f;
    f32 speedMax = 0.0f;
    f32 weight = 0.0f; ///< scales how a volley's missiles fall
    s16 effect = -1;
    s16 hitEffect = -1;
    s16 loopEffect = -1;
    s16 next = -1;      ///< the strike that goes with it
    s16 startFrame = 0; ///< of the move's sequence
    s16 endFrame = -1;
    s16 help = -1; ///< a help message it brings up
};

/** The tuning record a class ships in its PDATA wad: stat ranges and body size. */
struct PlayerClassRecord {
    u16 effectCount = 0;
    u16 damageCount = 0;
    f32 fightMin = 0.0f;
    f32 fightMax = 0.0f;
    f32 speedMin = 0.0f;
    f32 speedMax = 0.0f;
    f32 armorMin = 0.0f;
    f32 armorMax = 0.0f;
    f32 magicMin = 0.0f;
    f32 magicMax = 0.0f;
    f32 height = 0.0f;
    f32 width = 0.0f;
    f32 attachY = 0.0f;    ///< where things attach to the body
    f32 collisionY = 0.0f; ///< the collision anchor
    f32 powerupTime = 0.0f;
    std::array<f32, 3> weaponOffset{};   ///< where a thrown weapon leaves the body, in its space
    std::array<f32, 3> familiarOffset{}; ///< class-specific permanent familiar attachment
    std::array<f32, 3> familiarShotOffset{}; ///< projectile origin in player-local space
    /** The first strike of each move, in the record's order; -1 for a move the class lacks. */
    static constexpr usize kMoveCount = 12;
    static constexpr std::array<std::string_view, kMoveCount> kMoveNames{
        "turboAClose", "turboALow", "turboAStep", "turboA360", "turboAThrow", "turboB",
        "turboC1",     "turboC2",   "combo1",     "combo2",    "comboHit",    "victory",
    };
    std::array<s16, kMoveCount> moves{};
    std::vector<MoveEffectRecord> effects; ///< empty when the wad has no such section
    std::vector<MoveStrikeRecord> strikes;
};

/**
 * Parses a PDATA wad: a little-endian container of tagged sections whose PDAT section holds
 * one class record. Throws FormatError on a malformed file.
 */
PlayerClassRecord parsePlayerDataWad(std::span<const u8> bytes);

} // namespace gdl::formats
