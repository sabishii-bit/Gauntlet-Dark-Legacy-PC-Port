#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <string_view>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::formats {
namespace {

constexpr usize kMaxObjects = 16384;
constexpr usize kMaxVertices = 1000000;
constexpr usize kMaxTriangles = 2000000;
constexpr usize kModelHeader = 104;
constexpr usize kModelStride = 140;
constexpr usize kWorldStride = 60;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("Dreamcast Legends map: " + std::string(message));
    }
}

void range(std::span<const u8> bytes, usize offset, usize count, usize stride = 1) {
    require(offset <= bytes.size() && count <= (bytes.size() - offset) / stride,
            "truncated data or invalid extent");
}

f32 real(std::span<const u8> bytes, usize offset) {
    const f32 value = std::bit_cast<f32>(readU32LE(bytes, offset));
    require(std::isfinite(value) && std::abs(value) <= 1000000.0f, "invalid coordinate");
    return value;
}

Vec3 position(std::span<const u8> bytes, usize offset) {
    return {real(bytes, offset), real(bytes, offset + 4), real(bytes, offset + 8)};
}

s16 index(std::span<const u8> bytes, usize offset) {
    return std::bit_cast<s16>(readU16LE(bytes, offset));
}

std::string name(std::span<const u8> bytes, usize offset) {
    usize length = 0;
    while (length < 16 && bytes[offset + length] != 0) {
        ++length;
    }
    return {std::bit_cast<const char*>(bytes.data() + offset), length};
}

