#include "expansions/n64/formats/LegendsN64Scene.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>

#include "engine/assets/ObjectFlags.h"
#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/world/WorldCamera.h"

#include "expansions/n64/formats/LegendsN64Morph.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace gdl::formats {
namespace {

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw FormatError("N64 Legends scene: " + std::string(reason));
    }
}

void range(std::span<const u8> bytes, usize offset, usize count) {
    require(offset <= bytes.size() && count <= bytes.size() - offset, "truncated record");
}

std::string fourcc(std::span<const u8> bytes, usize offset) {
    range(bytes, offset, 4);
    if (readU32BE(bytes, offset) == 0) {
        return {};
    }
    usize begin = offset;
    while (begin < offset + 4 && bytes[begin] == 0) {
        ++begin;
    }
    return {std::bit_cast<const char*>(bytes.data() + begin), offset + 4 - begin};
}

s16 signed16(std::span<const u8> bytes, usize at) {
    return static_cast<s16>(readU16BE(bytes, at));
}

struct Section {
    usize offset;
    usize count;
    usize end;
};

class SequenceWad {
public:
    explicit SequenceWad(std::span<const u8> bytes) : m_bytes(bytes) {
        range(bytes, 0, 8);
        const usize directory = readU32BE(bytes, 0);
        const usize count = readU32BE(bytes, 4);
        require(directory >= 8 && count <= 64, "invalid sequence directory");
        range(bytes, directory, count * 12);
        for (usize i = 0; i < count; ++i) {
            const usize at = directory + i * 12;
            const std::string tag(std::bit_cast<const char*>(bytes.data() + at), 4);
            const usize offset = readU32BE(bytes, at + 4);
            require(offset >= 8 && offset <= directory, "invalid sequence section offset");
            require(m_sections.emplace(tag, Section{offset, readU32BE(bytes, at + 8), directory})
                        .second,
                    "duplicate sequence section");
        }
        for (auto& [tag, section] : m_sections) {
            for (const auto& [otherTag, other] : m_sections) {
                if (other.offset > section.offset) {
                    section.end = std::min(section.end, other.offset);
                }
            }
        }
    }

    const Section& section(std::string_view tag, usize stride) const {
        const auto found = m_sections.find(std::string(tag));
        require(found != m_sections.end(), "missing sequence section " + std::string(tag));
        require(found->second.count <= (found->second.end - found->second.offset) / stride,
                "invalid sequence section size");
        return found->second;
    }

    usize metadataCount(std::string_view tag) const {
        const auto found = m_sections.find(std::string(tag));
        return found == m_sections.end() ? 0 : found->second.count;
    }

private:
    std::span<const u8> m_bytes;
    std::map<std::string, Section> m_sections;
};

struct Family {
    LegendsN64FixtureKind kind;
    const char* group;
    usize pointer;
    usize count;
    usize stride;
    usize templateStride;
    usize key;
    usize minimumPlayers;
    usize model;
    usize active;
    usize death;
    std::span<const u8> templates;
};

struct TransformKeys {
    u8 flags = 0;
    std::vector<u16> frames{0};
    std::vector<Vec3> values;
};

std::string powerupArchive(u16 key, u32 flags) {
    // e0064e60 loads these only when the pickup's sequence is not already loaded.
    // The first template byte is the kind; the second is its native subtype.
    switch (key >> 8U) {
    case 0x15: return "items/s/r" + std::to_string(key & 0xffU);
    case 0x27:
        switch (flags & 15U) {
        case 1: return "items/s/cmin";
        case 2: return "items/s/cfal";
        case 3: return "items/s/cjac";
        case 4: return "items/s/ctig";
        case 5: return "items/s/csum";
        default: return {};
        }
    case 0x29:
    case 0x2a:
        switch (flags & 0x3f0U) {
        case 0x1c0: return "items/s/li";
        case 0x1d0: return "items/s/lf";
        case 0x1e0: return "items/s/ls";
        case 0x1f0: return "items/s/lj";
        case 0x200: return "items/s/lss";
        case 0x220: return "items/s/bossm";
        case 0x230: return "items/s/bossh";
        case 0x240: return "items/s/bossgr";
        case 0x250: return "items/s/bossgl";
        default: return {};
        }
    case 0x2b:
        switch (key & 0xffU) {
        case 1: return "items/s/sm";
        case 2: return "items/s/si";
        case 3: return "items/s/sc";
        case 4: return "items/s/st";
        default: return {};
        }
    default: return {};
    }
}

class InitialPowerupSelectors {
public:
    explicit InitialPowerupSelectors(std::span<const u8> bytes) {
        if (bytes.empty()) {
            return;
        }
        range(bytes, 0, 4);
        const usize count = readU32BE(bytes, 0);
        require(count <= 4096, "selector table budget exceeded");
        range(bytes, 4, count * 8);
        for (usize row = 0; row < count;) {
            const u32 name = readU32BE(bytes, 4 + row * 8);
            if (name == 0x454e4420U) { // END
                break;
            }
            require(name != 0 && name != 0x53544f50U, "selector lacks a group name");
            std::vector<u16> keys;
            bool stopped = false;
            for (; row < count; ++row) {
                const usize at = 4 + row * 8;
                const u32 tag = readU32BE(bytes, at);
                if (tag == 0x53544f50U) { // STOP
                    ++row;
                    stopped = true;
                    break;
                }
                require(keys.empty() ? tag == name : tag == 0, "unterminated selector group");
                const u16 kind = readU16BE(bytes, at + 4);
                const u16 subtype = readU16BE(bytes, at + 6);
                require(kind <= 255 && subtype <= 255, "selector key exceeds native bytes");
                keys.push_back(static_cast<u16>((kind << 8U) | subtype));
            }
            require(stopped && !keys.empty(), "selector lacks STOP or entries");
            require(m_groups.emplace(name, std::move(keys)).second, "duplicate selector group");
        }
    }

    usize resolve(std::span<const u8> templates, usize selected) {
        const usize count = readU32BE(templates, 0);
        std::set<usize> visited;
        while (templates[selected] == 0x22) {
            require(visited.size() < 32 && visited.insert(selected).second,
                    "cyclic or excessive nested pickup selectors");
            const auto group = m_groups.find(readU32BE(templates, selected + 8));
            require(group != m_groups.end(), "pickup selector group is absent");
            auto [cursor, inserted] =
                m_cursors.try_emplace(selected, signed16(templates, selected + 2));
            static_cast<void>(inserted);
            // e006d7c0 increments the template's cursor before selecting, wraps
            // at STOP, and recursively resolves nested kind22 selectors.
            s32 next = cursor->second + 1;
            require(next >= 0, "negative pickup selector cursor");
            if (static_cast<usize>(next) >= group->second.size()) {
                next = 0;
            }
            cursor->second = next;
            const u16 key = group->second[static_cast<usize>(next)];
            usize found = templates.size();
            for (usize i = 0; i < count; ++i) {
                const usize at = 4 + i * 28;
                if (readU16BE(templates, at) == key) {
                    require(found == templates.size(), "ambiguous selected pickup template");
                    found = at;
                }
            }
            require(found != templates.size(), "selected pickup template is absent");
            selected = found;
        }
        return selected;
    }

private:
    std::map<u32, std::vector<u16>> m_groups;
    std::map<usize, s32> m_cursors;
};

