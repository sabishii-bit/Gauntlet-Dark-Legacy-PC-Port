#include "expansions/n64/formats/LegendsN64Texture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/formats/LegendsN64Scene.h"

namespace gdl::formats {

namespace {

constexpr usize kRecordSize = 24;
constexpr usize kDirectoryEntrySize = 12;
constexpr usize kMaxTextures = 4096;
constexpr u16 kMaxDimension = 4096;
constexpr usize kMaxDecodedBytes = usize{128} * 1024 * 1024;
constexpr u32 kBorrowedPixels = 0xFFFFFFFE;
constexpr u32 kTemplateLoader = 4;
constexpr u32 kDynamicPixels = 0x80;
constexpr usize kMaxLoadCommands = 128;

void checkRange(std::span<const u8> data, usize offset, usize size) {
    if (offset > data.size() || size > data.size() - offset) {
        throw FormatError("N64 texture record extends outside its section");
    }
}

u16 read16(std::span<const u8> data, usize offset) {
    checkRange(data, offset, 2);
    return static_cast<u16>((u32{data[offset]} << 8U) | data[offset + 1]);
}

u32 read32(std::span<const u8> data, usize offset) {
    checkRange(data, offset, 4);
    return (u32{data[offset]} << 24U) | (u32{data[offset + 1]} << 16U) |
           (u32{data[offset + 2]} << 8U) | data[offset + 3];
}

struct Section {
    usize offset = 0;
    usize count = 0;
    usize limit = 0;
};

Section findSection(std::span<const u8> data, std::string_view tag, bool required = true) {
    const auto directory = read32(data, 0);
    const auto count = read32(data, 4);
    if (directory < 8 || count > 65536) {
        throw FormatError("Invalid N64 object WAD directory");
    }
    checkRange(data, directory, usize{count} * kDirectoryEntrySize);
    std::optional<Section> found;
    for (usize i = 0; i < count; ++i) {
        const auto at = usize{directory} + i * kDirectoryEntrySize;
        if (std::equal(tag.begin(), tag.end(), data.begin() + static_cast<std::ptrdiff_t>(at))) {
            if (found) {
                throw FormatError("Duplicate N64 object WAD section");
            }
            found = Section{read32(data, at + 4), read32(data, at + 8), directory};
            if (found->offset < 8 || found->offset > directory) {
                throw FormatError("N64 object WAD section overlaps directory");
            }
        }
    }
    if (!found) {
        if (!required) {
            return {};
        }
        throw FormatError("Missing N64 texture WAD section");
    }
    for (usize i = 0; i < count; ++i) {
        const auto offset = read32(data, usize{directory} + i * kDirectoryEntrySize + 4);
        if (offset > found->offset) {
            found->limit = std::min(found->limit, usize{offset});
        }
    }
    return *found;
}

void checkSection(const Section& section, usize bytes) {
    if (bytes > section.limit - section.offset) {
        throw FormatError("N64 texture section overlaps another section or its directory");
    }
}

struct Loader {
    std::optional<usize> palette;
    usize rowBytes = 0;
    bool clampU = false;
    bool clampV = false;
};

Loader inspectLoader(std::span<const u8> commands, bool templated) {
    Loader result;
    bool seenImage = false;
    for (usize index = 0; index < kMaxLoadCommands; ++index) {
        const auto command = read32(commands, index * 8);
        const auto value = read32(commands, index * 8 + 4);
        const auto opcode = command >> 24U;
        if (templated && (command & 0xFFFF0000U) == 0xE0020000U) {
            // US game.bin e002b56c builds the shared loader templates; bit 8 clamps both axes.
            result.clampU = result.clampV = (command & 0x100U) != 0;
            return result;
        }
        if (opcode == 0xDF) {
            return result;
        }
        if (opcode == 0xFD) {
            if (seenImage && command == 0xFD100000U) {
                result.palette = value;
            }
            seenImage = true;
        }
        if (opcode == 0xF5 && (value >> 24U) == 0) {
            result.rowBytes = usize{(command >> 9U) & 0x1ffU} * 8;
            result.clampU = (value & 0x200U) != 0;
            result.clampV = (value & 0x80000U) != 0;
        }
    }
    throw FormatError("Unterminated N64 texture load display list");
}

Color rgba16(u16 value) {
    const auto expand = [](u32 component) {
        return static_cast<u8>((component << 3U) | (component >> 2U));
    };
    return Color::rgba(expand((value >> 11U) & 31U), expand((value >> 6U) & 31U),
                       expand((value >> 1U) & 31U), (value & 1U) != 0 ? 255 : 0);
}

Image decodePixels(const LegendsN64Texture& texture, std::span<const u8> data, usize offset,
                   usize storageSize, usize paletteOffset, usize rowBytes) {
    const auto pixelCount = usize{texture.width} * texture.height;
    // The native format table at e002c258 maps the game's extended types to
    // RDP CI, I and IA respectively. Types 6/7 retain the same intensity
    // storage; their material/vertex tint is a draw property, not pixel data.
    const bool pairedAlpha = texture.format == 5;
    const auto format = texture.format == 5   ? 2
                        : texture.format == 6 ? 4
                        : texture.format == 7 ? 3
                                              : texture.format;
    const auto size = texture.pixelSize;
    const bool sized = (format == 0 && (size == 2 || size == 3)) ||
                       (format == 2 && (size == 0 || size == 1)) || (format == 3 && size <= 2) ||
                       (format == 4 && size <= 1);
    if (!sized || (pairedAlpha && size != 1) || (texture.format == 6 && size != 0) ||
        (texture.format == 7 && size != 1)) {
        throw FormatError("Unsupported N64 texture pixel format");
    }
    const usize packedRow = (usize{texture.width} * (usize{4} << size) + 7) / 8;
    // The render tile's line is in 64-bit TMEM words; RGBA32 splits its
    // channels across two banks. LoadBlock sources retain this row padding:
    // SLIME METER_BG1/2 are200 CI4 texels but13 words (104 bytes) per row,
    // not100. TXHD allocates3328 bytes, and TXLD loads exactly that extent.
    // Ignoring the pitch shifts every subsequent row of the meter artwork.
    if (size == 3)
        rowBytes *= 2;
    if (rowBytes == 0)
        rowBytes = packedRow; // Compact loader or no explicit render-tile stride.
    if (rowBytes < packedRow)
        throw FormatError("N64 texture row pitch is smaller than its image");
    const usize byteCount = (usize{texture.height} - 1) * rowBytes + packedRow;
    if (byteCount > storageSize) {
        throw FormatError("N64 texture pixels exceed their allocated storage");
    }
    checkRange(data, offset, byteCount);
    std::array<Color, 256> palette{};
    if (format == 2) {
        const usize entries = size == 0 ? 16 : 256;
        checkRange(data, paletteOffset, entries * 2);
        for (usize i = 0; i < entries; ++i) {
            palette[i] = rgba16(read16(data, paletteOffset + i * 2));
        }
    }
    Image result{texture.width, texture.height, std::vector<u8>(pixelCount * 4)};
    for (usize i = 0; i < pixelCount; ++i) {
        const usize x = i % texture.width;
        const usize row = offset + (i / texture.width) * rowBytes;
        Color color{};
        if (format == 0 && size == 3) {
            const auto at = row + x * 4;
            color = Color::rgba(data[at], data[at + 1], data[at + 2], data[at + 3]);
        } else if (format == 0) {
            color = rgba16(read16(data, row + x * 2));
        } else if (format == 2) {
            const auto index =
                size == 0 ? (data[row + x / 2] >> ((x & 1U) == 0 ? 4U : 0U)) & 15U : data[row + x];
            color = palette[index];
            if (pairedAlpha) {
                // Native e002b184 configures a second CI4 tile at twice S;
                // it sees the high nibble of the same CI8 texel. The blit
                // combiner e0015438 (fcffabff/ff0c923f, ENV alpha 0x55)
                // computes (A1-A0)/3+A0, retaining four authored alpha levels.
                const int alpha = color.a;
                color.a = static_cast<u8>(alpha + (int{palette[index >> 4U].a} - alpha) / 3);
            }
        } else if (format == 3) {
            if (size == 0) {
                const auto value = (data[row + x / 2] >> ((x & 1U) == 0 ? 4U : 0U)) & 15U;
                const auto three = value >> 1U;
                const auto intensity =
                    static_cast<u8>((three << 5U) | (three << 2U) | (three >> 1U));
                color = Color::rgba(intensity, intensity, intensity, (value & 1U) != 0 ? 255 : 0);
            } else if (size == 1) {
                const auto value = data[row + x];
                const auto intensity = static_cast<u8>((value >> 4U) * 17U);
                color = Color::rgba(intensity, intensity, intensity,
                                    static_cast<u8>((value & 15U) * 17U));
            } else {
                const auto intensity = data[row + x * 2];
                color = Color::rgba(intensity, intensity, intensity, data[row + x * 2 + 1]);
            }
        } else {
            const auto intensity =
                size == 1 ? data[row + x]
                          : static_cast<u8>(
                                ((data[row + x / 2] >> ((x & 1U) == 0 ? 4U : 0U)) & 15U) * 17U);
            color = Color::rgba(intensity, intensity, intensity, intensity);
        }
        result.setPixel(static_cast<u32>(i % texture.width), static_cast<u32>(i / texture.width),
                        color);
    }
    return result;
}

} // namespace

std::vector<LegendsN64Texture> decodeLegendsN64Textures(std::span<const u8> objects,
                                                        std::span<const u8> dynamicPixels) {
    const auto headers = findSection(objects, "TXHD");
    const auto names = findSection(objects, "NAME");
    const auto pixels = findSection(objects, "TEXR");
    const auto loaders = findSection(objects, "TXLD");
    if (headers.count > kMaxTextures) {
        throw FormatError("Too many N64 texture records");
    }
    checkSection(headers, headers.count * kRecordSize);
    checkSection(names, names.count);
    checkSection(pixels, pixels.count);
    checkSection(loaders, loaders.count);
    const auto nameBytes = objects.subspan(names.offset, names.count);
    const auto pixelBytes = objects.subspan(pixels.offset, pixels.count);
    std::vector<LegendsN64Texture> result;
    result.reserve(headers.count);
    usize decodedBytes = 0;
    for (usize i = 0; i < headers.count; ++i) {
        const auto at = headers.offset + i * kRecordSize;
        const auto pixelOffset = read32(objects, at);
        LegendsN64Texture texture;
        texture.displayListOffset = read32(objects, at + 4);
        texture.flags = read32(objects, at + 8);
        texture.width = read16(objects, at + 12);
        texture.height = read16(objects, at + 14);
        // Native e001d924 binds TXHD slots; e0026674 sizes their storage in eight-byte units.
        const auto storageSize = usize{read16(objects, at + 16)} * 8;
        texture.format = objects[at + 18];
        texture.pixelSize = objects[at + 19];
        const auto nameOffset = read16(objects, at + 20);
        checkRange(nameBytes, nameOffset, 1);
        const auto text = nameBytes.subspan(nameOffset);
        const auto nameEnd = std::ranges::find(text, u8{0});
        if (nameEnd == text.end()) {
            throw FormatError("Unterminated N64 texture name");
        }
        texture.name.assign(text.begin(), nameEnd);
        if (texture.width == 0 || texture.height == 0 || texture.width > kMaxDimension ||
            texture.height > kMaxDimension) {
            throw FormatError("Invalid N64 texture dimensions");
        }
        decodedBytes += usize{texture.width} * texture.height * 4;
        if (decodedBytes > kMaxDecodedBytes) {
            throw FormatError("N64 texture archive exceeds decoded image limit");
        }
        if (texture.displayListOffset < loaders.offset ||
            texture.displayListOffset >= loaders.offset + loaders.count) {
            throw FormatError("N64 texture load address is outside TXLD");
        }
        const auto loader = inspectLoader(
            objects.subspan(texture.displayListOffset,
                            loaders.offset + loaders.count - texture.displayListOffset),
            (texture.flags & kTemplateLoader) != 0);
        texture.clampU = loader.clampU;
        texture.clampV = loader.clampV;
        const bool dynamic = (texture.flags & kDynamicPixels) != 0;
        texture.paletteAddress = static_cast<u32>(loader.palette.value_or(0));
        // e001ea34 binds segment 10 from the drawable's selected palette. Its
        // player colour is not recoverable from this standalone texture record.
        texture.externalPalette = loader.palette && (texture.paletteAddress >> 24U) == 10;
        texture.external = pixelOffset == kBorrowedPixels || (dynamic && dynamicPixels.empty()) ||
                           texture.externalPalette;
        if (!texture.external) {
            const auto source = dynamic ? dynamicPixels : pixelBytes;
            checkRange(source, pixelOffset, storageSize);
            usize palette = loader.palette.value_or(usize{pixelOffset} + storageSize);
            if (loader.palette && (texture.paletteAddress >> 24U) != 0) {
                // e00259d0 streams one texture frame and its palette together;
                // segment 12 is that frame's pixel base, not the archive base.
                if ((texture.paletteAddress >> 24U) != 12 || !dynamic) {
                    throw FormatError("Unsupported N64 texture palette segment");
                }
                palette = usize{pixelOffset} + (texture.paletteAddress & 0xFFFFFFU);
            }
            texture.image =
                decodePixels(texture, source, pixelOffset, storageSize, palette, loader.rowBytes);
            for (usize alpha = 3; alpha < texture.image.pixels.size(); alpha += 4) {
                if (texture.image.pixels[alpha] != 255) {
                    texture.hasTransparency = true;
                    break;
                }
            }
        }
        result.push_back(std::move(texture));
    }
    return result;
}

void resolveLegendsN64ColorRamps(std::vector<LegendsN64Model>& models,
                                 std::vector<LegendsN64Texture>& textures, AnimationFile& animation,
                                 std::vector<std::string>* warnings) {
    const auto require = [](bool value, const char* message) {
        if (!value) {
            throw FormatError(std::string("N64 color ramp: ") + message);
        }
    };
    const usize nativeTextures = textures.size();
    // Shared property7 clocks (for example DEATH/ARC's DETH cycle) replace
    // an archive slot, independently of the tree's pose. Retint the complete
    // cycle as well as its destination; copying only the initial image leaves
    // the converted material permanently stuck on its often-transparent frame.
    std::vector<TextureAnimation> sharedCycles;
    for (const auto& motion : animation.textureAnimations) {
        if (motion.flag == -1 && motion.texture >= 0 && motion.source >= 0 && motion.frames > 0)
            sharedCycles.push_back(motion);
    }
    const auto available = [&](u32 first, u32 count) {
        require(first < nativeTextures && count > 0 && count <= nativeTextures - first,
                "texture range is outside native slots");
        for (u32 i = 0; i < count; ++i) {
            if (textures[first + i].external || textures[first + i].image.pixels.empty()) {
                return false;
            }
        }
        return true;
    };
    usize remainingBytes = usize{64} * 1024 * 1024;
    using RampKey = std::tuple<u32, u32, u8, u8, u8, u8, u8, u8>;
    std::map<RampKey, u32> converted;
    const auto convert = [&](u32 first, u32 count, const LegendsN64ColorRamp& ramp) {
        const auto& low = ramp.primitive;
        const auto& high = ramp.environment;
        const RampKey key{first, count, low.r, low.g, low.b, high.r, high.g, high.b};
        if (const auto old = converted.find(key); old != converted.end()) {
            return old->second;
        }
        require(first < nativeTextures && count > 0 && count <= nativeTextures - first,
                "texture range is outside native slots");
        require(textures.size() <= 32767 && count <= 32767 - textures.size(),
                "texture variant count exceeds common animation indices");
        usize charge = 0;
        for (u32 i = 0; i < count; ++i) {
            const auto& source = textures[first + i];
            require(!source.external && !source.image.pixels.empty(),
                    "authored texture pixels are unavailable");
            require(source.image.width != 0 && source.image.height != 0 &&
                        source.image.width <=
                            source.image.pixels.size() / 4 / source.image.height &&
                        usize{source.image.width} * source.image.height * 4 ==
                            source.image.pixels.size(),
                    "invalid RGBA image extent");
            require(source.image.pixels.size() <= remainingBytes - charge,
                    "texture variant byte budget exceeded");
            charge += source.image.pixels.size();
        }
        const auto start = static_cast<u32>(textures.size());
        for (u32 i = 0; i < count; ++i) {
            auto result = textures[first + i];
            result.name += ":RDP:" + std::to_string(start);
            for (usize pixel = 0; pixel < result.image.pixels.size(); pixel += 4) {
                const std::array<u8, 3> a{low.r, low.g, low.b};
                const std::array<u8, 3> b{high.r, high.g, high.b};
                for (usize channel = 0; channel < 3; ++channel) {
                    const int sample = result.image.pixels[pixel + channel];
                    result.image.pixels[pixel + channel] = static_cast<u8>(
                        (int{a[channel]} * (255 - sample) + int{b[channel]} * sample + 127) / 255);
                }
            }
            textures.push_back(std::move(result));
        }
        remainingBytes -= charge;
        converted.emplace(key, start);
        return start;
    };
    std::map<std::string, usize> byName;
    for (usize index = 0; index < models.size(); ++index) {
        byName.emplace(models[index].name, index);
    }
    // Preflight a whole model before changing either its animation or geometry.
    // Some native archives borrow palettes/images from a later bank; an unused
    // original model can also retain a placeholder while its node variant has
    // real frames. Leave those materials intact for the ordinary lender path.
    std::vector<bool> supported(models.size(), true);
    for (usize index = 0; index < models.size(); ++index) {
        const auto& model = models[index];
        if (model.colorRamps.empty()) {
            continue;
        }
        for (const auto& ramp : model.colorRamps) {
            require(ramp.part < model.mesh.parts.size(), "part is outside its model");
            const u32 texture = model.mesh.parts[ramp.part].texture;
            if (texture != LegendsN64Model::kRuntimeTexture && !available(texture, 1)) {
                supported[index] = false;
            }
            for (const auto& motion : sharedCycles) {
                if (static_cast<u32>(motion.texture) == texture &&
                    !available(static_cast<u32>(motion.source), static_cast<u32>(motion.frames)))
                    supported[index] = false;
            }
        }
        if (!model.mesh.prelit) {
            const bool lit = model.colorRamps.front().shadeRgb;
            for (usize part = 0; part < model.mesh.parts.size(); ++part) {
                if (std::ranges::none_of(model.colorRamps, [&](const auto& ramp) {
                        return ramp.part == part && ramp.shadeRgb == lit;
                    })) {
                    supported[index] = false;
                }
            }
        }
    }
    for (const auto& tree : animation.trees) {
        for (const auto& node : tree.nodes) {
            const auto found = byName.find(node.object);
            if (found == byName.end() || node.textureAnimation < 0 ||
                models[found->second].colorRamps.empty()) {
                continue;
            }
            require(static_cast<usize>(node.textureAnimation) < animation.textureAnimations.size(),
                    "node animation is outside its archive");
            const auto& motion =
                animation.textureAnimations[static_cast<usize>(node.textureAnimation)];
            if (motion.flag != 0 || motion.source < 0 || motion.texture < 0 || motion.frames <= 0 ||
                !available(static_cast<u32>(motion.source), static_cast<u32>(motion.frames))) {
                supported[found->second] = false;
            }
        }
    }
    const usize omitted = static_cast<usize>(std::ranges::count(supported, false));
    if (omitted != 0 && warnings != nullptr) {
        warnings->push_back(std::to_string(omitted) +
                            " native color-ramp models retain original materials: unresolved "
                            "texture dependencies or mixed lighting.");
    }
    std::map<std::pair<usize, usize>, u32> bindings;
    for (auto& tree : animation.trees) {
        for (auto& node : tree.nodes) {
            const auto found = byName.find(node.object);
            if (found == byName.end() || node.textureAnimation < 0 || !supported[found->second]) {
                continue;
            }
            auto& model = models[found->second];
            if (model.colorRamps.empty()) {
                continue;
            }
            require(static_cast<usize>(node.textureAnimation) < animation.textureAnimations.size(),
                    "node animation is outside its archive");
            auto motion = animation.textureAnimations[static_cast<usize>(node.textureAnimation)];
            require(motion.flag == 0 && motion.source >= 0 && motion.texture >= 0 &&
                        motion.frames > 0,
                    "unsupported color-ramp animation");
            std::optional<LegendsN64ColorRamp> selected;
            for (const auto& ramp : model.colorRamps) {
                require(ramp.part < model.mesh.parts.size(), "part is outside its model");
                if (model.mesh.parts[ramp.part].texture != static_cast<u32>(motion.texture)) {
                    continue;
                }
                require(!selected || (selected->primitive == ramp.primitive &&
                                      selected->environment == ramp.environment),
                        "one node animation has conflicting color ramps");
                selected = ramp;
            }
            if (!selected) {
                continue;
            }
            const u32 target = convert(static_cast<u32>(motion.source),
                                       static_cast<u32>(motion.frames), *selected);
            const auto nativeSlot = static_cast<u32>(motion.texture);
            motion.texture = static_cast<s32>(target);
            motion.source = static_cast<s32>(target);
            motion.name += ":RDP";
            require(animation.textureAnimations.size() < 32767, "animation budget exceeded");
            node.textureAnimation = static_cast<s32>(animation.textureAnimations.size());
            animation.textureAnimations.push_back(std::move(motion));
            for (const auto& ramp : model.colorRamps) {
                if (model.mesh.parts[ramp.part].texture != nativeSlot) {
                    continue;
                }
                const auto [existing, inserted] =
                    bindings.emplace(std::pair{found->second, usize{ramp.part}}, target);
                require(inserted || existing->second == target,
                        "shared model has conflicting animation bindings");
            }
        }
    }
    std::set<std::pair<usize, u32>> convertedCycles;
    for (usize modelIndex = 0; modelIndex < models.size(); ++modelIndex) {
        if (!supported[modelIndex]) {
            continue;
        }
        auto& model = models[modelIndex];
        if (!model.mesh.prelit && !model.colorRamps.empty() && !model.colorRamps.front().shadeRgb) {
            // A native model may enable RSP lighting while its explicit RDP
            // combiner ignores SHADE. The common mesh can bypass lighting only
            // if every part supplies that combiner; never alter a mixed mesh.
            for (usize part = 0; part < model.mesh.parts.size(); ++part) {
                require(std::ranges::any_of(model.colorRamps,
                                            [&](const auto& ramp) { return ramp.part == part; }),
                        "mixed lit/shadeless color-ramp model is unsupported");
            }
            model.mesh.prelit = true;
        }
        for (const auto& ramp : model.colorRamps) {
            require(ramp.part < model.mesh.parts.size(), "part is outside its model");
            auto& part = model.mesh.parts[ramp.part];
            if (part.texture == LegendsN64Model::kRuntimeTexture) {
                continue; // The node-specific variant carries its actual texture binding.
            }
            const auto found = bindings.find({modelIndex, ramp.part});
            const u32 nativeSlot = part.texture;
            part.texture = found == bindings.end() ? convert(nativeSlot, 1, ramp) : found->second;
            if (found == bindings.end()) {
                for (usize i = 0; i < sharedCycles.size(); ++i) {
                    auto motion = sharedCycles[i];
                    if (static_cast<u32>(motion.texture) != nativeSlot ||
                        !convertedCycles.emplace(i, part.texture).second)
                        continue;
                    motion.texture = static_cast<s32>(part.texture);
                    motion.source = static_cast<s32>(convert(
                        static_cast<u32>(motion.source), static_cast<u32>(motion.frames), ramp));
                    motion.name += ":RDP";
                    require(animation.textureAnimations.size() < 32767,
                            "animation budget exceeded");
                    animation.textureAnimations.push_back(std::move(motion));
                }
            }
            std::map<u32, u32> copies;
            for (auto& index : part.indices) {
                require(index < model.mesh.vertices.size(), "vertex is outside its model");
                const auto [copy, inserted] =
                    copies.try_emplace(index, static_cast<u32>(model.mesh.vertices.size()));
                if (inserted) {
                    constexpr usize kCharge = sizeof(MeshVertex) + sizeof(u32);
                    require(remainingBytes >= kCharge, "geometry variant byte budget exceeded");
                    remainingBytes -= kCharge;
                    auto vertex = model.mesh.vertices[index];
                    if (!ramp.shadeRgb)
                        vertex.color = Color::white();
                    else
                        vertex.color.a = 255; // Cycle1 modulates only RGB, never SHADE alpha.
                    model.mesh.vertices.push_back(vertex);
                    if (!model.vertexSources.empty()) {
                        require(index < model.vertexSources.size(),
                                "source vertex identity is unavailable");
                        const u32 source = model.vertexSources[index];
                        model.vertexSources.push_back(source);
                    }
                }
                index = copy->second;
            }
        }
        model.colorRamps.clear();
    }
}

std::vector<LegendsN64TextureScroll> decodeLegendsN64TextureScrolls(std::span<const u8> objects) {
    const auto scrolls = findSection(objects, "TSCR", false);
    if (scrolls.count == 0) {
        return {};
    }
    const auto headers = findSection(objects, "TXHD");
    const auto loaders = findSection(objects, "TXLD");
    if (scrolls.count > kMaxTextures || headers.count > kMaxTextures) {
        throw FormatError("N64 texture scroll count exceeds archive limit");
    }
    checkSection(scrolls, scrolls.count * 20);
    checkSection(headers, headers.count * kRecordSize);
    checkSection(loaders, loaders.count);
    std::vector<LegendsN64TextureScroll> result;
    result.reserve(scrolls.count);
    for (usize i = 0; i < scrolls.count; ++i) {
        const auto at = scrolls.offset + i * 20;
        LegendsN64TextureScroll scroll;
        scroll.commandOffset = read32(objects, at);
        scroll.texelsPerTick = {std::bit_cast<f32>(read32(objects, at + 4)),
                                std::bit_cast<f32>(read32(objects, at + 8))};
        scroll.texture = read16(objects, at + 12);
        scroll.initialPhase = {std::bit_cast<s16>(read16(objects, at + 14)),
                               std::bit_cast<s16>(read16(objects, at + 16))};
        scroll.reserved = read16(objects, at + 18);
        if (scroll.texture >= headers.count) {
            throw FormatError("N64 texture scroll targets a missing texture");
        }
        const auto texture = headers.offset + scroll.texture * kRecordSize;
        scroll.width = read16(objects, texture + 12);
        scroll.height = read16(objects, texture + 14);
        // The native signed phase has only sixteen bits; larger tile periods
        // cannot be advanced by its add/subtract loops without wrapping again.
        if (scroll.width == 0 || scroll.height == 0 || scroll.width > 1024 ||
            scroll.height > 1024 || !std::isfinite(scroll.texelsPerTick.x) ||
            !std::isfinite(scroll.texelsPerTick.y) ||
            std::abs(scroll.texelsPerTick.x) > static_cast<f32>(scroll.width) ||
            std::abs(scroll.texelsPerTick.y) > static_cast<f32>(scroll.height)) {
            throw FormatError("Invalid N64 texture scroll dimensions or speed");
        }
        if (loaders.count < 8 || scroll.commandOffset < loaders.offset ||
            scroll.commandOffset - loaders.offset > loaders.count - 8 ||
            (scroll.commandOffset & 7U) != 0 ||
            (read32(objects, scroll.commandOffset) >> 24U) != 0xF2U ||
            (read32(objects, scroll.commandOffset + 4) & 0xFF000000U) != 0) {
            throw FormatError("N64 texture scroll does not reference a render-tile command");
        }
        result.push_back(scroll);
    }
    return result;
}

void LegendsN64TextureScrollState::advance(const LegendsN64TextureScroll& scroll, f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0f || seconds > 60.0f || scroll.width == 0 ||
        scroll.height == 0 || scroll.width > 1024 || scroll.height > 1024 ||
        !std::isfinite(scroll.texelsPerTick.x) || !std::isfinite(scroll.texelsPerTick.y) ||
        std::abs(scroll.texelsPerTick.x) > static_cast<f32>(scroll.width) ||
        std::abs(scroll.texelsPerTick.y) > static_cast<f32>(scroll.height)) {
        return;
    }
    m_remainder += static_cast<f64>(seconds) * 30.0;
    const auto ticks = static_cast<u32>(std::floor(m_remainder + 0.00001));
    m_remainder = std::max(0.0, m_remainder - ticks);
    const std::array<u16, 2> dimensions{scroll.width, scroll.height};
    for (u32 tick = 0; tick < ticks; ++tick) {
        for (usize axis = 0; axis < m_phase.size(); ++axis) {
            const f32 rate = scroll.texelsPerTick[static_cast<int>(axis)];
            if (rate == 0.0f) {
                continue;
            }
            // US game.bin e0025d7c: truncate after adding to the signed phase,
            // then wrap negatives by dimension*32. Its upper comparison really
            // is dimension (e0025e70/e0025f0c), not dimension*32. Keep this order:
            // truncating a negative sum differs from truncating the rate alone.
            s32 phase =
                static_cast<s16>(static_cast<s32>(static_cast<f32>(m_phase[axis]) + rate * 32.0f));
            const s32 period = dimensions[axis] * 32;
            while (phase < 0) {
                phase += period;
            }
            while (phase > dimensions[axis]) {
                phase -= period;
            }
            m_phase[axis] = static_cast<s16>(phase);
        }
    }
}

Vec2 LegendsN64TextureScrollState::offset(const LegendsN64TextureScroll& scroll) const {
    if (scroll.width == 0 || scroll.height == 0) {
        return Vec2{0.0f};
    }
    // e0025f54/e0025f5c arithmetic-shift the signed 1/32 phase by three
    // before writing the RDP's quarter-texel upper-left tile coordinates.
    return {-std::floor(static_cast<f32>(m_phase[0]) / 8.0f) /
                (4.0f * static_cast<f32>(scroll.width)),
            -std::floor(static_cast<f32>(m_phase[1]) / 8.0f) /
                (4.0f * static_cast<f32>(scroll.height))};
}

} // namespace gdl::formats