Mesh geometry(std::span<const u8> bytes, usize model, usize bitmapCount, usize bitmapRecords,
              usize& remainingVertices, usize& remainingTriangles,
              std::vector<u32>* partLightmaps = nullptr) {
    // DC 1ST_READ.BIN FUN_8c0f0e60 uses a 104-byte header and 140-byte models.
    // Each model has four 32-byte LODs. Use authored LOD 0, not a synthetic GC mesh.
    const usize lod = kModelHeader + model * kModelStride + 12;
    u32 flags = readU32LE(bytes, lod + 4);
    require((flags & ~0x417U) == 0, "unsupported model command format");
    const bool packed = (flags & 0x14U) == 4U;
    const bool authoredNormals = (flags & 0x16U) == 2U;
    const usize vertexCount = readU32LE(bytes, lod + 8);
    const usize vertices = readU32LE(bytes, lod + 12);
    const usize triangleCount = readU32LE(bytes, lod + 16);
    const usize triangles = readU32LE(bytes, lod + 20);
    require(vertexCount <= remainingVertices && triangleCount <= remainingTriangles,
            "geometry budget exceeded");
    range(bytes, vertices, vertexCount, packed ? 6 : 24);
    range(bytes, triangles, triangleCount, 8);
    // MBOX_LoadModel finishes with 8c0eff20: serialized alpha bits are stale.
    // Rebuild the flag from the referenced bitmap formats before choosing the
    // lightmap path. RGB565 floors otherwise incorrectly lose their lighting.
    flags &= ~1U;
    for (usize i = 0; i < triangleCount; ++i) {
        const usize texture = readU16LE(bytes, triangles + i * 8 + 6) & 0x3fffU;
        require(texture < bitmapCount, "triangle bitmap index out of range");
        if ((bytes[bitmapRecords + texture * 80 + 16] & 7U) != 1U)
            flags |= 1U;
    }
    const usize normals = authoredNormals ? readU32LE(bytes, lod + 28) : 0;
    if (authoredNormals) {
        range(bytes, normals, vertexCount, 12);
    }
    remainingVertices -= vertexCount;
    remainingTriangles -= triangleCount;

    Mesh mesh;
    // DrawObject (8c0f29a0) tests LOD+4 bit 0 for alpha-textured geometry.
    // Keep that reconstructed material property through the adapter; the other
    // bits describe DC vertex storage and are not shared mesh flags.
    mesh.flags = flags & mesh_flags::kAlphaTextured;
    mesh.vertices.reserve(vertexCount);
    std::vector<Vec2> normalizedUv;
    normalizedUv.reserve(vertexCount);
    usize packedCursor = vertices;
    for (usize i = 0; i < vertexCount; ++i) {
        MeshVertex vertex;
        vertex.normal = authoredNormals ? position(bytes, normals + i * 12) : Vec3(0.0f);
        if (packed) {
            // Native dispatch 8c0f29a0 -> 8c0f5160 -> 8c0f8a40. A zero
            // back-reference byte stores XYZ in 1/256 units after the UVs;
            // otherwise this six-byte record reuses an earlier position.
            range(bytes, packedCursor, 6);
            const usize back = bytes[packedCursor];
            require(back <= i, "packed vertex back-reference out of bounds");
            if (back == 0) {
                range(bytes, packedCursor, 12);
                vertex.position =
                    Vec3(index(bytes, packedCursor + 6), index(bytes, packedCursor + 8),
                         index(bytes, packedCursor + 10)) /
                    256.0f;
            } else {
                vertex.position = mesh.vertices[i - back].position;
            }
            normalizedUv.emplace_back(static_cast<f32>(index(bytes, packedCursor + 2)) / 1024.0f,
                                      static_cast<f32>(index(bytes, packedCursor + 4)) / 1024.0f);
            packedCursor += back == 0 ? 12 : 6;
            mesh.vertices.push_back(vertex);
            continue;
        }
        vertex.position = position(bytes, vertices + i * 24);
        // FUN_8c065076 multiplies lightmapped vertex UV shorts by 1/1024;
        // FUN_8c0f8d00 copies non-lightmapped UV floats unchanged. Both are
        // normalized coordinates. Convert per material below for the shared
        // preview API, whose UVs are in texels (as on the N64).
        const usize at = vertices + i * 24 + 12;
        if ((flags & 16U) != 0) {
            // 8c0f29a0's second pass reads the independent lightmap UVs
            // at vertex+16/+18, in the same 1/1024 normalized units.
            vertex.lightmapUv = Vec2(index(bytes, at + 4), index(bytes, at + 6)) / 1024.0f;
        }
        normalizedUv.emplace_back(
            (flags & 16U) != 0
                ? Vec2(static_cast<f32>(index(bytes, at)), static_cast<f32>(index(bytes, at + 2))) /
                      1024.0f
                : Vec2(real(bytes, at), real(bytes, at + 4)));
        mesh.vertices.push_back(vertex);
    }
    bool continuing = false;
    bool odd = false;
    u32 previousTexture = 0;
    usize lightmapCursor = readU32LE(bytes, lod + 28);
    u32 lightmap = 0;
    for (usize i = 0; i < triangleCount; ++i) {
        const usize at = triangles + i * 8;
        const u16 material = readU16LE(bytes, at + 6);
        const u32 texture = material & 0x3fffU;
        // 8c0f1860 consumes one 12-byte descriptor whenever bit14 starts a
        // new lightmap region. 8c0f4920 retains it across subsequent strips.
        if (partLightmaps && (flags & 17U) == 16 && (material & 0x4000U) != 0) {
            range(bytes, lightmapCursor, 12);
            require(lightmapCursor != 0, "missing lightmap descriptor");
            lightmap = static_cast<u32>(lightmapCursor);
            lightmapCursor += 12;
        }
        require(texture < bitmapCount, "triangle bitmap index out of range");
        require(!continuing || texture == previousTexture,
                "material changed within triangle strip");
        if (!continuing) {
            odd = false;
        }
        // FUN_8c0f4920 masks the bitmap to 14 bits. Bit 15 continues a
        // triangle strip; 8c0f4b1a submits [2,1,0], then the next record's
        // index 0. Its clipped path alternates successive triangle winding.
        const u32 a = readU16LE(bytes, at);
        const u32 b = readU16LE(bytes, at + (odd ? 2 : 4));
        const u32 c = readU16LE(bytes, at + (odd ? 4 : 2));
        require(a < vertexCount && b < vertexCount && c < vertexCount,
                "triangle vertex index out of range");
        if (mesh.parts.empty() || mesh.parts.back().texture != texture ||
            (partLightmaps && partLightmaps->back() != lightmap)) {
            mesh.parts.push_back(MeshPart{texture, 0, {}});
            if (partLightmaps) {
                partLightmaps->push_back(lightmap);
            }
        }
        auto& indices = mesh.parts.back().indices;
        // Native PVR strip winding is opposite to the shared renderer's
        // left-handed, on-screen counter-clockwise convention. Flip only the
        // raster indices; native positions and outward lighting normals stay
        // unchanged. Copying PVR order made one-sided fixture tops disappear
        // (notably EXIT_PORTALCENT), masked by two-sided preview geometry.
        indices.insert(indices.end(), {a, c, b});
        const Vec3 normal = glm::cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                       mesh.vertices[c].position - mesh.vertices[a].position);
        if (!authoredNormals) {
            mesh.vertices[a].normal += normal;
            mesh.vertices[b].normal += normal;
            mesh.vertices[c].normal += normal;
        }
        continuing = (material & 0x8000U) != 0;
        previousTexture = texture;
        odd = !odd;
    }
    require(!continuing, "unterminated triangle strip");
    for (auto& vertex : mesh.vertices) {
        const f32 length = glm::length(vertex.normal);
        vertex.normal = length > 0.0f ? vertex.normal / length : Vec3(0.0f, 1.0f, 0.0f);
    }
    // Vertices can be shared by materials with different texture dimensions.
    // Preserve their geometry/normals, splitting only those UV bindings.
    std::vector<usize> assignedTexture(vertexCount, bitmapCount);
    std::map<std::pair<u32, u32>, u32> splitVertices;
    for (auto& part : mesh.parts) {
        const usize bitmap = bitmapRecords + static_cast<usize>(part.texture) * 80;
        const u32 width = readU16LE(bytes, bitmap + 4);
        const u32 height = readU16LE(bytes, bitmap + 6);
        require(width <= 1024 && height <= 1024 && std::has_single_bit(width) &&
                    std::has_single_bit(height),
                "invalid bitmap dimensions");
        const Vec2 dimensions(static_cast<f32>(width), static_cast<f32>(height));
        for (auto& source : part.indices) {
            if (assignedTexture[source] == bitmapCount || assignedTexture[source] == part.texture) {
                assignedTexture[source] = part.texture;
                mesh.vertices[source].uv = normalizedUv[source] * dimensions;
            } else {
                const auto key = std::pair{source, part.texture};
                const auto found = splitVertices.find(key);
                if (found != splitVertices.end()) {
                    source = found->second;
                } else {
                    require(remainingVertices != 0, "split vertex budget exceeded");
                    --remainingVertices;
                    auto vertex = mesh.vertices[source];
                    vertex.uv = normalizedUv[source] * dimensions;
                    const u32 destination = static_cast<u32>(mesh.vertices.size());
                    mesh.vertices.push_back(vertex);
                    splitVertices.emplace(key, destination);
                    source = destination;
                }
            }
        }
    }
    return mesh;
}

