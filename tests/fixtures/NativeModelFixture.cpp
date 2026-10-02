#include "fixtures/NativeModelFixture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/assets/PngImage.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/render/Image.h"
#include "engine/render/Mesh.h"

#include "fixtures/ReferenceObj.h"
#include "formats/ModelArchive.h"

namespace gdl::test {
namespace {

constexpr usize kArchiveHeader = 160;
constexpr usize kRecordSize = 64;
constexpr usize kObjectDefSize = 24;
constexpr usize kBitmapDefSize = 36;
constexpr usize kObjectNameSize = 16;
constexpr usize kBitmapNameSize = 30;
constexpr usize kQuadword = 16;
constexpr usize kWordSize = 4;
constexpr u32 kTriangleVertices = 3;
constexpr f32 kCoordinateScale = 128.0f;

/** Random-access little-endian construction for tables containing absolute offsets. */
class Bytes {
public:
    explicit Bytes(usize size = 0) : data(size) {}

    void u16At(usize at, u16 value) {
        data.at(at) = static_cast<u8>(value);
        data.at(at + 1) = static_cast<u8>(value >> 8U);
    }
    void u32At(usize at, u32 value) {
        u16At(at, static_cast<u16>(value));
        u16At(at + 2, static_cast<u16>(value >> 16U));
    }
    void nameAt(usize at, usize size, const std::string& name) {
        if (name.size() > size) {
            throw FormatError("synthetic native name is too long: " + name);
        }
        for (usize i = 0; i < name.size(); ++i) {
            data.at(at + i) = static_cast<u8>(name[i]);
        }
    }
    usize reserve(usize size) {
        const usize at = data.size();
        data.resize(at + size);
        return at;
    }
    void append(std::span<const u8> bytes) { data.insert(data.end(), bytes.begin(), bytes.end()); }
    void align(usize alignment) {
        data.resize((data.size() + alignment - 1) / alignment * alignment);
    }

