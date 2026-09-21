#pragma once

#include <array>
#include <span>

#include "engine/core/Types.h"

namespace gdl::formats {

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
    std::array<f32, 3> weaponOffset{}; ///< where a thrown weapon leaves the body, in its space
};

/**
 * Parses a PDATA wad: a little-endian container of tagged sections whose PDAT section holds
 * one class record. Throws FormatError on a malformed file.
 */
PlayerClassRecord parsePlayerDataWad(std::span<const u8> bytes);

} // namespace gdl::formats