struct Placement {
    Vec3 position{0.0f};
    s32 parent = -1;
    s16 next = -1;
    s16 child = -1;
    bool model = false;
};

void collisionAndStarts(LegendsMap& result, std::span<const u8> worlds, usize records,
                        const std::vector<Placement>& placements) {
    const usize triangleCount = readU32LE(worlds, 8);
    const usize triangles = readU32LE(worlds, 12);
    require(triangleCount <= kMaxTriangles, "collision budget exceeded");
    range(worlds, triangles, triangleCount, 40);
    usize remaining = kMaxTriangles;
    for (usize owner = 0; owner < placements.size(); ++owner) {
        const usize record = records + owner * kWorldStride;
        const s16 count = index(worlds, record + 54);
        if (count <= 0) {
            continue;
        }
        const s32 first = std::bit_cast<s32>(readU32LE(worlds, record + 56));
        require(first >= 0 && static_cast<usize>(first) <= triangleCount &&
                    static_cast<usize>(count) <= triangleCount - static_cast<usize>(first),
                "collision owner range out of bounds");
        require(static_cast<usize>(count) <= remaining, "collision instance budget exceeded");
        remaining -= static_cast<usize>(count);
        const u32 flags = result.objects[owner].flags;
        const s32 parent = placements[owner].parent;
        // WorldCollide (8c0ea600) consults both this object's and its immediate
        // parent's no-collision byte. Preserve disabled records for inspection,
        // but never turn them into a solid floor or wall.
        const bool enabled =
            worlds[record + 53] == 0 &&
            (parent < 0 || worlds[records + static_cast<usize>(parent) * kWorldStride + 53] == 0);
        // WorldObjCollide (8c0ea060) only transforms dynamic objects. Static
        // collision vertices are already world-space, unlike render vertices.
        const Vec3 translation =
            (flags & 0x01001000U) != 0 ? placements[owner].position : Vec3(0.0f);
        for (s32 i = 0; i < count; ++i) {
            const usize at = triangles + static_cast<usize>(first + i) * 40;
            LegendsMapCollisionTriangle triangle;
            triangle.flags = flags;
            triangle.owner = static_cast<s32>(owner);
            triangle.nativeIndex = static_cast<u32>(first + i);
            triangle.normal = position(worlds, at + 8);
            require(std::abs(glm::dot(triangle.normal, triangle.normal) - 1.0f) <= 0.001f,
                    "invalid collision plane normal");
            Vec3 u(1.0f, 0.0f, 0.0f);
            Vec3 v(0.0f, 0.0f, triangle.normal.y < 0.0f ? -1.0f : 1.0f);
            // CTri's basis builder (8c0e9d20) uses identity / inverted YZ
            // matrices at the two poles. Their stored reciprocal may be +inf.
            if (std::abs(triangle.normal.y) <= std::bit_cast<f32>(0x3f7fffefU)) {
                const f32 scale = real(worlds, at + 4);
                require(scale > 0.0f, "invalid collision plane scale");
                const auto& n = triangle.normal;
                require(std::abs((n.x * n.x + n.z * n.z) * scale * scale - 1.0f) <= 0.005f,
                        "inconsistent collision plane scale");
                u = Vec3(-n.z * scale, 0.0f, n.x * scale);
                v = Vec3(-n.x * scale * n.y, (1.0f - n.y * n.y) * scale, -n.z * scale * n.y);
            }
            triangle.vertices[0] = position(worlds, at + 20) + translation;
            for (usize vertex = 1; vertex < 3; ++vertex) {
                const usize offset = at + 32 + (vertex - 1) * 4;
                triangle.vertices[vertex] =
                    triangle.vertices[0] + u * (static_cast<f32>(index(worlds, offset)) / 64.0f) +
                    v * (static_cast<f32>(index(worlds, offset + 2)) / 64.0f);
            }
            // Native FloorCollide (8c0eac00) uses 0x23c and a +0.5 Y-normal
            // minimum; player/enemy wall wrappers use 0x13a (8c0eadc0/eae60).
            triangle.floor = enabled && (flags & 0x23cU) != 0 && triangle.normal.y >= 0.5f;
            triangle.wall = enabled && (flags & 0x13aU) != 0;
            result.collision.push_back(triangle);
        }
    }
    // Native locator setup (8c0a78a0, case 7) reads a 16-bit type, copies XYZ
    // at +4 into the player-start array, and yaw at +20 unchanged. These are
    // not camera positions or render-mesh centroids.
    const usize locatorCount = readU32LE(worlds, 88);
    const usize locators = readU32LE(worlds, 92);
    require(locatorCount <= kMaxObjects, "locator budget exceeded");
    range(worlds, locators, locatorCount, 28);
    for (usize i = 0; i < locatorCount; ++i) {
        const usize at = locators + i * 28;
        if (readU16LE(worlds, at) != 7) {
            continue;
        }
        LegendsMapStart start;
        start.position = position(worlds, at + 4);
        start.facingRadians = position(worlds, at + 16).y;
        start.nativeIndex = static_cast<u32>(i);
        start.flags = readU32LE(worlds, at);
        result.starts.push_back(start);
    }
}

} // namespace