    std::vector<u8> data;
};

u16 narrowU16(usize value) {
    if (value > std::numeric_limits<u16>::max()) {
        throw FormatError("synthetic native fixture exceeds a 16-bit field");
    }
    return static_cast<u16>(value);
}

nlohmann::json readJson(const std::filesystem::path& file) {
    const auto bytes = readFile(file);
    return nlohmann::json::parse(bytes.begin(), bytes.end());
}

s32 fixedPosition(f32 value) {
    const auto scaled = std::round(static_cast<f64>(value) * kCoordinateScale);
    if (!std::isfinite(scaled) || scaled < std::numeric_limits<s32>::min() ||
        scaled > std::numeric_limits<s32>::max()) {
        throw FormatError("synthetic position is outside the native coordinate range");
    }
    return static_cast<s32>(scaled);
}

u16 fixedUv(f32 value) {
    const auto scaled = std::round(static_cast<f64>(value) * kCoordinateScale);
    if (!std::isfinite(scaled) || scaled < 0 || scaled > std::numeric_limits<u16>::max()) {
        throw FormatError("synthetic UV is outside the native coordinate range");
    }
    return static_cast<u16>(scaled);
}

u16 normalOf(const Vec3& normal) {
    u16 packed = 0;
    for (s32 axis = 0; axis < 3; ++axis) {
        const auto value =
            static_cast<u16>(std::clamp(std::lround(normal[axis] * 15), -15L, 16L) + 15);
        packed |= static_cast<u16>(value << (axis * 5));
    }
    return packed;
}

u16 colorOf(Color color) {
    return static_cast<u16>((color.r >> 3U) | ((color.g >> 3U) << 5U) | ((color.b >> 3U) << 10U));
}

/** Independent triangles avoid strip parity/restart dependencies in synthetic meshes. */
std::vector<u8> geometryOf(const Mesh& mesh, const MeshPart& part) {
    Bytes bytes(8);
    for (usize triangle = 0; triangle < part.indices.size(); triangle += kTriangleVertices) {
        const usize packet = bytes.reserve(24);
        bytes.u32At(packet, 0x6C018000);
        bytes.u32At(packet + 4, kTriangleVertices);
        bytes.u32At(packet + 20, 0x68000000);
        // Position words (four entries; the last is the packet's unused extent).
        const usize positions = bytes.reserve(48);
        const usize normals = bytes.reserve(12);
        bytes.u32At(normals, 0x6F038002);
        const usize colors = mesh.prelit ? bytes.reserve(12) : 0;
        if (mesh.prelit) {
            bytes.u32At(colors, 3);
        }
        const usize texcoords = bytes.reserve(28);
        bytes.u32At(texcoords, 0x6D000000);
        for (usize corner = 0; corner < kTriangleVertices; ++corner) {
            const auto& vertex = mesh.vertices.at(part.indices.at(triangle + corner));
            for (s32 axis = 0; axis < 3; ++axis) {
                bytes.u32At(positions + corner * 12 + static_cast<usize>(axis) * kWordSize,
                            static_cast<u32>(fixedPosition(vertex.position[axis])));
            }
            bytes.u16At(normals + 4 + corner * 2, normalOf(vertex.normal));
            if (mesh.prelit) {
                bytes.u16At(colors + 4 + corner * 2, colorOf(vertex.color));
            }
            bytes.u16At(texcoords + 4 + corner * 8, fixedUv(vertex.uv.x));
            bytes.u16At(texcoords + 6 + corner * 8, fixedUv(vertex.uv.y));
            bytes.u16At(texcoords + 8 + corner * 8, fixedUv(vertex.lightmapUv.x));
            bytes.u16At(texcoords + 10 + corner * 8, fixedUv(vertex.lightmapUv.y));
        }
        const usize end = bytes.reserve(kWordSize);
        bytes.u32At(end, 0x17000000);
    }
    bytes.reserve(kWordSize);
    bytes.align(kQuadword);
    bytes.u16At(0, narrowU16(bytes.data.size() / kQuadword - 1));
    return bytes.data;
}

u16 rgb5a3(Color color) {
    if (color.a == 255) {
        return static_cast<u16>(0x8000U | ((color.r >> 3U) << 10U) | ((color.g >> 3U) << 5U) |
                                (color.b >> 3U));
    }
    return static_cast<u16>(((color.a >> 5U) << 12U) | ((color.r >> 4U) << 8U) |
                            ((color.g >> 4U) << 4U) | (color.b >> 4U));
}

void appendPixels(Bytes& bytes, const Image& image) {
    constexpr u32 kTileSize = 4;
    for (u32 tileY = 0; tileY < image.height; tileY += kTileSize) {
        for (u32 tileX = 0; tileX < image.width; tileX += kTileSize) {
            for (u32 y = 0; y < kTileSize; ++y) {
                for (u32 x = 0; x < kTileSize; ++x) {
                    const auto color = tileX + x < image.width && tileY + y < image.height
                                           ? image.pixel(tileX + x, tileY + y)
                                           : Color::transparent();
                    const u16 packed = rgb5a3(color);
                    bytes.data.push_back(static_cast<u8>(packed >> 8U));
                    bytes.data.push_back(static_cast<u8>(packed));
                }
            }
        }
    }
}

} // namespace

void convertModelFixture(const std::filesystem::path& directory) {
    const auto objectFile = directory / "objects.json";
    const auto textureFile = directory / "textures.json";
    if (!std::filesystem::exists(objectFile) && !std::filesystem::exists(textureFile)) {
        return;
    }
    const auto objects = std::filesystem::exists(objectFile) ? readJson(objectFile).at("objects")
                                                             : nlohmann::json::array();
    const auto textures = std::filesystem::exists(textureFile)
                              ? readJson(textureFile)
                              : nlohmann::json{{"bitmaps", nlohmann::json::array()}};
    const auto& bitmaps = textures.at("bitmaps");
    auto bitmapDefs = nlohmann::json::array();
    for (usize i = 0; i < bitmaps.size(); ++i) {
        bitmapDefs.push_back({{"name", bitmaps[i].at("name")}, {"index", i}});
    }
    for (const auto& def : textures.value("defs", nlohmann::json::array())) {
        bitmapDefs.push_back(def);
    }

    Bytes archive(kArchiveHeader);
    archive.u32At(64, formats::ModelArchive::kVersion13);
    const usize objectTable = archive.reserve(objects.size() * kRecordSize);
    const usize bitmapTable = archive.reserve(bitmaps.size() * kRecordSize);
    const usize objectNames = archive.reserve(objects.size() * kObjectDefSize);
    const usize bitmapNames = archive.reserve(bitmapDefs.size() * kBitmapDefSize);
    archive.u32At(68, static_cast<u32>(objects.size()));
    archive.u32At(72, static_cast<u32>(bitmaps.size()));
    archive.u32At(76, static_cast<u32>(objects.size()));
    archive.u32At(80, static_cast<u32>(bitmapDefs.size()));
    archive.u32At(84, static_cast<u32>(objectTable));
    archive.u32At(88, static_cast<u32>(bitmapTable));
    archive.u32At(92, static_cast<u32>(objectNames));
    archive.u32At(96, static_cast<u32>(bitmapNames));

    for (usize i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        const usize record = objectTable + i * kRecordSize;
        const usize name = objectNames + i * kObjectDefSize;
        archive.nameAt(name, kObjectNameSize, object.value("name", std::string{}));
        archive.u16At(name + 20, narrowU16(i));
        if (!object.contains("file")) {
            continue;
        }
        const Mesh mesh = loadObj(directory / object.at("file").get<std::string>());
        archive.u32At(record + 12, static_cast<u32>(mesh.parts.size()));
        archive.u32At(record + 32, static_cast<u32>(mesh.vertices.size()));
        archive.u32At(record + 36, static_cast<u32>(mesh.triangleCount()));
        if (mesh.parts.empty()) {
            continue;
        }
        const usize extraParts = archive.reserve((mesh.parts.size() - 1) * 8);
        archive.u32At(record + 24, static_cast<u32>(extraParts));
        archive.align(kQuadword);
        archive.u32At(record + 28, static_cast<u32>(archive.data.size()));
        for (usize p = 0; p < mesh.parts.size(); ++p) {
            const auto& part = mesh.parts[p];
            const auto geometry = geometryOf(mesh, part);
            const usize sub = p == 0 ? record + 16 : extraParts + (p - 1) * 8;
            archive.u16At(sub, narrowU16(geometry.size() / kQuadword));
            archive.u16At(sub + 2, narrowU16(part.texture));
            archive.u16At(sub + 4, narrowU16(part.lightmap));
            archive.append(geometry);
        }
    }

    Bytes pixels;
    for (usize i = 0; i < bitmaps.size(); ++i) {
        const auto& bitmap = bitmaps[i];
        const usize record = bitmapTable + i * kRecordSize;
        auto flags = bitmap.value("flags", u16{0});
        if (bitmap.value("halfResolution", false)) {
            flags |= formats::bitmap_flags::kHalfResolution;
        }
        if (bitmap.value("clampU", false)) {
            flags |= formats::bitmap_flags::kClampU;
        }
        if (bitmap.value("clampV", false)) {
            flags |= formats::bitmap_flags::kClampV;
        }
        archive.u16At(record + 8, flags);
        archive.u16At(record + 20, bitmap.value("frames", u16{0}));
        archive.u16At(record + 22, bitmap.at("width").get<u16>());
        archive.u16At(record + 24, bitmap.at("height").get<u16>());
        archive.u32At(record + 12, static_cast<u32>(pixels.data.size()));
        if ((flags & (formats::bitmap_flags::kInvalid | formats::bitmap_flags::kExternal)) == 0) {
            const Image image = loadImageFile(directory / bitmap.at("file").get<std::string>());
            // Small stand-in pictures are expanded to the fixture's authored dimensions.
            const auto width = bitmap.at("width").get<u16>();
            const auto height = bitmap.at("height").get<u16>();
            Image sized = Image::filled(width, height, Color::transparent());
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    sized.setPixel(x, y,
                                   image.pixel(x * image.width / width, y * image.height / height));
                }
            }
            appendPixels(pixels, sized);
        }
    }
    for (usize i = 0; i < bitmapDefs.size(); ++i) {
        const auto& def = bitmapDefs[i];
        const usize record = bitmapNames + i * kBitmapDefSize;
        archive.nameAt(record, kBitmapNameSize, def.at("name").get<std::string>());
        const auto index = def.at("index").get<u16>();
        archive.u16At(record + 30, index);
        archive.u16At(record + 32, bitmaps.at(index).at("width").get<u16>());
        archive.u16At(record + 34, bitmaps.at(index).at("height").get<u16>());
    }
    writeFile(directory / "objects.ngc", archive.data);
    writeFile(directory / "textures.ngc", pixels.data);
}

void convertModelFixtures(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root)) {
        return;
    }
    std::vector<std::filesystem::path> directories{root};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_directory()) {
            directories.push_back(entry.path());
        }
    }
    for (const auto& directory : directories) {
        convertModelFixture(directory);
    }
}

} // namespace gdl::test