// e0030838's three encodings share an initial fixed-point XYZ and byte deltas
// through the executable's exact tables. Keep those native tables as input,
// rather than embedding game data or approximating them with a cubic curve.
std::optional<TransformKeys> transformKeys(std::span<const u8> bytes, usize payload, usize end,
                                           u8 property, u16 sequenceFrames,
                                           std::span<const u8> program) {
    require(payload <= end && end - payload >= 6, "truncated transform initial value");
    const bool rotation = (property & 31U) == 9;
    const f32 unit = rotation ? std::numbers::pi_v<f32> / 16384.0f : 1.0f / 64.0f;
    TransformKeys result;
    Vec3 value{signed16(bytes, payload), signed16(bytes, payload + 2),
               signed16(bytes, payload + 4)};
    value *= unit;
    result.values.push_back(value);
    const u8 mode = property & 0xc0U;
    if (mode == 0x40) {
        return result;
    }
    require(end - payload >= 7, "truncated transform header");
    usize frameCount = sequenceFrames;
    usize cursor = payload + 7;
    result.flags = bytes[payload + 6];
    if ((result.flags & 0x80U) != 0) {
        require(end - payload >= 9, "truncated local transform clock");
        frameCount = readU16BE(bytes, payload + 6) & 0x7fffU;
        result.flags = bytes[payload + 8];
        cursor += 2;
    }
    require(frameCount > 0 && frameCount <= 4096, "invalid transform frame count");
    if (program.empty()) {
        return std::nullopt;
    }
    const usize table = rotation ? 0x32b90 : 0x32f98; // US load base E0000400.
    range(program, table, usize{256} * 4);
    std::array<f32, 256> deltas{};
    for (usize i = 0; i < deltas.size(); ++i) {
        deltas[i] = std::bit_cast<f32>(readU32BE(program, table + i * 4));
        require(std::isfinite(deltas[i]) && std::abs(deltas[i]) <= 16.0f,
                "invalid native transform delta table");
    }
    const auto append = [&](usize frame) {
        for (usize axis = 0; axis < 3; ++axis) {
            if ((result.flags & (16U >> axis)) != 0) {
                require(cursor < end, "truncated transform delta");
                value[static_cast<int>(axis)] += deltas[bytes[cursor++]];
            }
        }
        result.frames.push_back(static_cast<u16>(frame));
        result.values.push_back(value);
    };
    if (mode == 0) {
        for (usize frame = 1; frame < frameCount; ++frame) {
            append(frame);
        }
    } else if (mode == 0x80) {
        usize frame = 0;
        while (frame + 1 < frameCount) {
            require(cursor < end, "truncated transform key interval");
            const usize interval = bytes[cursor++];
            require(interval != 0 && interval < frameCount - frame,
                    "invalid transform key interval");
            frame += interval;
            append(frame);
        }
    } else {
        const usize mask = cursor;
        const usize length = (frameCount + 7) / 8;
        require(length <= end - cursor, "truncated transform key bitmap");
        cursor += length;
        for (usize frame = 1; frame < frameCount; ++frame) {
            if ((bytes[mask + frame / 8] & (0x80U >> (frame % 8))) != 0) {
                append(frame);
            }
        }
        if (result.frames.back() + 1U < frameCount) {
            // e00314dc-e0031528 scans for the next set bit even beyond the
            // nominal bitmap. Several retail boss tracks end one key early;
            // their lookahead consequently reads padding or the first delta
            // bytes. Preserve that bounded native result, not a guessed hold.
            // At the last clock frame e003114c blends toward the initial value
            // using wrapped angles (also for translation/scale), whereas the
            // preceding tail frames use the next delta as usual.
            usize nextFrame = frameCount;
            for (; nextFrame < frameCount + 4096; ++nextFrame) {
                require(nextFrame / 8 < end - mask, "truncated transform key lookahead");
                if ((bytes[mask + nextFrame / 8] & (0x80U >> (nextFrame % 8))) != 0) {
                    break;
                }
            }
            require(nextFrame < frameCount + 4096, "transform key lookahead budget exceeded");
            const usize previousFrame = result.frames.back();
            const Vec3 previousValue = value;
            Vec3 delta{0.0f};
            if (previousFrame + 2 < frameCount) {
                for (usize axis = 0; axis < 3; ++axis) {
                    if ((result.flags & (16U >> axis)) != 0) {
                        require(cursor < end, "truncated transform tail delta");
                        delta[static_cast<int>(axis)] = deltas[bytes[cursor++]];
                    }
                }
            }
            for (usize frame = previousFrame + 1; frame < frameCount; ++frame) {
                Vec3 difference = delta;
                if (frame + 1 == frameCount) {
                    difference = result.values.front() - previousValue;
                    for (usize axis = 0; axis < 3; ++axis) {
                        auto& component = difference[static_cast<int>(axis)];
                        while (component > std::numbers::pi_v<f32>) {
                            component -= 2.0f * std::numbers::pi_v<f32>;
                        }
                        while (component < -std::numbers::pi_v<f32>) {
                            component += 2.0f * std::numbers::pi_v<f32>;
                        }
                        if ((result.flags & (16U >> axis)) == 0) {
                            component = 0;
                        }
                    }
                }
                const f32 fraction = static_cast<f32>(frame - previousFrame) /
                                     static_cast<f32>(nextFrame - previousFrame);
                result.frames.push_back(static_cast<u16>(frame));
                result.values.push_back(previousValue + difference * fraction);
            }
        }
    }
    return result;
}

Vec3 sampleTransform(const TransformKeys& keys, u16 frame) {
    const auto next = std::lower_bound(keys.frames.begin(), keys.frames.end(), frame);
    if (next == keys.frames.end()) {
        return keys.values.back();
    }
    const usize index = static_cast<usize>(next - keys.frames.begin());
    if (index == 0 || *next == frame) {
        return keys.values[index];
    }
    const f32 t = static_cast<f32>(frame - keys.frames[index - 1]) /
                  static_cast<f32>(*next - keys.frames[index - 1]);
    return keys.values[index - 1] + (keys.values[index] - keys.values[index - 1]) * t;
}

} // namespace