std::vector<LegendsMapObject> decodeDreamcastModelLibrary(std::span<const u8> models) {
    range(models, 0, kModelHeader);
    require(readU32LE(models, 64) == 0xf00b0001U, "expected v1 objects.rom");
    const usize count = readU32LE(models, 96);
    const usize bitmapCount = readU32LE(models, 100);
    const usize definitions = readU32LE(models, 88);
    const usize definitionCount = readU32LE(models, 80) != 0 ? readU32LE(models, 80) : count;
    require(count <= kMaxObjects && definitionCount <= kMaxObjects && bitmapCount <= kMaxObjects,
            "model table budget exceeded");
    range(models, kModelHeader, count, kModelStride);
    const usize bitmapRecords = kModelHeader + count * kModelStride;
    range(models, bitmapRecords, bitmapCount, 80);
    range(models, definitions, definitionCount, 24);
    std::vector<LegendsMapObject> result(count);
    std::map<std::string, usize> lookup;
    for (usize i = 0; i < definitionCount; ++i) {
        const usize at = definitions + i * 24;
        const s16 object = index(models, at + 20);
        require(object >= 0 && static_cast<usize>(object) < count,
                "model definition index out of range");
        auto& model = result[object];
        require(model.name.empty(), "multiple names for one model slot");
        model.name = name(models, at);
        require(!model.name.empty() && lookup.emplace(model.name, object).second,
                "empty or duplicate model name");
    }
    usize remainingVertices = kMaxVertices;
    usize remainingTriangles = kMaxTriangles;
    for (usize i = 0; i < count; ++i) {
        result[i].flags = readU32LE(models, kModelHeader + i * kModelStride + 16);
        result[i].mesh = geometry(models, i, bitmapCount, bitmapRecords, remainingVertices,
                                  remainingTriangles, &result[i].partLightmaps);
    }
    return result;
}

