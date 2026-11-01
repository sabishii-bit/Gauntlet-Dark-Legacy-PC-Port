#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "formats/CritterWad.h"

namespace gdl::formats {
/** Native big-endian Legends boss moves.wad, translated to the common combat data.
 * Runtime pointers/sequence caches are never imported. Throws FormatError. */
CritterFile decodeLegendsN64Critter(std::span<const u8> bytes);

/** BNAM resolves combat references by authored node index, independently of model names. */
struct LegendsN64BoneName {
    std::string name;
    u32 node = 0;
};
std::vector<LegendsN64BoneName> decodeLegendsN64BoneNames(std::span<const u8> sequences);
/** Native combat lookup compares the first fifteen case-folded characters, first match wins. */
std::optional<u32> findLegendsN64Bone(std::span<const LegendsN64BoneName> names,
                                      std::string_view wanted);
} // namespace gdl::formats
