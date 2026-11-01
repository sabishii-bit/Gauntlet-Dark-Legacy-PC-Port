#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/render/Mesh.h"

namespace gdl::formats {

/** Optional native texture pixels for a geometry survey. Empty images retain missing slots. */
struct LegendsMapTexture {
    Image image;
    bool clampU = false;
    bool clampV = false;
    bool hasTransparency = false;
    std::string name;
    bool external = false;
    u32 width = 0; ///< native dimensions, including slots borrowed from another archive
    u32 height = 0;
};

/** A native Legends world object. Native flags are not GameCube mesh flags. */
struct LegendsMapObject {
    std::string name;
    Mesh mesh;
    Mat4 transform{1.0f};
    u32 flags = 0;
    u32 nativeDrawFlags = 0; ///< N64 WOBJ drawable flags; separate from world/collision flags
    bool hasNativeDrawState = false; ///< decoded depth flags, rather than survey defaults
    s32 parent = -1;     ///< native WOBJ hierarchy; -1 for roots or unavailable platform metadata
    u32 nativeOrder = 0; ///< native depth-first lookup order without reindexing collision owners
    /** Native world triangle culling, one entry per mesh part; empty retains survey defaults. */
    std::vector<bool> partCullBack;
    bool dreamcastDrawState = false;
    /** Dreamcast's per-strip lightmap descriptor offsets, zero for no lightmap.
     * Kept separate from the archive's bitmap slots and GameCube lightmaps. */
    std::vector<u32> partLightmaps;
};

struct LegendsMapLightmap {
    u32 record = 0;
    Image image;
};

/** Initial world-space collision, decoded independently of the visible mesh. */
struct LegendsMapCollisionTriangle {
    std::array<Vec3, 3> vertices{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    u32 flags = 0;  ///< original platform metadata, not GameCube collision flags
    s32 owner = -1; ///< native world-object index, when known
    u32 nativeIndex = 0;
    bool floor = false;
    bool wall = false;
};

/** An authored player entrance; no campaign destination or save state is implied. */
struct LegendsMapStart {
    Vec3 position{0.0f};
    f32 facingRadians = 0.0f;
    u32 nativeIndex = 0;
    u32 flags = 0;
};

/** Native SWAL roster. Bounds are authored world-space WOBJ extents, not guessed mesh bounds. */
struct LegendsMapDestructible {
    u32 object = 0;
    Vec3 minimum{0};
    Vec3 maximum{0};
};

/** Native level survey and traversal data. Dynamic fixtures remain platform-specific. */
struct LegendsMap {
    Vec3 minBounds{0.0f};
    Vec3 maxBounds{0.0f};
    std::vector<LegendsMapObject> objects;
    std::vector<LegendsMapTexture> textures;
    std::vector<LegendsMapCollisionTriangle> collision;
    std::vector<LegendsMapStart> starts;
    std::vector<LegendsMapDestructible> destructibles;
    std::vector<std::string> warnings;
    std::vector<LegendsMapLightmap> lightmaps;

    /** Decodes native big-endian N64 world/model WADs; throws FormatError on malformed
     * data or unsupported geometry commands. MeshPart::texture retains the TXHD slot.
     * UVs remain in texels; normalize per part's texture dimensions. Texture pixels may be
     * decoded separately with decodeLegendsN64Textures; native material state is not applied. */
    static LegendsMap parseN64(std::span<const u8> worlds, std::span<const u8> models,
                               std::span<const u8> placementBytes = {});

    /** Decodes Dreamcast's little-endian worlds.rom and v1 objects.rom.
     * Static geometry only; MeshPart::texture retains the native bitmap index.
     * UVs are in texels; pixels can be decoded with decodeLegendsDreamcastTextures. */
    static LegendsMap parseDreamcast(std::span<const u8> worlds, std::span<const u8> models);
};

} // namespace gdl::formats