LegendsMap LegendsMap::parseDreamcast(std::span<const u8> worlds, std::span<const u8> models) {
    range(worlds, 0, 96);
    range(models, 0, kModelHeader);
    require(readU32LE(models, 64) == 0xf00b0001U, "expected v1 objects.rom");
    const usize modelCount = readU32LE(models, 96);
    const usize bitmapCount = readU32LE(models, 100);
    const usize definitions = readU32LE(models, 88);
    const usize definitionCount = readU32LE(models, 80) != 0 ? readU32LE(models, 80) : modelCount;
    require(modelCount <= kMaxObjects && definitionCount <= kMaxObjects &&
                bitmapCount <= kMaxObjects,
            "model table budget exceeded");
    range(models, kModelHeader, modelCount, kModelStride);
    range(models, kModelHeader + modelCount * kModelStride, bitmapCount, 80);
    range(models, definitions, definitionCount, 24);
    std::map<std::string, usize> lookup;
    for (usize i = 0; i < definitionCount; ++i) {
        const usize at = definitions + i * 24;
        const s16 object = index(models, at + 20);
        require(object >= 0 && static_cast<usize>(object) < modelCount,
                "model definition index out of range");
        require(lookup.emplace(name(models, at), static_cast<usize>(object)).second,
                "duplicate model name");
    }

    const usize count = readU32LE(worlds, 0);
    const usize records = readU32LE(worlds, 4);
    require(count <= kMaxObjects, "world object budget exceeded");
    require(records >= 96, "world object table overlaps header");
    range(worlds, records, count, kWorldStride);
    LegendsMap result;
    result.minBounds = position(worlds, 36);
    result.maxBounds = position(worlds, 48);
    for (int axis = 0; axis < 3; ++axis) {
        require(result.minBounds[axis] <= result.maxBounds[axis], "inverted world bounds");
    }
    std::vector<Placement> placements(count);
    result.objects.resize(count);
    for (usize i = 0; i < count; ++i) {
        const usize at = records + i * kWorldStride;
        auto& placement = placements[i];
        placement.position = position(worlds, at + 28);
        placement.next = index(worlds, at + 44);
        placement.child = index(worlds, at + 46);
        const u32 hasModel = readU32LE(worlds, at + 40);
        require(hasModel <= 1, "invalid serialized model marker");
        placement.model = hasModel != 0;
        for (const s16 link : {placement.next, placement.child}) {
            require(link >= -1 && (link == -1 || static_cast<usize>(link) < count),
                    "world hierarchy link out of range");
        }
        result.objects[i].name = name(worlds, at);
        result.objects[i].flags = readU32LE(worlds, at + 16);
        result.objects[i].nativeDrawFlags = readU32LE(worlds, at + 24);
        result.objects[i].hasNativeDrawState = true;
        result.objects[i].dreamcastDrawState = true;
    }
    // Native positions are relative to the parent, including non-model groups.
    // Assign each child's sibling chain once; cycles/multiple ownership fail.
    for (usize i = 0; i < count; ++i) {
        for (s32 child = placements[i].child; child >= 0; child = placements[child].next) {
            require(placements[child].parent == -1, "cyclic or shared world child");
            placements[child].parent = static_cast<s32>(i);
        }
    }
    std::vector<bool> resolved(count, false);
    std::vector<usize> chain;
    for (usize i = 0; i < count; ++i) {
        chain.clear();
        for (s32 current = static_cast<s32>(i); current >= 0 && !resolved[current];
             current = placements[current].parent) {
            require(chain.size() < count, "cyclic world parent");
            chain.push_back(static_cast<usize>(current));
        }
        while (!chain.empty()) {
            const usize current = chain.back();
            chain.pop_back();
            auto& placement = placements[current];
            if (placement.parent >= 0) {
                placement.position += placements[placement.parent].position;
            }
            require(std::isfinite(placement.position.x) && std::isfinite(placement.position.y) &&
                        std::isfinite(placement.position.z),
                    "invalid hierarchy position");
            result.objects[current].parent = placement.parent;
            result.objects[current].transform[3] = Vec4(placement.position, 1.0f);
            resolved[current] = true;
        }
    }
    usize remainingVertices = kMaxVertices;
    usize remainingTriangles = kMaxTriangles;
    for (usize i = 0; i < count; ++i) {
        if (!placements[i].model) {
            continue;
        }
        auto& object = result.objects[i];
        const auto model = lookup.find(object.name);
        require(model != lookup.end(), "world model missing: " + object.name);
        object.mesh =
            geometry(models, model->second, bitmapCount, kModelHeader + modelCount * kModelStride,
                     remainingVertices, remainingTriangles, &object.partLightmaps);
    }
    collisionAndStarts(result, worlds, records, placements);
    result.warnings.emplace_back(
        "Dreamcast lightmaps require their native textures.rom resource at scene binding.");
    usize borrowedTextures = 0;
    for (usize i = 0; i < bitmapCount; ++i) {
        if ((models[kModelHeader + modelCount * kModelStride + i * 80 + 2] & 8U) != 0) {
            ++borrowedTextures;
        }
    }
    if (borrowedTextures != 0) {
        result.warnings.push_back(
            std::to_string(borrowedTextures) +
            " borrowed texture slots require the level's native archive lenders.");
    }
    return result;
}

} // namespace gdl::formats
