#include "expansions/n64/formats/LegendsN64IntensityAlpha.h"

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {
namespace {

constexpr u32 kOpaque = 255;
constexpr u32 kNibbleScale = 17; ///< a nibble's 15 is 255

} // namespace

Color legendsN64IntensityAlpha4(u32 bits) {
    const u32 three = (bits >> 1U) & 7U;
    const auto intensity = static_cast<u8>((three << 5U) | (three << 2U) | (three >> 1U));
    return Color::rgba(intensity, intensity, intensity, static_cast<u8>((bits & 1U) * kOpaque));
}

Color legendsN64IntensityAlpha8(u32 bits) {
    const auto intensity = static_cast<u8>(((bits >> 4U) & 15U) * kNibbleScale);
    return Color::rgba(intensity, intensity, intensity,
                       static_cast<u8>((bits & 15U) * kNibbleScale));
}

} // namespace gdl::formats