std::vector<LegendsN64Fixture> decodeLegendsN64Fixtures(std::span<const u8> bytes,
                                                        const LegendsN64Templates& templates,
                                                        u8 activationThreshold) {
    range(bytes, 0, 92);
    // US initializers e0064e60, e0067818, e00694e4, e006d080 and e006d9f0.
    const std::array families{
        Family{LegendsN64FixtureKind::Powerup, "POWERUP", 0, 36, 12, 28, 6, 8, 8, 16, 0,
               templates.powerups},
        Family{LegendsN64FixtureKind::Decoration, "DECORATION", 4, 38, 10, 24, 6, 8, 8, 0, 0,
               templates.decorations},
        Family{LegendsN64FixtureKind::Generator, "GENERATOR", 8, 40, 16, 56, 8, 14, 12, 16, 20,
               templates.generators},
        Family{LegendsN64FixtureKind::Trap, "TRAP", 12, 42, 24, 96, 8, 11, 12, 16, 24,
               templates.traps},
        Family{LegendsN64FixtureKind::Teleport, "TELEPORT", 16, 46, 16, 40, 8, 11, 8, 12, 0,
               templates.teleports},
        Family{LegendsN64FixtureKind::Container, "CONTAINER", 24, 50, 16, 44, 8, 11, 8, 12, 16,
               templates.containers},
    };
    std::vector<LegendsN64Fixture> result;
    InitialPowerupSelectors selectors(templates.randoms);
    for (const auto& family : families) {
        const usize count = readU16BE(bytes, family.count);
        if (count == 0) {
            continue;
        }
        const usize offset = readU32BE(bytes, family.pointer);
        require(offset >= 92 && count <= 16384 && result.size() + count <= 32768,
                "invalid placement table");
        range(bytes, offset, count * family.stride);
        range(family.templates, 0, 4);
        const usize templateCount = readU32BE(family.templates, 0);
        require(templateCount <= 4096, "template budget exceeded");
        range(family.templates, 4, templateCount * family.templateStride);
        for (usize i = 0; i < count; ++i) {
            const usize at = offset + i * family.stride;
            const u16 key = readU16BE(bytes, at + family.key);
            usize selected = family.templates.size();
            for (usize j = 0; j < templateCount; ++j) {
                const usize candidate = 4 + j * family.templateStride;
                if (readU16BE(family.templates, candidate) == key &&
                    (family.kind != LegendsN64FixtureKind::Generator ||
                     family.templates[candidate + 10] == bytes[at + 10])) {
                    require(selected == family.templates.size(), "ambiguous fixture template");
                    selected = candidate;
                }
            }
            require(selected != family.templates.size(), "unknown fixture template");
            LegendsN64Fixture item;
            item.kind = family.kind;
            item.nativeIndex = static_cast<u32>(i);
            item.templateKey = key;
            item.minimumPlayers = bytes[at + family.minimumPlayers];
            item.templateFlags = readU32BE(family.templates, selected + 4);
            usize visual = selected;
            if (family.kind == LegendsN64FixtureKind::Powerup && (key >> 8U) == 0x22 &&
                !templates.randoms.empty() && item.minimumPlayers <= activationThreshold) {
                visual = selectors.resolve(family.templates, selected);
            }
            item.resolvedTemplateKey = readU16BE(family.templates, visual);
            item.resolvedTemplateFlags = readU32BE(family.templates, visual + 4);
            item.position =
                Vec3{signed16(bytes, at), signed16(bytes, at + 2), signed16(bytes, at + 4)} / 4.0f;
            item.group = family.group;
            item.sequence = fourcc(family.templates, visual + family.model);
            if (family.active != 0) {
                item.activeSequence = fourcc(family.templates, visual + family.active);
            }
            if (family.death != 0) {
                item.deathSequence = fourcc(family.templates, selected + family.death);
            }
            if (family.key == 8) {
                // e002fcf8 normalizes this directly against +/-pi: these are radians.
                item.facingRadians = static_cast<f32>(signed16(bytes, at + 6)) / 32.0f;
                item.placementFlags = bytes[at + 10];
            }
            switch (family.kind) {
            case LegendsN64FixtureKind::Powerup:
                item.archive = powerupArchive(item.resolvedTemplateKey,
                                              readU32BE(family.templates, visual + 4));
                item.quantity = static_cast<s8>(bytes[at + 9]);
                // e0065618 reads amount/cap/cue at +2/+20/+24. Timed grants
                // multiply by150 in e0054fdc and tick by dt*150 in e00496d4.
                item.templateAmount = signed16(family.templates, visual + 2);
                item.pickupLimit = readU32BE(family.templates, visual + 20);
                item.pickupSound = readU32BE(family.templates, visual + 24);
                // e0064e60 builds the native pickup cylinder in 1/32 units.
                item.pickupRadius =
                    (item.resolvedTemplateKey >> 8U) == 0x21 ? 51.0f / 32.0f : 16.0f / 32.0f;
                item.pickupHeight = 96.0f / 32.0f;
                break;
            case LegendsN64FixtureKind::Generator:
                item.generatorTier = bytes[at + 10];
                item.placementFlags = bytes[at + 13];
                item.actorInsteadOfGenerator = (item.placementFlags & 2U) != 0;
                break;
            case LegendsN64FixtureKind::Trap:
                item.scale = std::bit_cast<f32>(readU32BE(bytes, at + 16));
                require(std::isfinite(item.scale), "non-finite trap scale");
                item.scale = item.scale > 0.001f ? item.scale : 1.0f;
                item.groundProjected = (readU32BE(family.templates, selected) & 0x20U) == 0;
                break;
            case LegendsN64FixtureKind::Container:
                item.containedPowerup = readU16BE(bytes, at + 12);
                item.quantity = static_cast<s8>(bytes[at + 14]);
                break;
            case LegendsN64FixtureKind::Decoration:
            case LegendsN64FixtureKind::Teleport: break;
            }
            result.push_back(std::move(item));
        }
    }
    return result;
}

std::string legendsN64TreeName(std::string_view group, std::string_view sequence) {
    return std::string(group) + ':' + std::string(sequence);
}

