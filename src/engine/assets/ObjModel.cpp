#include "engine/assets/ObjModel.h"

#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Error.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

std::vector<std::string_view> splitWords(std::string_view line) {
    std::vector<std::string_view> words;
    usize at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
            ++at;
        }
        const usize start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') {
            ++at;
        }
        if (at > start) {
            words.push_back(line.substr(start, at - start));
        }
    }
    return words;
}

f32 parseFloat(std::string_view text, usize lineNumber) {
    f32 value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw FormatError(std::format("OBJ line {}: '{}' is not a number", lineNumber, text));
    }
    return value;
}

/** A 1-based OBJ index (negative counts from the end), converted to 0-based; -1 when absent. */
s64 parseIndex(std::string_view text, usize count, usize lineNumber) {
    if (text.empty()) {
        return -1;
    }
    s64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        throw FormatError(std::format("OBJ line {}: bad index '{}'", lineNumber, text));
    }
    const s64 zeroBased = value > 0 ? value - 1 : static_cast<s64>(count) + value;
    if (zeroBased < 0 || zeroBased >= static_cast<s64>(count)) {
        throw FormatError(std::format("OBJ line {}: index {} out of range", lineNumber, value));
    }
    return zeroBased;
}

struct Corner {
    s64 position = -1;
    s64 texcoord = -1;
    s64 normal = -1;
    auto operator<=>(const Corner&) const = default;
};

} // namespace

/** The number a material name carries after `tex` or `_lm`, or nothing when it is not one. */
std::optional<u32> materialIndex(std::string_view digits) {
    u32 value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

Mesh parseObj(std::string_view text) {
    std::vector<Vec3> positions;
    std::vector<Color> colors; // one per position when the file carries the extension
    std::vector<Vec2> texcoords;
    std::vector<Vec2> lightmapCoords;
    std::vector<Vec3> normals;
    Mesh mesh;
    MeshPart part;
    bool partOpen = false;
    std::map<Corner, u32> corners;

    const auto vertexFor = [&](const Corner& corner) {
        const auto found = corners.find(corner);
        if (found != corners.end()) {
            return found->second;
        }
        MeshVertex v;
        v.position = positions[static_cast<usize>(corner.position)];
        if (static_cast<usize>(corner.position) < colors.size()) {
            v.color = colors[static_cast<usize>(corner.position)];
        }
        if (corner.texcoord >= 0) {
            v.uv = texcoords[static_cast<usize>(corner.texcoord)];
            if (static_cast<usize>(corner.texcoord) < lightmapCoords.size()) {
                v.lightmapUv = lightmapCoords[static_cast<usize>(corner.texcoord)];
            }
        }
        if (corner.normal >= 0) {
            v.normal = normals[static_cast<usize>(corner.normal)];
        }
        const auto index = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back(v);
        corners.emplace(corner, index);
        return index;
    };
    const auto closePart = [&]() {
        if (partOpen && !part.indices.empty()) {
            mesh.parts.push_back(std::move(part));
        }
        part = MeshPart{};
        partOpen = false;
    };

    usize lineNumber = 0;
    usize at = 0;
    while (at <= text.size()) {
        const usize lineEnd = text.find('\n', at);
        const std::string_view line =
            text.substr(at, (lineEnd == std::string_view::npos ? text.size() : lineEnd) - at);
        at = lineEnd == std::string_view::npos ? text.size() + 1 : lineEnd + 1;
        ++lineNumber;
        const std::vector<std::string_view> words = splitWords(line);
        if (words.empty() || words[0][0] == '#') {
            continue;
        }
        const std::string_view key = words[0];
        if (key == "v" && words.size() >= 4) {
            positions.emplace_back(parseFloat(words[1], lineNumber),
                                   parseFloat(words[2], lineNumber),
                                   parseFloat(words[3], lineNumber));
            if (words.size() >= 7) {
                colors.push_back(Color::fromFloats(parseFloat(words[4], lineNumber),
                                                   parseFloat(words[5], lineNumber),
                                                   parseFloat(words[6], lineNumber)));
            }
        } else if (key == "vt" && words.size() >= 3) {
            texcoords.emplace_back(parseFloat(words[1], lineNumber),
                                   1.0f - parseFloat(words[2], lineNumber));
        } else if (key == "vl" && words.size() >= 3) {
            lightmapCoords.emplace_back(parseFloat(words[1], lineNumber),
                                        parseFloat(words[2], lineNumber));
        } else if (key == "vn" && words.size() >= 4) {
            normals.emplace_back(parseFloat(words[1], lineNumber), parseFloat(words[2], lineNumber),
                                 parseFloat(words[3], lineNumber));
        } else if (key == "usemtl" && words.size() >= 2) {
            closePart();
            partOpen = true;
            const std::string_view material = words[1];
            if (material.starts_with("tex")) {
                const std::string_view rest = material.substr(3);
                const usize lightmapAt = rest.find("_lm");
                part.texture = materialIndex(rest.substr(0, lightmapAt)).value_or(0);
                if (lightmapAt != std::string_view::npos) {
                    part.lightmap = materialIndex(rest.substr(lightmapAt + 3)).value_or(0);
                }
            }
        } else if (key == "f" && words.size() >= 4) {
            if (!partOpen) {
                partOpen = true;
            }
            std::vector<u32> polygon;
            for (usize w = 1; w < words.size(); ++w) {
                const std::string_view corner = words[w];
                const usize firstSlash = corner.find('/');
                const usize secondSlash = firstSlash == std::string_view::npos
                                              ? std::string_view::npos
                                              : corner.find('/', firstSlash + 1);
                Corner c;
                c.position = parseIndex(corner.substr(0, firstSlash), positions.size(), lineNumber);
                if (firstSlash != std::string_view::npos) {
                    const usize texEnd =
                        secondSlash == std::string_view::npos ? corner.size() : secondSlash;
                    c.texcoord = parseIndex(corner.substr(firstSlash + 1, texEnd - firstSlash - 1),
                                            texcoords.size(), lineNumber);
                }
                if (secondSlash != std::string_view::npos) {
                    c.normal =
                        parseIndex(corner.substr(secondSlash + 1), normals.size(), lineNumber);
                }
                polygon.push_back(vertexFor(c));
            }
            for (usize k = 1; k + 1 < polygon.size(); ++k) {
                part.indices.insert(part.indices.end(), {polygon[0], polygon[k], polygon[k + 1]});
            }
        }
    }
    closePart();
    mesh.prelit = !colors.empty();
    return mesh;
}

Mesh loadObj(const std::filesystem::path& path) {
    const std::vector<u8> bytes = readFile(path);
    return parseObj(std::string(bytes.begin(), bytes.end()));
}

} // namespace gdl
