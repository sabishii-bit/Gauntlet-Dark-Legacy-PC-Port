#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/** Names an object in the archive by index. */
struct ArchiveObjectDef {
    std::string name;
    f32 boundingRadius = 0.0f;
    s16 objectIndex = 0;
    s16 frames = 0;
};

/** Names a bitmap (the first frame of an animated set) by index. */
struct ArchiveBitmapDef {
    std::string name;
    u16 textureIndex = 0;
    u16 width = 0;
    u16 height = 0;
};

/** Texture format ids stored in the bitmap records. */
namespace bitmap_format {
inline constexpr u8 kRgb5a3 = 0;
inline constexpr u8 kRgb5a3NoAlpha = 1;
inline constexpr u8 kIndexed4 = 16;
inline constexpr u8 kIndexed4NoAlpha = 17;
inline constexpr u8 kIndexed4Gc = 18;
inline constexpr u8 kIndexed4Wide = 34; ///< 16-entry palette stored with 4-byte entries
inline constexpr u8 kIndexed4WideNoAlpha = 35;
inline constexpr u8 kIndexed8 = 48;
inline constexpr u8 kIndexed8NoAlpha = 49;
inline constexpr u8 kIndexed8Gc = 50;
inline constexpr u8 kIntensityAlpha8 = 56;
inline constexpr u8 kIndexed8Wide = 66; ///< 256-entry palette stored with 4-byte entries
inline constexpr u8 kIndexed8WideNoAlpha = 67;
inline constexpr u8 kAlpha8 = 130;
inline constexpr u8 kIntensity8 = 131;
inline constexpr u8 kAlpha4 = 146;
inline constexpr u8 kIntensity4 = 147;
} // namespace bitmap_format

/** Flags stored in the bitmap records. */
namespace bitmap_flags {
inline constexpr u16 kHalfResolution = 0x0001; ///< drawn at twice its pixel size
inline constexpr u16 kSeeAlpha = 0x0002;
inline constexpr u16 kClampU = 0x0004;
inline constexpr u16 kClampV = 0x0008;
inline constexpr u16 kAnimation = 0x0010;
inline constexpr u16 kExternal = 0x0020;
inline constexpr u16 kTextureShift = 0x0040;
inline constexpr u16 kHasAlpha = 0x0080;
inline constexpr u16 kInvalid = 0x0100;
inline constexpr u16 kDualTexture = 0x0200;
} // namespace bitmap_flags

/** One texture record; the pixels live in the companion textures file at dataOffset. */
struct ArchiveBitmap {
    u8 format = 0;
    s8 lodK = 0;
    u8 mipmapCount = 0;
    u16 flags = 0;
    u32 dataOffset = 0;
    u16 frameCount = 0; ///< non-zero on the first bitmap of an animated set
    u16 width = 0;
    u16 height = 0;
};

struct ArchiveSubObject {
    u16 quadwordCount = 0; ///< quadwords of geometry including its header quadword
    u16 textureIndex = 0;
    u16 lightmapIndex = 0;
    s16 lodK = 0;
    std::vector<u8> geometry; ///< packed vertex stream, quadwordCount quadwords, header first
};

struct ArchiveObject {
    f32 inverseRadius = 0.0f;
    f32 boundingRadius = 0.0f;
    u32 flags = 0;
    s32 vertexCount = 0;
    s32 triangleCount = 0;
    s32 id = 0;
    std::vector<ArchiveSubObject> subObjects;
};

/**
 * The objects.ngc model archive: object and bitmap records, their names, and the raw
 * geometry streams. Pixel data is not included; see GcTexture for reading it.
 */
class ModelArchive {
public:
    static constexpr u32 kVersion13 = 0xF00B000D;
    static constexpr u32 kVersion12 = 0xF00B000C;

    /** Parses a whole objects.ngc file; throws FormatError. */
    static ModelArchive parse(std::span<const u8> file);

    const std::string& sourceDirectory() const { return m_sourceDirectory; }
    u32 version() const { return m_version; }
    const std::vector<ArchiveObject>& objects() const { return m_objects; }
    const std::vector<ArchiveBitmap>& bitmaps() const { return m_bitmaps; }
    const std::vector<ArchiveObjectDef>& objectDefs() const { return m_objectDefs; }
    const std::vector<ArchiveBitmapDef>& bitmapDefs() const { return m_bitmapDefs; }

    /** Texture index for a bitmap name, ignoring case; nothing when unknown. */
    std::optional<u32> findBitmap(std::string_view name) const;

    /** Object index for an object name, ignoring case; nothing when unknown. */
    std::optional<u32> findObject(std::string_view name) const;

private:
    std::string m_sourceDirectory;
    u32 m_version = 0;
    std::vector<ArchiveObject> m_objects;
    std::vector<ArchiveBitmap> m_bitmaps;
    std::vector<ArchiveObjectDef> m_objectDefs;
    std::vector<ArchiveBitmapDef> m_bitmapDefs;
};

} // namespace gdl::formats