LegendsN64FixturePlacement
legendsN64FixturePlacement(const LegendsN64Fixture& fixture,
                           std::span<const LegendsMapCollisionTriangle> collision) {
    if (!fixture.groundProjected) {
        return {fixture.position, -1};
    }
    const auto finite = [](Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    require(finite(fixture.position), "non-finite fixture position");
    struct Floor {
        f32 y;
        s32 owner;
    };
    std::vector<Floor> floors;
    floors.reserve(16);
    const Vec2 point{fixture.position.x, fixture.position.z};
    for (const auto& triangle : collision) {
        if (!triangle.floor) {
            continue;
        }
        const auto& vertices = triangle.vertices;
        require(finite(vertices[0]) && finite(vertices[1]) && finite(vertices[2]),
                "non-finite fixture floor");
        const Vec2 a{vertices[0].x, vertices[0].z};
        const Vec2 b{vertices[1].x, vertices[1].z};
        const Vec2 c{vertices[2].x, vertices[2].z};
        const Vec2 ab = b - a;
        const Vec2 ac = c - a;
        const Vec2 ap = point - a;
        const f32 determinant = ab.x * ac.y - ab.y * ac.x;
        if (std::abs(determinant) < 1e-7f) {
            continue;
        }
        const f32 u = (ap.x * ac.y - ap.y * ac.x) / determinant;
        const f32 v = (ab.x * ap.y - ab.y * ap.x) / determinant;
        if (u >= -1e-5f && v >= -1e-5f && u + v <= 1.00001f) {
            floors.push_back({vertices[0].y + u * (vertices[1].y - vertices[0].y) +
                                  v * (vertices[2].y - vertices[0].y),
                              triangle.owner});
        } else {
            // The native floor query has a one-unit footprint. Only the closest
            // point on an actual triangle edge participates outside its face.
            f32 nearestSquared = 1.0f;
            std::optional<f32> edgeHeight;
            for (usize edge = 0; edge < 3; ++edge) {
                const Vec3 from = vertices[edge];
                const Vec3 to = vertices[(edge + 1) % 3];
                const Vec2 start{from.x, from.z};
                const Vec2 direction{to.x - from.x, to.z - from.z};
                const f32 lengthSquared = glm::dot(direction, direction);
                if (lengthSquared < 1e-8f) {
                    continue;
                }
                const f32 along =
                    std::clamp(glm::dot(point - start, direction) / lengthSquared, 0.0f, 1.0f);
                const Vec2 delta = point - (start + along * direction);
                const f32 distanceSquared = glm::dot(delta, delta);
                if (distanceSquared <= nearestSquared) {
                    nearestSquared = distanceSquared;
                    edgeHeight = from.y + along * (to.y - from.y);
                }
            }
            if (edgeHeight) {
                floors.push_back({*edgeHeight, triangle.owner});
            }
        }
    }
    const auto probe = [&](f32 y, f32 up, f32 down) -> std::optional<Floor> {
        std::optional<Floor> height;
        for (const auto floor : floors) {
            if (floor.y <= y + up && floor.y >= y - down - 1.0f &&
                (!height || floor.y > height->y)) {
                height = floor;
            }
        }
        return height;
    };
    const bool generator = fixture.kind == LegendsN64FixtureKind::Generator;
    auto height = probe(fixture.position.y, generator ? 3.0f : 1.5f, generator ? 10.0f : 15.0f);
    if (!height && !generator) {
        // e0064714 retries at authoredY + [0,29]; it does not snap every fixture
        // to the highest floor in the level or to the visible geometry.
        for (int retry = 0; retry < 30 && !height; ++retry) {
            height = probe(fixture.position.y + static_cast<f32>(retry), 1.0f, 15.0f);
        }
    }
    Vec3 position = fixture.position;
    if (height) {
        position.y = height->y + 0.2f; // e003fee0's shared contact pad.
    }
    return {position, height ? height->owner : -1};
}

Vec3 legendsN64FixturePosition(const LegendsN64Fixture& fixture,
                               std::span<const LegendsMapCollisionTriangle> collision) {
    return legendsN64FixturePlacement(fixture, collision).position;
}

LegendsN64Sequences decodeLegendsN64Sequences(std::span<const u8> bytes,
                                              std::span<const LegendsN64Model> models,
                                              std::span<const u8> program,
                                              std::span<const u8> dynamicSequences,
                                              bool bakeSequenceTextures) {
    const SequenceWad wad(bytes);
    const Section& body = wad.section("BODY", 1);
    const Section& sequences = wad.section("SEQS", 1);
    const Section& lists = wad.section("SLST", 8);
    const Section& groups = wad.section("GRPS", 24);
    require(body.count % 8 == 0 && body.count <= 1048576 && groups.count <= 256 &&
                lists.count <= 4096 && models.size() <= 16384,
            "sequence budget exceeded");
    LegendsN64Sequences result;
    usize unsupportedNodes = 0;
    usize unsupportedFacing = 0;
    usize unsupportedTracks = 0;
    usize unsupportedRuntimeTextures = 0;
    usize externalSequences = 0;
    usize remainingNodes = 65536;
    usize remainingValues = usize{4} * 1024 * 1024;
    usize remainingVariantVertices = usize{1024} * 1024;
    usize remainingVariantBytes = kLegendsN64VariantByteLimit;
    std::set<std::string> treeNames;
    std::set<std::string> sharedTextureSequences;
    std::set<usize> dynamicOffsets;
    for (usize i = 0; i < lists.count; ++i) {
        const u32 address = readU32BE(bytes, lists.offset + i * 8 + 4);
        if ((address & 0xf0000000U) == 0x10000000U) {
            dynamicOffsets.insert(address & 0x03ffffffU);
        }
    }
    const usize dynamicBufferSize = wad.metadataCount("DSIZ");
    if (!dynamicSequences.empty() && !dynamicOffsets.empty()) {
        require(dynamicSequences.size() <= usize{64} * 1024 * 1024 && dynamicBufferSize >= 8 &&
                    dynamicBufferSize <= 1048576,
                "invalid dynamic sequence buffer extent");
    }
    for (usize groupIndex = 0; groupIndex < groups.count; ++groupIndex) {
        const usize groupAt = groups.offset + groupIndex * 24;
        usize nameLength = 0;
        while (nameLength < 16 && bytes[groupAt + nameLength] != 0) {
            ++nameLength;
        }
        require(nameLength != 0 && nameLength < 16, "invalid sequence group name");
        const std::string group(std::bit_cast<const char*>(bytes.data() + groupAt), nameLength);
        const usize count = readU32BE(bytes, groupAt + 16);
        const usize first = readU32BE(bytes, groupAt + 20);
        require(first >= lists.offset && first <= lists.offset + lists.count * 8 &&
                    (first - lists.offset) % 8 == 0 &&
                    count <= (lists.offset + lists.count * 8 - first) / 8,
                "group sequence list outside SLST");
        for (usize sequenceIndex = 0; sequenceIndex < count; ++sequenceIndex) {
            const usize listAt = first + sequenceIndex * 8;
            const usize address = readU32BE(bytes, listAt + 4);
            const bool external = (address & 0xf0000000U) == 0x10000000U;
            if (external && dynamicSequences.empty()) {
                ++externalSequences;
                continue;
            }
            // e002d230 relocates high-nibble1 entries against dynseq.rom's base;
            // e002db0c reads that stream into the DSIZ-sized native scratch buffer.
            const auto sequenceBytes = external ? dynamicSequences : bytes;
            const usize at = external ? address & 0x03ffffffU : address;
            usize sequenceEnd = sequences.offset + sequences.count;
            if (external) {
                range(dynamicSequences, at, 8);
                sequenceEnd = at + std::min(dynamicBufferSize, dynamicSequences.size() - at);
                const auto next = dynamicOffsets.upper_bound(at);
                if (next != dynamicOffsets.end()) {
                    sequenceEnd = std::min(sequenceEnd, *next);
                }
            } else {
                require(at >= sequences.offset && at <= sequenceEnd && sequenceEnd - at >= 8,
                        "sequence outside SEQS");
            }
            const usize trackCount = sequenceBytes[at];
            require(at + 8 + trackCount * 4 <= sequenceEnd, "track list outside sequence extent");
            const std::string sequenceName = fourcc(bytes, listAt);
            TreeDefinition tree;
            tree.name = legendsN64TreeName(group, sequenceName);
            if (!treeNames.insert(tree.name).second) {
                // e002d434 starts every load-time SLST entry, not a name lookup.
                // levels/effects intentionally has four AUTO:!+ entries, each
                // controlling a different shared texture destination.
                require(group == "!" || readU32BE(bytes, listAt) == 0x212bU,
                        "duplicate group sequence");
                tree.name += "#" + std::to_string(sequenceIndex);
                require(treeNames.insert(tree.name).second, "duplicate auto-start sequence");
            }
            const usize previousNodes = unsupportedNodes;
            const usize previousTracks = unsupportedTracks;
            const usize previousTextures = unsupportedRuntimeTextures;
            const usize previousFacing = unsupportedFacing;
            std::set<usize> visited;
            std::map<usize, std::span<const u8>> morphBindings;
            // e002dde8: BODY records hold a model/type, next sibling and child in 8-byte
            // units. Position-bearing records occupy two units; the stored X is negated.
            std::function<void(usize, s32, usize)> visit = [&](usize index, s32 parent,
                                                               usize depth) {
                while (index != 0xffffU) {
                    require(depth < 128 && remainingNodes != 0 && index < body.count / 8,
                            "invalid or excessive BODY hierarchy");
                    require(visited.insert(index).second, "cyclic or shared BODY node");
                    --remainingNodes;
                    const usize nodeAt = body.offset + index * 8;
                    const u16 flags = readU16BE(bytes, nodeAt);
                    const u16 type = flags & 0xe00U;
                    const usize sibling = readU16BE(bytes, nodeAt + 2);
                    const usize child = readU16BE(bytes, nodeAt + 4);
                    if (type == 0xe00U && (flags & 0x1ffU) == 0) {
                        // e002dde8's terminal data child belongs to its parent;
                        // its next four bytes are a BDAT pointer, not linkage.
                        const Section& data = wad.section("BDAT", 1);
                        const usize at = readU32BE(bytes, nodeAt + 2);
                        require(parent >= 0 && at >= data.offset && at < data.offset + data.count,
                                "BODY data binding outside BDAT or without parent");
                        require(morphBindings
                                    .emplace(static_cast<usize>(parent),
                                             bytes.subspan(at, data.offset + data.count - at))
                                    .second,
                                "duplicate BODY data binding");
                        return;
                    }
                    TreeNode node;
                    node.name = "NODE" + std::to_string(tree.nodes.size());
                    node.parent = parent;
                    node.type = TreeNodeType::Skeletal;
                    std::string modelName;
                    if ((type == 0 && (flags & 0x1ffU) == 0x1ffU) ||
                        (type == 0x800U && readU16BE(bytes, nodeAt + 6) != 0)) {
                        // e002dde8's sentinel resolves an exact name within the
                        // caller's model archive (e001d07c), not model slot511.
                        // The word offset is relative to seq.wad, inside BDAT.
                        const Section& names = wad.section("BDAT", 1);
                        const usize nameAt = static_cast<usize>(readU16BE(bytes, nodeAt + 6)) * 4;
                        require(nameAt >= names.offset && nameAt < names.offset + names.count,
                                "BODY model name outside BDAT");
                        usize length = 0;
                        while (length < 32 && nameAt + length < names.offset + names.count &&
                               bytes[nameAt + length] != 0) {
                            ++length;
                        }
                        require(length < 32 && nameAt + length < names.offset + names.count,
                                "unterminated BODY model name");
                        modelName.assign(std::bit_cast<const char*>(bytes.data() + nameAt), length);
                        if (!modelName.empty()) {
                            node.name = modelName;
                        }
                    }
                    if ((flags & 0x8000U) != 0) {
                        if (type == 0 || (type == 0xa00U && (flags & 0x1f8U) == 0)) {
                            // e002dde8 selects native mode1; e001e794 uses view
                            // depth for its yaw, not the ordinary upright billboard.
                            // Empty drawables later filled by property5 use it too.
                            node.objectFlags |= CameraFrame::kFacingProjectedYaw
                                                << object_flags::kFacingShift;
                        } else {
                            ++unsupportedFacing; // Group nodes use a distinct native mode2.
                        }
                    } else if ((flags & 0x4000U) != 0) {
                        // e002dde8 maps this to native mode3; e001e640 rebuilds
                        // camera orientation while retaining the authored scale.
                        node.objectFlags |= 4U << object_flags::kFacingShift;
                    }
                    if ((flags & 0x2000U) != 0) {
                        require(index + 1 < body.count / 8, "truncated BODY translation");
                        node.position =
                            Vec3{-static_cast<f32>(signed16(bytes, nodeAt + 8)),
                                 signed16(bytes, nodeAt + 10), signed16(bytes, nodeAt + 12)} /
                            64.0f;
                    }
                    if (type == 0) {
                        const usize index = flags & 0x1ffU;
                        const LegendsN64Model* model = nullptr;
                        if (index == 0x1ffU) {
                            const auto found =
                                std::find_if(models.begin(), models.end(), [&](const auto& value) {
                                    return value.name == modelName;
                                });
                            if (found != models.end()) {
                                model = &*found;
                            }
                        } else {
                            require(index < models.size(), "BODY model outside object archive");
                            model = &models[index];
                        }
                        // Missing optional skin limbs retain their transform and
                        // children, just as native e001f594(-1,...) does.
                        if (model != nullptr) {
                            node.object = model->name;
                            if (modelName.empty()) {
                                // Indexed BODY models still have native attachment names:
                                // e0027210 resolves their OBHD suffix (e001cfc0), such as
                                // HITNODE in TENT_MOAT_RDYHITNODE, before the full name.
                                node.name = model->partName.empty() ? model->name : model->partName;
                            }
                        }
                        if (model != nullptr &&
                            ((model->flags & 0x20U) != 0 || (model->flags & 0x11U) == 0x11U)) {
                            node.objectFlags |= object_flags::kSorted;
                        }
                    } else if (type != 0xa00U && type != 0x800U) {
                        ++unsupportedNodes;
                        // Control/particle nodes have another linkage interpretation. Do not
                        // follow arbitrary operand words as sibling/child pointers.
                        if (type == 0xe00U) {
                            return;
                        }
                    }
                    const s32 current = static_cast<s32>(tree.nodes.size());
                    tree.nodes.push_back(std::move(node));
                    if (child != 0xffffU) {
                        visit(child, current, depth + 1);
                    }
                    index = sibling;
                }
            };
            if ((sequenceBytes[at + 3] & 0x80U) == 0) {
                visit(readU16BE(sequenceBytes, at + 4), -1, 0);
            }
            const u16 frameCount = std::max<u16>(readU16BE(sequenceBytes, at + 6), 1);
            require(frameCount <= 4096, "sequence frame budget exceeded");
            std::map<usize, std::array<std::optional<TransformKeys>, 3>> transforms;
            std::map<usize, TextureAnimation> nodeTextures;
            std::map<usize, std::array<u8, 12>> nodeTextureProperties;
            std::map<usize, std::vector<usize>> runtimeTextureParts;
            std::map<usize, std::span<const u8>> morphTracks;
            struct ObjectKeys {
                u16 delay = 0;
                std::vector<u16> models;
            };
            std::map<usize, ObjectKeys> objectTracks;
            std::set<usize> vertexKeyNodes;
            for (usize track = 0; track < trackCount; ++track) {
                const usize trackAt = at + 8 + track * 4;
                const u8 type = sequenceBytes[trackAt] & 0x1fU;
                const usize node = sequenceBytes[trackAt + 1];
                if ((type == 9 || type == 11 || type == 13) && node < tree.nodes.size()) {
                    const usize payload =
                        at + static_cast<usize>(readU16BE(sequenceBytes, trackAt + 2)) * 4;
                    require(payload >= at + 8 + trackCount * 4,
                            "transform overlaps sequence header");
                    // sequenceEnd bounds the keys and frameCount is how many a sequence
                    // holds: the names share a stem, not a meaning.
                    // NOLINTNEXTLINE(readability-suspicious-call-argument)
                    auto keys = transformKeys(sequenceBytes, payload, sequenceEnd,
                                              sequenceBytes[trackAt], frameCount, program);
                    if (!keys || sequenceBytes[at + 1] == 0 ||
                        (30U * sequenceBytes[at + 1]) % 16U != 0) {
                        ++unsupportedTracks;
                    } else {
                        const usize channel = type == 9 ? 0 : type == 11 ? 1 : 2;
                        require(!transforms[node][channel], "duplicate node transform property");
                        transforms[node][channel] = std::move(keys);
                    }
                } else if ((type == 6 || type == 16) && node < tree.nodes.size()) {
                    const usize payload =
                        at + static_cast<usize>(readU16BE(sequenceBytes, trackAt + 2)) * 4;
                    const usize header = type == 16 ? 16 : 12;
                    require(payload >= at + 8 + trackCount * 4 && payload + header <= sequenceEnd,
                            "morph track outside sequence extent");
                    require(
                        morphTracks
                            .emplace(node, sequenceBytes.subspan(payload, sequenceEnd - payload))
                            .second,
                        "duplicate node morph property");
                    if (type == 16) {
                        vertexKeyNodes.insert(node);
                    }
                } else if (type == 7) {
                    const usize payload =
                        at + static_cast<usize>(readU16BE(sequenceBytes, trackAt + 2)) * 4;
                    require(payload >= at + 8 + trackCount * 4 && payload + 12 <= sequenceEnd,
                            "texture track outside SEQS");
                    // e0032478 advances a six-bit fractional counter by dt*30*16*64/rate.
                    // e002d434 instantiates the '!' group AND sequence ID '!+' in
                    // any group at archive load. Both run independently of item poses.
                    const u16 destination = readU16BE(sequenceBytes, payload);
                    const u16 source = readU16BE(sequenceBytes, payload + 2);
                    const u8 frames = sequenceBytes[payload + 5];
                    const u8 rate = sequenceBytes[payload + 7];
                    require(rate != 0, "invalid native texture animation");
                    // e0032478 resets a zero-frame track to phase zero; a one-frame
                    // track always clamps or wraps to zero. With zero start, both
                    // sides of the delay assign the same source. Plague uses count
                    // zero, while Dragon and Yeti use count one for static bindings.
                    const bool staticTexture = frames <= 1 && sequenceBytes[payload + 6] == 0;
                    const bool archiveLoadSequence =
                        group == "!" || readU32BE(bytes, listAt) == 0x212bU;
                    if (archiveLoadSequence && frames != 0 && node == 0xff &&
                        sequenceBytes[payload + 4] == 0xff && destination < 1024 &&
                        static_cast<usize>(source) + frames <= 1024 && rate % 16 == 0 &&
                        readU16BE(sequenceBytes, payload + 10) == 0 &&
                        (sequenceBytes[at + 2] & 5U) == 0) {
                        TextureAnimation animation;
                        animation.name = sequenceName;
                        animation.texture = destination;
                        animation.source = source;
                        animation.frames = frames;
                        animation.start = sequenceBytes[payload + 6];
                        animation.rate = rate / 16;
                        animation.flag = -1;
                        result.animation.textureAnimations.push_back(std::move(animation));
                        sharedTextureSequences.insert(sequenceName);
                    } else if (node < tree.nodes.size() && sequenceBytes[payload + 4] != 0xff &&
                               source < 1024 &&
                               (staticTexture ||
                                (bakeSequenceTextures &&
                                 static_cast<usize>(source) + std::max<usize>(frames, 1) <= 1024) ||
                                (static_cast<usize>(source) + frames <= 1024 &&
                                 rate == sequenceBytes[at + 1] && sequenceBytes[payload + 6] == 0 &&
                                 readU16BE(sequenceBytes, payload + 10) == 0 &&
                                 frameCount == frames))) {
                        // e0032478 -> e00259d0 replaces this drawable's segment13.
                        // Here texture and pose clocks agree, so the common keyed
                        // animation reproduces both once/clamp and looping sequences.
                        if (bakeSequenceTextures && frames > 1) {
                            require(sequenceBytes[at + 1] != 0 && rate % sequenceBytes[at + 1] == 0,
                                    "texture clock cannot be represented by pose samples");
                        }
                        TextureAnimation animation;
                        animation.name = tree.name + ":NODE" + std::to_string(node);
                        animation.texture = source;
                        animation.source = source;
                        animation.frames = std::max<s32>(frames, 1);
                        animation.rate = 1;
                        animation.flag = 0;
                        std::array<u8, 12> property{};
                        std::copy_n(sequenceBytes.begin() + static_cast<std::ptrdiff_t>(payload),
                                    property.size(), property.begin());
                        const auto [previous, inserted] =
                            nodeTextureProperties.emplace(node, property);
                        // Native applies every property in order. Chimera's SUPERFIREFX
                        // repeats an identical assignment on node1; collapsing that is
                        // exact, unlike silently choosing between conflicting tracks.
                        require(bakeSequenceTextures || inserted || previous->second == property,
                                "conflicting node texture properties");
                        if (bakeSequenceTextures) {
                            // e002f02c links properties in header order. Every
                            // e0032478 call assigns a texture, including before
                            // its delay, so the last property for a node wins.
                            // Yeti effects deliberately layer delayed assignments.
                            previous->second = property;
                            nodeTextures.insert_or_assign(node, std::move(animation));
                        } else {
                            nodeTextures.try_emplace(node, std::move(animation));
                        }
                    } else {
                        ++unsupportedTracks;
                    }
                } else if (type == 15) {
                    const usize payload =
                        at + static_cast<usize>(readU16BE(sequenceBytes, trackAt + 2)) * 4;
                    require(payload >= at + 8 + trackCount * 4 && payload + 4 <= sequenceEnd,
                            "shared animation reference outside SEQS");
                    if (!sharedTextureSequences.contains(fourcc(sequenceBytes, payload))) {
                        ++unsupportedTracks;
                    }
                } else if (type == 5 && node < tree.nodes.size()) {
                    const usize payload =
                        at + static_cast<usize>(readU16BE(sequenceBytes, trackAt + 2)) * 4;
                    require(payload >= at + 8 + trackCount * 4 && payload + 4 <= sequenceEnd,
                            "object track outside SEQS");
                    const usize countFrames = sequenceBytes[payload];
                    const bool delayed = (sequenceBytes[payload + 1] & 1U) != 0;
                    require(countFrames != 0 &&
                                payload + 2 + (countFrames + (delayed ? 1 : 0)) * 2 <= sequenceEnd,
                            "invalid object frame table");
                    const usize model = readU16BE(sequenceBytes, payload + 2 + (delayed ? 2 : 0));
                    ObjectKeys keys;
                    keys.delay = delayed ? readU16BE(sequenceBytes, payload + 2) : 0;
                    for (usize frame = 0; frame < countFrames; ++frame) {
                        const u16 slot =
                            readU16BE(sequenceBytes, payload + 2 + (frame + (delayed ? 1 : 0)) * 2);
                        require(slot == 0xffff || slot < models.size(),
                                "object frame outside model archive");
                        keys.models.push_back(slot);
                    }
                    require(objectTracks.emplace(node, std::move(keys)).second,
                            "duplicate object frame property");
                    tree.nodes[node].object =
                        model == 0xffff || (delayed && readU16BE(sequenceBytes, payload + 2) != 0)
                            ? std::string{}
                            : models[model].name;
                    tree.nodes[node].objectFlags &= ~object_flags::kSorted;
                    if (model != 0xffff && ((models[model].flags & 0x20U) != 0 ||
                                            (models[model].flags & 0x11U) == 0x11U)) {
                        // e001e794's deferred eligibility, not a blanket depth-write ban.
                        tree.nodes[node].objectFlags |= object_flags::kSorted;
                    }
                } else {
                    ++unsupportedTracks;
                }
            }
            for (const auto& [nodeIndex, keys] : objectTracks) {
                auto& node = tree.nodes[nodeIndex];
                std::optional<bool> sorted;
                bool supported = !morphTracks.contains(nodeIndex);
                for (const auto slot : keys.models) {
                    if (slot == 0xffff) {
                        continue;
                    }
                    const auto& model = models[slot];
                    const bool frameSorted =
                        (model.flags & 0x20U) != 0 || (model.flags & 0x11U) == 0x11U;
                    supported &= !sorted || *sorted == frameSorted;
                    sorted = frameSorted;
                    supported &= std::none_of(
                        model.mesh.parts.begin(), model.mesh.parts.end(), [](const auto& part) {
                            return part.texture == LegendsN64Model::kRuntimeTexture;
                        });
                }
                if (!supported) {
                    // Combined morph/runtime-texture and per-frame draw-state changes
                    // need another binding path, not a guessed global material override.
                    ++unsupportedTracks;
                    continue;
                }
                // e0031e74 selects the integer sequence frame after an optional delay.
                // A FFFF slot is invisible; a short looping table holds its last slot,
                // while a short one-shot table disappears. Expand those bounds into
                // the common clock, retaining discrete swaps rather than vertex morphs.
                const std::string name = tree.name + ":NODE" + std::to_string(nodeIndex) + ":SWAP";
                const Mesh empty;
                for (usize frame = 0; frame < frameCount; ++frame) {
                    u16 slot = 0xffff;
                    if (frame >= keys.delay) {
                        const usize index = frame - keys.delay;
                        if (index < keys.models.size()) {
                            slot = keys.models[index];
                        } else if ((sequenceBytes[at + 2] & 1U) == 0) {
                            slot = keys.models.back();
                        }
                    }
                    const auto* model = slot == 0xffff ? nullptr : &models[slot];
                    const auto& mesh = model != nullptr ? model->mesh : empty;
                    require(mesh.vertices.size() <= remainingVariantVertices,
                            "object-frame variant vertex budget exceeded");
                    const usize bytes = legendsN64VariantGeometryBytes(
                        mesh, model != nullptr ? model->vertexSources.size() : 0, 1,
                        remainingVariantBytes);
                    remainingVariantVertices -= mesh.vertices.size();
                    remainingVariantBytes -= bytes;
                    LegendsN64Model variant = model != nullptr ? *model : LegendsN64Model{};
                    variant.name = name + std::to_string(frame);
                    result.modelVariants.push_back(std::move(variant));
                }
                node.object.clear();
                node.objectFrames.push_back({name + "0", 0, static_cast<s16>(frameCount), false});
                node.type = TreeNodeType::Object;
                node.objectFlags &= ~object_flags::kSorted;
                if (sorted.value_or(false)) {
                    node.objectFlags |= object_flags::kSorted;
                }
            }
            for (usize nodeIndex = 0; nodeIndex < tree.nodes.size(); ++nodeIndex) {
                auto& node = tree.nodes[nodeIndex];
                const auto model =
                    std::find_if(models.begin(), models.end(),
                                 [&](const auto& value) { return value.name == node.object; });
                if (model == models.end() ||
                    std::none_of(model->mesh.parts.begin(), model->mesh.parts.end(),
                                 [](const auto& part) {
                                     return part.texture == LegendsN64Model::kRuntimeTexture;
                                 })) {
                    continue;
                }
                const auto animation = nodeTextures.find(nodeIndex);
                if (animation == nodeTextures.end()) {
                    ++unsupportedRuntimeTextures;
                    node.object.clear();
                    continue;
                }
                require(model->mesh.vertices.size() <= remainingVariantVertices,
                        "runtime-texture variant budget exceeded");
                const usize bytes = legendsN64VariantGeometryBytes(
                    model->mesh, model->vertexSources.size(), 1, remainingVariantBytes);
                remainingVariantVertices -= model->mesh.vertices.size();
                remainingVariantBytes -= bytes;
                LegendsN64Model variant = *model;
                variant.name = animation->second.name + ":" + model->name;
                for (usize partIndex = 0; partIndex < variant.mesh.parts.size(); ++partIndex) {
                    auto& part = variant.mesh.parts[partIndex];
                    if (part.texture == LegendsN64Model::kRuntimeTexture) {
                        runtimeTextureParts[nodeIndex].push_back(partIndex);
                        part.texture = static_cast<u32>(animation->second.source);
                    }
                }
                node.object = variant.name;
                if (!bakeSequenceTextures) {
                    node.textureAnimation =
                        static_cast<s32>(result.animation.textureAnimations.size());
                    result.animation.textureAnimations.push_back(std::move(animation->second));
                }
                result.modelVariants.push_back(std::move(variant));
            }
            std::set<usize> hiddenSingleFrameNodes;
            for (const auto& [nodeIndex, payload] : morphTracks) {
                auto& node = tree.nodes[nodeIndex];
                const auto binding = morphBindings.find(nodeIndex);
                const auto findModel = [&](const auto& list) -> const LegendsN64Model* {
                    const auto found =
                        std::find_if(list.begin(), list.end(),
                                     [&](const auto& value) { return value.name == node.object; });
                    return found == list.end() ? nullptr : &*found;
                };
                const auto* model = findModel(models);
                if (model == nullptr) {
                    model = findModel(result.modelVariants);
                }
                if (binding == morphBindings.end() || model == nullptr) {
                    ++unsupportedTracks;
                    continue;
                }
                const bool vertexKeys = vertexKeyNodes.contains(nodeIndex);
                const usize morphFrames =
                    vertexKeys ? readU16BE(payload, 2) : readU16BE(payload, 0) & 0x7fffU;
                if (morphFrames != 0 && morphFrames != frameCount && morphFrames != 1) {
                    ++unsupportedTracks;
                    continue; // No allocation/charge for a run the common clock cannot represent.
                }
                // e00321ec hides a finished property16 run on once sequences
                // unless hold-after is set. A common single-model run holds
                // forever, so represent that edge with an explicit empty second
                // model rather than leaving the unmorphed source visible.
                const bool hideSingleFrame = vertexKeys && morphFrames == 1 && frameCount > 1 &&
                                             (sequenceBytes[at + 2] & 1U) != 0 &&
                                             (payload[0] & 8U) == 0;
                const auto decode = vertexKeys ? decodeLegendsN64VertexKeys : decodeLegendsN64Morph;
                auto frames = decode(payload, binding->second, model->mesh, model->vertexSources,
                                     remainingVariantVertices, remainingVariantBytes);
                if (!frames) {
                    ++unsupportedTracks;
                    continue;
                }
                // Appending variants can invalidate their source. Retain only
                // metadata/identity, not another full mesh and index-buffer copy.
                const u32 sourceFlags = model->flags;
                const auto sourceVertices = model->vertexSources;
                const auto sourceRamps = model->colorRamps;
                const auto sourceDepth = model->depthFlags;
                const bool sourceMixedDepth = model->mixedDepthFlags;
                const std::string name = tree.name + ":NODE" + std::to_string(nodeIndex) + ":MORPH";
                for (usize frame = 0; frame < frames->size(); ++frame) {
                    LegendsN64Model variant;
                    variant.name = name + std::to_string(frame);
                    variant.flags = sourceFlags;
                    variant.mesh = std::move((*frames)[frame]);
                    variant.vertexSources = sourceVertices;
                    variant.colorRamps = sourceRamps;
                    // Morph keys replace positions, not the source display
                    // list's material state (including explicit Z_CMP/Z_UPD).
                    variant.depthFlags = sourceDepth;
                    variant.mixedDepthFlags = sourceMixedDepth;
                    result.modelVariants.push_back(std::move(variant));
                }
                if (hideSingleFrame) {
                    hiddenSingleFrameNodes.insert(nodeIndex);
                    LegendsN64Model hidden;
                    hidden.name = name + "1";
                    hidden.flags = sourceFlags;
                    result.modelVariants.push_back(std::move(hidden));
                }
                node.objectFrames.push_back(
                    {name + "0", 0, static_cast<s16>(frames->size() + (hideSingleFrame ? 1 : 0)),
                     !hideSingleFrame});
                node.type = TreeNodeType::Object;
            }
            if (bakeSequenceTextures) {
                // A common tree has one texture-animation binding per BODY node,
                // but native groups replace it independently for each sequence.
                // Fold only these per-drawable bindings into sequence-local model
                // runs before combining groups. Shared archive cycles stay shared.
                for (const auto& [nodeIndex, parts] : runtimeTextureParts) {
                    auto& node = tree.nodes[nodeIndex];
                    const auto& property = nodeTextureProperties.at(nodeIndex);
                    const usize count = property[5];
                    const usize start = property[6];
                    const usize rate = property[7];
                    const usize delay = readU16BE(property, 10);
                    const usize source = readU16BE(property, 2);
                    const bool reverse = (sequenceBytes[at + 2] & 4U) != 0;
                    const bool once = (sequenceBytes[at + 2] & 1U) != 0;
                    const auto slotAt = [&](usize clockFrame) {
                        if (clockFrame < delay) {
                            return source;
                        }
                        // e0032478: phase has six fractional bits, is held at zero
                        // through the delay, and advances independently of the pose
                        // clock. Decode at each authored pose frame; the renderer
                        // keeps texture swaps discrete while interpolating geometry.
                        usize phase = (clockFrame - delay) * sequenceBytes[at + 1] * 64 / rate;
                        if (count == 0) {
                            return source + start;
                        }
                        phase = once ? std::min(phase, count * 64 - 1) : phase % (count * 64);
                        usize index = phase / 64;
                        if (reverse) {
                            index = count - index - 1;
                        }
                        return source + (index + start) % count;
                    };
                    require(source + std::max(count, start + 1) <= 1024,
                            "baked texture range escapes native archive");
                    const bool held = count <= 1 && start == 0;
                    if (held && node.objectFrames.empty()) {
                        // One immutable mesh is sufficient for a held source.
                        node.objectFrames.push_back({node.object, 0, 1, false});
                        node.object.clear();
                        node.type = TreeNodeType::Object;
                        continue;
                    }
                    const auto findModel = [&](std::string_view name) -> const LegendsN64Model* {
                        for (const auto& model : result.modelVariants) {
                            if (model.name == name) {
                                return &model;
                            }
                        }
                        for (const auto& model : models) {
                            if (model.name == name) {
                                return &model;
                            }
                        }
                        return nullptr;
                    };
                    std::vector<std::string> inputNames;
                    bool interpolate = false;
                    if (node.objectFrames.empty()) {
                        inputNames.push_back(node.object);
                    } else {
                        const auto& run = node.objectFrames.front();
                        interpolate = run.interpolate;
                        const auto first = std::find_if(
                            result.modelVariants.begin(), result.modelVariants.end(),
                            [&](const auto& model) { return model.name == run.object; });
                        require(first != result.modelVariants.end() && run.start == 0 &&
                                    run.frames > 0 &&
                                    static_cast<usize>(run.frames) <=
                                        static_cast<usize>(result.modelVariants.end() - first),
                                "baked texture model run is unavailable");
                        for (s32 i = 0; i < run.frames; ++i) {
                            inputNames.push_back((first + i)->name);
                        }
                    }
                    const bool hiddenTail = hiddenSingleFrameNodes.contains(nodeIndex);
                    require(inputNames.size() == 1 || inputNames.size() == frameCount ||
                                (hiddenTail && inputNames.size() == 2),
                            "baked texture and morph clocks disagree");
                    const std::string base =
                        tree.name + ":NODE" + std::to_string(nodeIndex) + ":TEXTURE";
                    for (usize frame = 0; frame < frameCount; ++frame) {
                        const usize sourceFrame = inputNames.size() == 1 ? 0
                                                  : hiddenTail           ? std::min<usize>(frame, 1)
                                                                         : frame;
                        const auto* input = findModel(inputNames[sourceFrame]);
                        require(input != nullptr, "baked texture source model is unavailable");
                        require(result.modelVariants.size() < 65536 &&
                                    input->mesh.vertices.size() <= remainingVariantVertices,
                                "baked texture variant budget exceeded");
                        const usize bytes = legendsN64VariantGeometryBytes(
                            input->mesh, input->vertexSources.size(), 1, remainingVariantBytes);
                        remainingVariantVertices -= input->mesh.vertices.size();
                        remainingVariantBytes -= bytes;
                        LegendsN64Model variant = *input;
                        variant.name = base + std::to_string(frame);
                        const usize clockFrame = reverse ? frameCount - frame - 1 : frame;
                        for (const usize part : parts) {
                            // A generated hidden tail has no geometry to texture.
                            // Keep its visibility when combining property7/16.
                            if (hiddenTail && frame > 0)
                                continue;
                            require(part < variant.mesh.parts.size(),
                                    "baked texture part is absent from morph");
                            variant.mesh.parts[part].texture = static_cast<u32>(slotAt(clockFrame));
                        }
                        result.modelVariants.push_back(std::move(variant));
                    }
                    node.object.clear();
                    node.objectFrames = {
                        {base + "0", 0, static_cast<s16>(frameCount), interpolate}};
                    node.type = TreeNodeType::Object;
                }
            }
            for (const auto& [node, binding] : morphBindings) {
                if (!morphTracks.contains(node)) {
                    ++unsupportedNodes;
                }
            }
            TreeSequence pose;
            pose.name = sequenceName;
            pose.frameCount = static_cast<s16>(frameCount);
            // e002f368 stores speed=16/headerByte; e0042614 supplies dt*30 to
            // e00327e4. Thus seconds/frame=headerByte/480, while the common
            // player stores period*900, not frames/second.
            pose.frameRate = sequenceBytes[at + 1] == 0
                                 ? s16{30}
                                 : static_cast<s16>(30U * sequenceBytes[at + 1] / 16U);
            pose.repeats = (sequenceBytes[at + 2] & 1U) == 0;
            // Common bit0 reverses effect frames only; transform keys below are
            // already reversed explicitly to retain native Euler interpolation.
            pose.flags = (sequenceBytes[at + 2] & 4U) != 0 ? 1 : 0;
            for (const auto& [node, properties] : transforms) {
                NodeTrack converted;
                converted.node = static_cast<u32>(node);
                converted.flags = NodeTrack::kLinearAngles;
                for (usize channel = 0; channel < properties.size(); ++channel) {
                    if (properties[channel]) {
                        converted.flags |= static_cast<u16>(7U << (channel * 4));
                    }
                }
                if (properties[0] && (properties[0]->flags & 0x20U) != 0) {
                    converted.flags |= NodeTrack::kYawPitchRoll;
                }
                const usize valueCount = static_cast<usize>(frameCount) * converted.channelCount();
                require(valueCount <= remainingValues, "transform sample budget exceeded");
                remainingValues -= valueCount;
                converted.frames.reserve(frameCount);
                converted.values.reserve(valueCount);
                for (u16 frame = 0; frame < frameCount; ++frame) {
                    converted.frames.push_back(frame);
                    const u16 source =
                        (sequenceBytes[at + 2] & 4U) != 0 ? frameCount - 1U - frame : frame;
                    for (usize channel = 0; channel < properties.size(); ++channel) {
                        if (!properties[channel]) {
                            continue;
                        }
                        Vec3 value = sampleTransform(*properties[channel], source);
                        if (channel == 0) {
                            value.y = -value.y;
                            value.z = -value.z;
                        } else if (channel == 1) {
                            value.x = -value.x;
                        } else if ((properties[channel]->flags & 15U) == 3) {
                            value = Vec3{value.x};
                        }
                        converted.values.insert(converted.values.end(),
                                                {value.x, value.y, value.z});
                    }
                }
                if (properties[1]) {
                    // e0031a20 replaces the BODY offset; common tracks add to their rest.
                    tree.nodes[node].position = Vec3{0.0f};
                }
                pose.tracks.push_back(std::move(converted));
            }
            tree.sequences.push_back(std::move(pose));
            usize mixedDepthNodes = 0;
            for (auto& node : tree.nodes) {
                bool seen = false;
                bool mixed = false;
                std::optional<u32> depth;
                const auto observe = [&](const LegendsN64Model& model) {
                    if (model.mesh.parts.empty())
                        return;
                    mixed |= model.mixedDepthFlags;
                    if (!seen) {
                        depth = model.depthFlags;
                        seen = true;
                    } else {
                        mixed |= depth != model.depthFlags;
                    }
                };
                const auto run = [&](std::string_view name, usize count) {
                    if (name.empty() || count == 0)
                        return;
                    const auto visitModels = [&](std::span<const LegendsN64Model> values) {
                        const auto first =
                            std::find_if(values.begin(), values.end(),
                                         [&](const auto& model) { return model.name == name; });
                        if (first == values.end())
                            return false;
                        require(count <= static_cast<usize>(values.end() - first),
                                "depth-state object run outside archive");
                        for (usize i = 0; i < count; ++i)
                            observe(first[static_cast<std::ptrdiff_t>(i)]);
                        return true;
                    };
                    if (!visitModels(models))
                        visitModels(result.modelVariants);
                };
                run(node.object, 1);
                for (const auto& frames : node.objectFrames)
                    run(frames.object, static_cast<usize>(std::max<s16>(frames.frames, 0)));
                if (mixed) {
                    ++mixedDepthNodes;
                } else if (depth) {
                    node.objectFlags |= *depth;
                }
            }
            const auto warning = [&](usize count, std::string_view reason) {
                if (count != 0) {
                    result.warnings.push_back(tree.name + ": " + std::to_string(count) + " " +
                                              std::string(reason));
                }
            };
            warning(unsupportedNodes - previousNodes,
                    "native particle/control/body-reference nodes are not decoded.");
            warning(unsupportedTracks - previousTracks,
                    "native animation tracks remain at their authored body pose.");
            warning(unsupportedRuntimeTextures - previousTextures,
                    "nodes need unsupported per-drawable texture bindings and are omitted.");
            warning(unsupportedFacing - previousFacing,
                    "native yaw-only facing nodes retain their authored orientation.");
            warning(mixedDepthNodes,
                    "nodes vary native depth state between draw parts or object frames.");
            result.animation.trees.push_back(std::move(tree));
        }
    }
    if (externalSequences != 0) {
        result.warnings.push_back(std::to_string(externalSequences) +
                                  " externally streamed native sequences are unavailable.");
    }
    return result;
}

std::vector<std::string> legendsN64FixtureArchives(std::string_view levelName,
                                                   std::span<const LegendsN64Fixture> fixtures,
                                                   u8 activationThreshold) {
    const std::string name = toLowerAscii(levelName);
    const usize digit = name.find_first_of("0123456789");
    if (digit == std::string::npos || digit == 0 ||
        !std::all_of(name.begin() + static_cast<std::ptrdiff_t>(digit), name.end(),
                     [](char value) { return value >= '0' && value <= '9'; })) {
        return {};
    }
    const std::string realm = name.substr(0, digit);
    // e006c360 maps directory prefixes to realm IDs; e006ab40 selects these common
    // archives, and e0062dfc loads the final realm-specific animation archive.
    std::vector<std::string_view> common;
    std::string realmArchive = realm;
    if (realm == "mount") {
        common = {"barrel", "doorslat", "schest", "gchest", "switch", "telep", "obelisk"};
    } else if (realm == "ice") {
        common = {"barrel", "saw", "switch", "telep", "obelisk"};
    } else if (realm == "town") {
        common = {"barrel", "doorslat", "gatevine", "schest", "gchest",
                  "saw",    "switch",   "telep",    "obelisk"};
    } else if (realm == "battle") {
        common = {"barrel",  "doorgrate", "doorslat", "schest", "gchest",
                  "cspikes", "saw",       "switch",   "telep"};
    } else if (realm == "hell" || realm == "boss") {
        common = {"barrel", "doorslat", "schest", "gchest", "switch", "telep"};
        realmArchive = "boss";
    } else if (realm == "tower" || realm == "secret") {
        common = {"telep"};
        realmArchive = realm == "tower" ? "tower" : "castle";
    } else if (realm == "castle") {
        common = {"barrel",  "doorgrate", "doorslat", "gatevine", "schest", "gchest",
                  "cspikes", "saw",       "switch",   "telep",    "obelisk"};
    } else {
        return {};
    }
    // e006b110 loads generic/hires, then levels/effects before realm fixtures.
    // Effects' AUTO:!+ owns the shared FIRE/TORCH animation destinations used
    // by external TXHD slots in the world archive, not just particle models.
    std::vector<std::string> result{"itemanim/generic", "itemanim/hires", "levels/effects"};
    // e006ab40 loads this for Castle's native ordinals 0 and 1. The
    // e006c468 level table maps those to directory suffixes "1" and "6",
    // respectively; directory numbers are not progression ordinals.
    if (name == "castle1" || name == "castle6") {
        result.emplace_back("items/castle/tent");
    }
    result.emplace_back("items/c/redbar");
    result.emplace_back("items/c/grebar");
    for (const auto entry : common) {
        result.push_back("items/c/" + std::string(entry));
    }
    result.push_back("items/" + realmArchive);
    for (const auto& fixture : fixtures) {
        if (fixture.kind == LegendsN64FixtureKind::Powerup &&
            fixture.minimumPlayers <= activationThreshold && !fixture.archive.empty() &&
            std::find(result.begin(), result.end(), fixture.archive) == result.end()) {
            result.push_back(fixture.archive);
        }
    }
    return result;
}

} // namespace gdl::formats
