#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

/** Names an object in the archive by index. */
struct ArchiveObjectDef {
    std::string name;
    float boundingRadius = 0.0f;
    std::int16_t objectIndex = 0;
    std::int16_t frames = 0;
};

/** Names a bitmap (the first frame of an animated set) by index. */
struct ArchiveBitmapDef {
    std::string name;
    std::uint16_t textureIndex = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

/** Texture format ids stored in the bitmap records. */
namespace bitmap_format {
inline constexpr std::uint8_t kRgb5a3 = 0;
inline constexpr std::uint8_t kRgb5a3NoAlpha = 1;
inline constexpr std::uint8_t kIndexed4 = 16;
inline constexpr std::uint8_t kIndexed4NoAlpha = 17;
inline constexpr std::uint8_t kIndexed4Gc = 18;
inline constexpr std::uint8_t kIndexed4Wide = 34; ///< 16-entry palette stored with 4-byte entries
inline constexpr std::uint8_t kIndexed4WideNoAlpha = 35;
inline constexpr std::uint8_t kIndexed8 = 48;
inline constexpr std::uint8_t kIndexed8NoAlpha = 49;
inline constexpr std::uint8_t kIndexed8Gc = 50;
inline constexpr std::uint8_t kIntensityAlpha8 = 56;
inline constexpr std::uint8_t kIndexed8Wide = 66; ///< 256-entry palette stored with 4-byte entries
inline constexpr std::uint8_t kIndexed8WideNoAlpha = 67;
inline constexpr std::uint8_t kAlpha8 = 130;
inline constexpr std::uint8_t kIntensity8 = 131;
inline constexpr std::uint8_t kAlpha4 = 146;
inline constexpr std::uint8_t kIntensity4 = 147;
} // namespace bitmap_format

/** Flags stored in the bitmap records. */
namespace bitmap_flags {
inline constexpr std::uint16_t kHalfResolution = 0x0001; ///< drawn at twice its pixel size
inline constexpr std::uint16_t kSeeAlpha = 0x0002;
inline constexpr std::uint16_t kClampU = 0x0004;
inline constexpr std::uint16_t kClampV = 0x0008;
inline constexpr std::uint16_t kAnimation = 0x0010;
inline constexpr std::uint16_t kExternal = 0x0020;
inline constexpr std::uint16_t kTextureShift = 0x0040;
inline constexpr std::uint16_t kHasAlpha = 0x0080;
inline constexpr std::uint16_t kInvalid = 0x0100;
inline constexpr std::uint16_t kDualTexture = 0x0200;
} // namespace bitmap_flags

/** One texture record; the pixels live in the companion textures file at dataOffset. */
struct ArchiveBitmap {
    std::uint8_t format = 0;
    std::int8_t lodK = 0;
    std::uint8_t mipmapCount = 0;
    std::uint16_t flags = 0;
    std::uint32_t dataOffset = 0;
    std::uint16_t frameCount = 0; ///< non-zero on the first bitmap of an animated set
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct ArchiveSubObject {
    std::uint16_t quadwordCount = 0; ///< quadwords of geometry including its header quadword
    std::uint16_t textureIndex = 0;
    std::uint16_t lightmapIndex = 0;
    std::int16_t lodK = 0;
    std::vector<std::uint8_t>
        geometry; ///< packed vertex stream, quadwordCount quadwords, header first
};

struct ArchiveObject {
    float inverseRadius = 0.0f;
    float boundingRadius = 0.0f;
    std::uint32_t flags = 0;
    std::int32_t vertexCount = 0;
    std::int32_t triangleCount = 0;
    std::int32_t id = 0;
    std::vector<ArchiveSubObject> subObjects;
};

/**
 * The objects.ngc model archive: object and bitmap records, their names, and the raw
 * geometry streams. Pixel data is not included; see GcTexture for reading it.
 */
class ModelArchive {
public:
    static constexpr std::uint32_t kVersion13 = 0xF00B000D;
    static constexpr std::uint32_t kVersion12 = 0xF00B000C;

    /** Parses a whole objects.ngc file; throws FormatError. */
    static ModelArchive parse(std::span<const std::uint8_t> file);

    const std::string& sourceDirectory() const { return m_sourceDirectory; }
    std::uint32_t version() const { return m_version; }
    const std::vector<ArchiveObject>& objects() const { return m_objects; }
    const std::vector<ArchiveBitmap>& bitmaps() const { return m_bitmaps; }
    const std::vector<ArchiveObjectDef>& objectDefs() const { return m_objectDefs; }
    const std::vector<ArchiveBitmapDef>& bitmapDefs() const { return m_bitmapDefs; }

    /** Texture index for a bitmap name, ignoring case; nothing when unknown. */
    std::optional<std::uint32_t> findBitmap(std::string_view name) const;

    /** Object index for an object name, ignoring case; nothing when unknown. */
    std::optional<std::uint32_t> findObject(std::string_view name) const;

private:
    std::string m_sourceDirectory;
    std::uint32_t m_version = 0;
    std::vector<ArchiveObject> m_objects;
    std::vector<ArchiveBitmap> m_bitmaps;
    std::vector<ArchiveObjectDef> m_objectDefs;
    std::vector<ArchiveBitmapDef> m_bitmapDefs;
};

} // namespace gdl::formats
