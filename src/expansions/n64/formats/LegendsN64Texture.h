#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"

namespace gdl::formats {

struct LegendsN64Model;
struct AnimationFile;

/** One native TXHD slot, preserving placeholders rather than inventing their artwork. */
struct LegendsN64Texture {
    std::string name;
    u32 flags = 0;
    u32 displayListOffset = 0;
    u16 width = 0;
    u16 height = 0;
    u8 format = 0;
    u8 pixelSize = 0;
    bool external = false;
    bool clampU = false;
    bool clampV = false;
    bool hasTransparency = false;
    Image image;
    u32 paletteAddress = 0;       ///< Authored loader address, before segment resolution.
    bool externalPalette = false; ///< Segment 10 is supplied by the drawable's palette.
};

/** Native TSCR tile scrolling. Rates are texels per 30 Hz update; the stored
 * phase is signed 1/32 texel, not normalized coordinates or a frame cycle. */
struct LegendsN64TextureScroll {
    u32 texture = 0;
    u32 commandOffset = 0;
    Vec2 texelsPerTick{0.0f};
    u16 width = 0;
    u16 height = 0;
    std::array<s16, 2> initialPhase{};
    u16 reserved = 0;
};

/** Local presentation clock for one immutable TSCR record. Fixed updates retain
 * native integer truncation independently of the caller's rendering frequency. */
class LegendsN64TextureScrollState {
public:
    explicit LegendsN64TextureScrollState(const LegendsN64TextureScroll& scroll)
        : m_phase(scroll.initialPhase) {}
    /** Ignores nonfinite/negative intervals and intervals above one minute. */
    void advance(const LegendsN64TextureScroll& scroll, f32 seconds);
    /** Normalized UV offset: RDP tile-origin subtraction, quantized to 1/4 texel. */
    Vec2 offset(const LegendsN64TextureScroll& scroll) const;
    const std::array<s16, 2>& phase() const { return m_phase; }

private:
    std::array<s16, 2> m_phase{};
    f64 m_remainder = 0.0;
};

/** Decode the native N64 TXHD/TEXR/TXLD records, in slot order, into RGBA8.
 * Missing dynamic or borrowed artwork keeps an empty image with external set.
 * Native texture flags remain uninterpreted except for storage/loader flags.
 * Malformed records and unsupported pixel formats throw FormatError. */
std::vector<LegendsN64Texture> decodeLegendsN64Textures(std::span<const u8> objects,
                                                        std::span<const u8> dynamicPixels = {});

/** Resolve authored RDP color ramps into private decoded texture variants.
 * Original native texture slots/pixels stay unchanged. Keyed frame cycles are
 * cloned together; affected geometry retains or drops SHADE as the combiner
 * specifies. Unresolved external pixels or mixed lighting retain their original
 * materials and metadata, with an optional warning. Malformed inputs throw. */
void resolveLegendsN64ColorRamps(std::vector<LegendsN64Model>& models,
                                 std::vector<LegendsN64Texture>& textures, AnimationFile& animation,
                                 std::vector<std::string>* warnings = nullptr);

/** Decode optional TSCR records and validate their TXHD slots and RDP tile commands.
 * Missing TSCR means no authored scrolls; malformed records throw FormatError. */
std::vector<LegendsN64TextureScroll> decodeLegendsN64TextureScrolls(std::span<const u8> objects);

} // namespace gdl::formats
